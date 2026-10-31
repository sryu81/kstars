/*
    SPDX-FileCopyrightText: 2026 Nocturne

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "filebrowser.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>

namespace EkosRemote
{
namespace FileBrowser
{
namespace
{
constexpr int MaxEntries = 5000;
constexpr int SizeBudget = 2000;

const QString RootName = QStringLiteral("Nocturne");

/** `/mnt/<name>/Nocturne` and `/media/<label>/Nocturne`, the USB drives' roots. */
const QRegularExpression &usbRoot()
{
    static const QRegularExpression re(QStringLiteral("^(/mnt|/media)/[^/]+/") + RootName + QStringLiteral("(/|$)"));
    return re;
}

/** Canonical path for something that may not exist yet: the nearest existing ancestor, resolved, plus the rest. */
QString resolveForCheck(const QString &path)
{
    if (!path.startsWith('/') || path.split('/').contains(QStringLiteral("..")))
        return QString();

    QString probe = QDir::cleanPath(path);
    QString rest;
    while (!QFileInfo::exists(probe))
    {
        const int cut = probe.lastIndexOf('/');
        if (cut <= 0)
            return QString();
        rest = probe.mid(cut) + rest;
        probe = probe.left(cut);
    }
    const QString canon = QFileInfo(probe).canonicalFilePath();
    return canon.isEmpty() ? QString() : canon + rest;
}

qint64 mtime(const QFileInfo &info)
{
    return info.lastModified().toSecsSinceEpoch();
}
}

QString sdRoot()
{
    return QDir::homePath() + '/' + RootName;
}

QString canonical(const QString &path)
{
    const QString canon = QFileInfo(path).canonicalFilePath();
    return canon.isEmpty() ? path : canon;
}

bool isAllowedPath(const QString &path, bool allowRoot)
{
    const QString canon = resolveForCheck(path);
    if (canon.isEmpty())
        return false;

    const QString sd = resolveForCheck(sdRoot());
    if (!sd.isEmpty() && (canon == sd || canon.startsWith(sd + '/')))
        return allowRoot || canon != sd;

    const auto match = usbRoot().match(canon);
    if (!match.hasMatch())
        return false;
    // The root itself is <mount>/Nocturne with nothing after it.
    return allowRoot || match.capturedEnd() != canon.size();
}

QJsonArray listDrives()
{
    QJsonArray drives;

    const QStorageInfo sd(QDir::homePath());
    if (sd.isValid() && sd.isReady())
    {
        drives.append(QJsonObject
        {
            {"id", "sd"},
            {"kind", "sd"},
            {"label", "SD card"},
            {"fs", QString::fromUtf8(sd.fileSystemType())},
            {"path", sdRoot()},
            {"total", static_cast<double>(sd.bytesTotal())},
            {"free", static_cast<double>(sd.bytesAvailable())},
            {"readOnly", sd.isReadOnly()},
            {"hasRoot", QFileInfo(sdRoot()).isDir()}
        });
    }

    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes())
    {
        const QString root = volume.rootPath();
        if (!volume.isValid() || !volume.isReady() || (!root.startsWith("/mnt/") && !root.startsWith("/media/")))
            continue;

        QString label = volume.name();
        if (label.isEmpty())
            label = QFileInfo(root).fileName();
        const QString path = root + '/' + RootName;
        drives.append(QJsonObject
        {
            {"id", root},
            {"kind", "usb"},
            {"label", label},
            {"fs", QString::fromUtf8(volume.fileSystemType())},
            {"path", path},
            {"total", static_cast<double>(volume.bytesTotal())},
            {"free", static_cast<double>(volume.bytesAvailable())},
            {"readOnly", volume.isReadOnly()},
            {"hasRoot", QFileInfo(path).isDir()}
        });
    }
    return drives;
}

bool isFitsName(const QString &name)
{
    const QString lower = name.toLower();
    return lower.endsWith(".fits") || lower.endsWith(".fit") || lower.endsWith(".fts");
}

