/*
    SPDX-FileCopyrightText: 2026 Nocturne

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "fileops.h"
#include "filebrowser.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QtConcurrent>

#include <atomic>
#include <unistd.h>

namespace EkosRemote
{
struct FileOpsJob
{
    int id = 0;
    QString op;
    QStringList paths;
    QString targetRoot;      // Nocturne folder of the target drive; empty for delete
    QStringList drives;      // ids of the drives the job touches
    std::atomic<bool> cancel{false};
    std::atomic<qint64> bytesDone{0};
    std::atomic<qint64> bytesTotal{0};
    std::atomic<int> filesDone{0};
    std::atomic<int> filesTotal{0};
    QFutureWatcher<void> watcher;

    mutable QMutex lock;     // guards the members below
    QString state = QStringLiteral("running");
    QString current;
    QString error;
    QStringList failed;
    qint64 needed = 0;
    qint64 free = 0;

    void fail(const QString &file, const QString &why)
    {
        QMutexLocker l(&lock);
        failed << QStringLiteral("%1: %2").arg(file, why);
    }
    void setCurrent(const QString &c)
    {
        QMutexLocker l(&lock);
        current = c;
    }
};

namespace
{
constexpr qint64 ChunkBytes = 4 * 1024 * 1024;
constexpr qint64 SpaceMargin = 200LL * 1024 * 1024;
constexpr int MaxPaths = 200;
const QString RootName = QStringLiteral("Nocturne");

/** The Nocturne folder holding @p path: ~/Nocturne, or /mnt|/media/<x>/Nocturne. Empty if none. */
QString rootOf(const QString &path)
{
    const QString canon = FileBrowser::canonical(path);
    const QString sd = FileBrowser::canonical(FileBrowser::sdRoot());
    if (canon == sd || canon.startsWith(sd + '/'))
        return sd;
    static const QRegularExpression usb(QStringLiteral("^((/mnt|/media)/[^/]+/") + RootName + QStringLiteral(")(/|$)"));
    const auto match = usb.match(canon);
    return match.hasMatch() ? match.captured(1) : QString();
}

/** "sd", or the mount point, for a Nocturne root. */
QString driveIdOf(const QString &root)
{
    return root == FileBrowser::canonical(FileBrowser::sdRoot()) ? QStringLiteral("sd")
           : root.chopped(RootName.size() + 1);
}

/** Whether @p path is the running capture's folder, holds it, or is inside it: all of those are locked. */
bool holdsActive(const QString &path, const QString &activeDir)
{
    if (activeDir.isEmpty())
        return false;
    const QString canon = FileBrowser::canonical(path);
    return activeDir == canon || activeDir.startsWith(canon + '/') || canon.startsWith(activeDir + '/');
}

QString sha1Of(const QString &path, const std::atomic<bool> &cancel)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha1);
    while (!file.atEnd())
    {
        if (cancel.load())
            return QString();
        hash.addData(file.read(ChunkBytes));
    }
    return QString::fromLatin1(hash.result().toHex());
}

struct Item
{
    QString src;
    QString dst;
    qint64 size = 0;
};

