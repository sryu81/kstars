/*
    SPDX-FileCopyrightText: 2026 Nocturne

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QSharedPointer>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace EkosRemote
{
struct FileOpsJob;

/**
 * Delete, copy and move inside the Nocturne folders, one operation at a time, off the GUI thread.
 *
 * Every path is checked with FileBrowser::isAllowedPath (never a Nocturne root, never outside one) and
 * nothing that holds the running capture's folder is touched. Copy and move put each source at the same
 * relative path under the target drive's Nocturne folder, merging into folders that exist and refusing a
 * name that exists. A file is written as `<name>.part` and renamed when complete, so a cancel or a pulled
 * drive never leaves a half file under its real name. Copy compares sizes; move also compares a SHA-1 of
 * source and copy before the source file is removed.
 */
class FileOps : public QObject
{
        Q_OBJECT

    public:
        explicit FileOps(QObject *parent = nullptr);

        bool busy() const;

        /** Maps a drive id to its Nocturne folder; the default asks FileBrowser::listDrives(). Tests replace it. */
        void setDriveLookup(std::function<QString(const QString &)> lookup);

        /** Mount point or "sd" of the drive the running operation reads from or writes to; empty when idle. */
        QStringList drivesInUse() const;

        /**
         * Starts @p op (`delete`, `copy`, `move`). Returns `{id}` once the request is accepted, else
         * `{error}` (`busy`, `bad_request`, `not_allowed`, `not_found`, `in_use`, `no_such_drive`,
         * `same_drive`); the work, and any later failure, is reported through progress().
         */
        QJsonObject start(const QString &op, const QStringList &paths, const QString &toDrive,
                          const QString &activeDir);

        /** Renames inside the same folder, at once. `{ok, path}` or `{error}`. */
        static QJsonObject rename(const QString &path, const QString &name, const QString &activeDir);

        /** Flags the running operation @p id to stop after the current chunk. */
        QJsonObject cancel(int id);

        /** Syncs and unmounts the USB drive at @p mountPoint through the usb-automount helper. Blocks. */
        static QJsonObject eject(const QString &mountPoint);

        /** Total bytes the paths hold, for the space check. Exposed for tests. */
        static qint64 treeBytes(const QString &path);

    Q_SIGNALS:
        /** `{id, op, state, filesDone, filesTotal, bytesDone, bytesTotal, current, error?, failed, needed?, free?}`. */
        void progress(const QJsonObject &snapshot);

    private:
        QJsonObject snapshot() const;
        void finish();

        std::function<QString(const QString &)> m_lookup;
        QSharedPointer<FileOpsJob> m_job;
        QTimer m_timer;
        int m_nextId = 1;
};
}