QJsonObject readHeader(const QString &path)
{
    QJsonObject out{{"path", path}};

    if (!isAllowedPath(path, false))
    {
        out["error"] = "not_allowed";
        return out;
    }
    const QFileInfo info(path);
    if (!info.exists())
    {
        out["error"] = "not_found";
        return out;
    }
    if (!info.isFile())
    {
        out["error"] = "not_file";
        return out;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        out["error"] = "not_found";
        return out;
    }

    constexpr int CardSize = 80;
    constexpr int BlockSize = 2880;
    constexpr int MaxBlocks = 64; // 5,000+ cards: a header this long is not a camera frame

    QByteArray block = file.read(BlockSize);
    if (block.size() < BlockSize || !block.startsWith("SIMPLE  ="))
    {
        out["error"] = "not_fits";
        return out;
    }

    QJsonArray records;
    for (int blocks = 0; blocks < MaxBlocks; ++blocks)
    {
        for (int at = 0; at + CardSize <= block.size(); at += CardSize)
        {
            const QByteArray card = block.mid(at, CardSize);
            const QString key = QString::fromLatin1(card.left(8)).trimmed();
            if (key == "END")
            {
                out["records"] = records;
                return out;
            }
            // Only a card with "= " in columns 9-10 carries a value.
            if (key.isEmpty() || card.mid(8, 2) != "= ")
                continue;

            const QByteArray field = card.mid(10);
            QString value;
            if (field.trimmed().startsWith('\''))
            {
                // A string: up to the closing quote, '' standing for one quote.
                const int open = field.indexOf('\'');
                int i = open + 1;
                QByteArray text;
                while (i < field.size())
                {
                    if (field[i] == '\'')
                    {
                        if (i + 1 < field.size() && field[i + 1] == '\'')
                        {
                            text.append('\'');
                            i += 2;
                            continue;
                        }
                        break;
                    }
                    text.append(field[i++]);
                }
                value = QString::fromLatin1(text).trimmed();
            }
            else
            {
                const int slash = field.indexOf('/');
                value = QString::fromLatin1(slash < 0 ? field : field.left(slash)).trimmed();
            }
            records.append(QJsonObject{{"key", key}, {"value", value}});
        }
        block = file.read(BlockSize);
        if (block.size() < BlockSize)
            break;
    }

    out["error"] = "not_fits"; // no END card within the limit
    return out;
}

QJsonObject listFolder(const QString &path, const QString &activeDir, bool capturing)
{
    QJsonObject out{{"path", path}, {"capturing", capturing}};

    if (!isAllowedPath(path))
    {
        out["error"] = "not_allowed";
        return out;
    }
    const QFileInfo info(path);
    if (!info.exists())
    {
        out["error"] = "not_found";
        return out;
    }
    if (!info.isDir())
    {
        out["error"] = "not_dir";
        return out;
    }

    const QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks;
    QFileInfoList list = QDir(path).entryInfoList(filters, QDir::Time);
    const bool truncated = list.size() > MaxEntries;
    if (truncated)
        list = list.mid(0, MaxEntries);

    int budget = SizeBudget;
    QJsonArray entries;
    for (const QFileInfo &entry : list)
    {
        QJsonObject row
        {
            {"name", entry.fileName()},
            {"isDir", entry.isDir()},
            {"modified", static_cast<double>(mtime(entry))}
        };

        if (!entry.isDir())
        {
            row["size"] = static_cast<double>(entry.size());
        }
        else
        {
            const QString dirPath = entry.absoluteFilePath();
            row["count"] = QDir(dirPath).entryList(filters).size();

            qint64 bytes = 0;
            bool partial = false;
            QDirIterator it(dirPath, QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                if (budget <= 0)
                {
                    partial = true;
                    break;
                }
                --budget;
                it.next();
                bytes += it.fileInfo().size();
            }
            row["bytes"] = static_cast<double>(bytes);
            if (partial)
                row["bytesPartial"] = true;

            const QString canon = canonical(dirPath);
            if (!activeDir.isEmpty() && (activeDir == canon || activeDir.startsWith(canon + '/')))
                row["inUse"] = true;
        }
        entries.append(row);
    }

    out["entries"] = entries;
    if (truncated)
        out["truncated"] = true;
    return out;
}
}
}