/** Everything under the sources: each file with its target path, the folders to create, empty sources. */
void enumerate(const QStringList &paths, const QString &targetRoot, QList<Item> &files, QStringList &dirs,
               QStringList &emptySources)
{
    for (const QString &source : paths)
    {
        const QString canon = FileBrowser::canonical(source);
        const QString root = rootOf(canon);
        const QString rel = canon.mid(root.size() + 1);
        const QFileInfo info(canon);
        if (info.isFile())
        {
            files.append({canon, targetRoot + '/' + rel, info.size()});
            continue;
        }
        if (!targetRoot.isEmpty())
            dirs << targetRoot + '/' + rel;
        QDirIterator it(canon, QDir::AllEntries | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        int found = 0;
        while (it.hasNext())
        {
            it.next();
            const QString entryRel = it.filePath().mid(root.size() + 1);
            if (it.fileInfo().isDir())
            {
                if (!targetRoot.isEmpty())
                    dirs << targetRoot + '/' + entryRel;
            }
            else
            {
                files.append({it.filePath(), targetRoot + '/' + entryRel, it.fileInfo().size()});
                ++found;
            }
        }
        if (found == 0)
            emptySources << canon;
    }
}

enum class Copied { Ok, Exists, Cancelled, Failed };

/** One file to `<dst>.part`, renamed when complete. Sizes are compared; the modified time carries over. */
Copied copyFile(const Item &item, FileOpsJob &job, QString &why)
{
    if (QFileInfo::exists(item.dst))
    {
        why = QStringLiteral("exists");
        return Copied::Exists;
    }
    QFile in(item.src);
    if (!in.open(QIODevice::ReadOnly))
    {
        why = QStringLiteral("cannot read the source");
        return Copied::Failed;
    }
    if (!QDir().mkpath(QFileInfo(item.dst).absolutePath()))
    {
        why = QStringLiteral("cannot create the folder");
        return Copied::Failed;
    }

    const QString part = item.dst + QStringLiteral(".part");
    QFile out(part);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        why = QStringLiteral("cannot write to the target");
        return Copied::Failed;
    }

    qint64 written = 0;
    while (!in.atEnd())
    {
        if (job.cancel.load())
        {
            out.close();
            QFile::remove(part);
            return Copied::Cancelled;
        }
        const QByteArray chunk = in.read(ChunkBytes);
        if (chunk.isEmpty() && !in.atEnd())
        {
            out.close();
            QFile::remove(part);
            why = QStringLiteral("read error");
            return Copied::Failed;
        }
        if (out.write(chunk) != chunk.size())
        {
            out.close();
            QFile::remove(part);
            why = QStringLiteral("write error (drive full or removed)");
            return Copied::Failed;
        }
        written += chunk.size();
        job.bytesDone += chunk.size();
    }

    out.flush();
    ::fsync(out.handle());
    out.setFileTime(QFileInfo(item.src).lastModified(), QFileDevice::FileModificationTime);
    out.close();

    if (written != in.size() || QFileInfo(part).size() != in.size())
    {
        QFile::remove(part);
        why = QStringLiteral("size differs after copy");
        return Copied::Failed;
    }
    if (!QFile::rename(part, item.dst))
    {
        QFile::remove(part);
        why = QStringLiteral("cannot finish the file");
        return Copied::Failed;
    }
    return Copied::Ok;
}

/** Removes empty folders under (and including) @p root, deepest first. */
void pruneEmptyDirs(const QString &root)
{
    QStringList dirs;
    QDirIterator it(root, QDir::Dirs | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        dirs << it.next();
    std::sort(dirs.begin(), dirs.end(), [](const QString & a, const QString & b)
    {
        return a.size() > b.size();
    });
    for (const QString &dir : dirs)
        QDir().rmdir(dir);
    QDir().rmdir(root);
}

void runCopyOrMove(FileOpsJob &job)
{
    const bool move = job.op == QLatin1String("move");

    QList<Item> files;
    QStringList dirs, emptySources;
    enumerate(job.paths, job.targetRoot, files, dirs, emptySources);

    qint64 total = 0;
    for (const Item &f : files)
        total += f.size;
    job.bytesTotal = total;
    job.filesTotal = files.size();

    // The target may not have a Nocturne folder yet; space is checked on the nearest existing ancestor.
    QString probe = job.targetRoot;
    while (!QFileInfo::exists(probe) && probe.contains('/'))
        probe = probe.left(probe.lastIndexOf('/'));
    const qint64 free = QStorageInfo(probe).bytesAvailable();
    if (free < total + SpaceMargin)
    {
        QMutexLocker l(&job.lock);
        job.state = QStringLiteral("failed");
        job.error = QStringLiteral("no_space");
        job.needed = total;
        job.free = free;
        return;
    }

    for (const QString &dir : dirs)
        QDir().mkpath(dir);

    for (const Item &item : files)
    {
        if (job.cancel.load())
            break;
        job.setCurrent(QFileInfo(item.src).fileName());

        QString why;
        const Copied result = copyFile(item, job, why);
        if (result == Copied::Cancelled)
            break;
        if (result != Copied::Ok)
        {
            job.fail(QFileInfo(item.src).fileName(), why);
            ++job.filesDone;
            continue;
        }

        if (move)
        {
            // The source goes only after its copy reads back identical.
            job.setCurrent(QStringLiteral("verifying ") + QFileInfo(item.src).fileName());
            const QString a = sha1Of(item.src, job.cancel);
            const QString b = job.cancel.load() ? QString() : sha1Of(item.dst, job.cancel);
            if (job.cancel.load())
            {
                QFile::remove(item.dst);
                break;
            }
            if (a.isEmpty() || a != b)
            {
                QFile::remove(item.dst);
                job.fail(QFileInfo(item.src).fileName(), QStringLiteral("checksum differs after copy, source kept"));
                ++job.filesDone;
                continue;
            }
            if (!QFile::remove(item.src))
                job.fail(QFileInfo(item.src).fileName(), QStringLiteral("copied, but the source could not be removed"));
        }
        ++job.filesDone;
    }

    if (move && !job.cancel.load())
    {
        for (const QString &source : job.paths)
        {
            const QString canon = FileBrowser::canonical(source);
            if (QFileInfo(canon).isDir())
                pruneEmptyDirs(canon);   // only empty folders go: a file that failed keeps its folder
        }
    }

    QMutexLocker l(&job.lock);
    if (job.cancel.load())
        job.state = QStringLiteral("cancelled");
    else
        job.state = job.failed.isEmpty() ? QStringLiteral("done") : QStringLiteral("failed");
}

void runDelete(FileOpsJob &job)
{
    QList<Item> files;
    QStringList dirs, emptySources;
    enumerate(job.paths, QString(), files, dirs, emptySources);

    qint64 total = 0;
    for (const Item &f : files)
        total += f.size;
    job.bytesTotal = total;
    job.filesTotal = files.size();

    for (const Item &item : files)
    {
        if (job.cancel.load())
            break;
        job.setCurrent(QFileInfo(item.src).fileName());
        if (QFile::remove(item.src))
            job.bytesDone += item.size;
        else
            job.fail(QFileInfo(item.src).fileName(), QStringLiteral("could not be removed"));
        ++job.filesDone;
    }

    if (!job.cancel.load())
    {
        for (const QString &source : job.paths)
        {
            const QString canon = FileBrowser::canonical(source);
            if (QFileInfo(canon).isDir())
                pruneEmptyDirs(canon);
        }
    }

    QMutexLocker l(&job.lock);
    if (job.cancel.load())
        job.state = QStringLiteral("cancelled");
    else
        job.state = job.failed.isEmpty() ? QStringLiteral("done") : QStringLiteral("failed");
}
}

FileOps::FileOps(QObject *parent) : QObject(parent)
{
    m_lookup = [](const QString & id)
    {
        for (const QJsonValue &v : FileBrowser::listDrives())
        {
            const QJsonObject drive = v.toObject();
            if (drive["id"].toString() == id)
                return drive["path"].toString();
        }
        return QString();
    };

    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, [this]()
    {
        if (m_job)
            Q_EMIT progress(snapshot());
    });
}

void FileOps::setDriveLookup(std::function<QString(const QString &)> lookup)
{
    m_lookup = std::move(lookup);
}

bool FileOps::busy() const
{
    return m_job && m_job->watcher.isRunning();
}

QStringList FileOps::drivesInUse() const
{
    return busy() ? m_job->drives : QStringList();
}

QJsonObject FileOps::snapshot() const
{
    QMutexLocker l(&m_job->lock);
    QJsonArray failed;
    for (const QString &f : m_job->failed)
        failed.append(f);
    QJsonObject o
    {
        {"id", m_job->id},
        {"op", m_job->op},
        {"state", m_job->state},
        {"filesDone", m_job->filesDone.load()},
        {"filesTotal", m_job->filesTotal.load()},
        {"bytesDone", static_cast<double>(m_job->bytesDone.load())},
        {"bytesTotal", static_cast<double>(m_job->bytesTotal.load())},
        {"current", m_job->current},
        {"failed", failed}
    };
    if (!m_job->error.isEmpty())
        o["error"] = m_job->error;
    if (m_job->needed > 0)
    {
        o["needed"] = static_cast<double>(m_job->needed);
        o["free"] = static_cast<double>(m_job->free);
    }
    return o;
}

void FileOps::finish()
{
    m_timer.stop();
    Q_EMIT progress(snapshot());
}

QJsonObject FileOps::start(const QString &op, const QStringList &paths, const QString &toDrive, const QString &activeDir)
{
    if (busy())
        return {{"error", "busy"}};
    if ((op != "delete" && op != "copy" && op != "move") || paths.isEmpty() || paths.size() > MaxPaths)
        return {{"error", "bad_request"}};

    auto job = QSharedPointer<FileOpsJob>::create();
    job->op = op;
    job->paths = paths;

    QStringList sourceRoots;
    for (const QString &path : paths)
    {
        if (!FileBrowser::isAllowedPath(path, false))
            return {{"error", "not_allowed"}, {"path", path}};
        const QFileInfo info(path);
        if (!info.exists() || info.isSymLink())
            return {{"error", "not_found"}, {"path", path}};
        if (holdsActive(path, activeDir))
            return {{"error", "in_use"}, {"path", path}};
        const QString root = rootOf(path);
        if (root.isEmpty())
            return {{"error", "not_allowed"}, {"path", path}};
        if (!sourceRoots.contains(root))
            sourceRoots << root;
    }
    for (const QString &root : sourceRoots)
        job->drives << driveIdOf(root);

    if (op != "delete")
    {
        const QString target = m_lookup(toDrive);
        if (target.isEmpty())
            return {{"error", "no_such_drive"}};
        // Allowed even when the folder is not there yet: it is created by the first file.
        if (!FileBrowser::isAllowedPath(target))
            return {{"error", "not_allowed"}, {"path", target}};
        if (sourceRoots.contains(FileBrowser::canonical(target)) || job->drives.contains(toDrive))
            return {{"error", "same_drive"}};
        job->targetRoot = target;
        job->drives << toDrive;
    }

    job->id = m_nextId++;
    m_job = job;
    connect(&job->watcher, &QFutureWatcher<void>::finished, this, &FileOps::finish);
    job->watcher.setFuture(QtConcurrent::run([job]()
    {
        if (job->op == QLatin1String("delete"))
            runDelete(*job);
        else
            runCopyOrMove(*job);
    }));
    m_timer.start();
    return {{"id", job->id}, {"op", op}};
}

QJsonObject FileOps::cancel(int id)
{
    if (!busy() || m_job->id != id)
        return {{"error", "not_running"}, {"id", id}};
    m_job->cancel = true;
    return {{"id", id}, {"ok", true}};
}

QJsonObject FileOps::rename(const QString &path, const QString &name, const QString &activeDir)
{
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.endsWith(".part"))
        return {{"error", "bad_name"}};
    if (!FileBrowser::isAllowedPath(path, false))
        return {{"error", "not_allowed"}, {"path", path}};
    const QFileInfo info(path);
    if (!info.exists() || info.isSymLink())
        return {{"error", "not_found"}, {"path", path}};
    if (holdsActive(path, activeDir))
        return {{"error", "in_use"}, {"path", path}};

    const QString target = info.absolutePath() + '/' + name;
    if (!FileBrowser::isAllowedPath(target, false))
        return {{"error", "not_allowed"}, {"path", target}};
    if (QFileInfo::exists(target))
        return {{"error", "exists"}, {"path", target}};
    if (!QFile::rename(path, target))
        return {{"error", "failed"}, {"path", path}};
    return {{"ok", true}, {"path", target}};
}

QJsonObject FileOps::eject(const QString &mountPoint)
{
    if (!(mountPoint.startsWith("/media/") || mountPoint.startsWith("/mnt/")) || mountPoint.contains(".."))
        return {{"error", "not_usb"}, {"id", mountPoint}};

    QProcess helper;
    helper.start("sudo", {"-n", "/usr/local/bin/nocturne-usb-mount", "eject", mountPoint});
    if (!helper.waitForFinished(30000))
    {
        helper.kill();
        return {{"error", "timeout"}, {"id", mountPoint}};
    }
    if (helper.exitCode() != 0)
    {
        const QString why = QString::fromUtf8(helper.readAllStandardError()).trimmed();
        return {{"error", "failed"}, {"id", mountPoint}, {"message", why}};
    }
    return {{"ok", true}, {"id", mountPoint}};
}

qint64 FileOps::treeBytes(const QString &path)
{
    const QFileInfo info(path);
    if (info.isFile())
        return info.size();
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}
}
