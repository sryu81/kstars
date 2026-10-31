/*
    SPDX-FileCopyrightText: 2026 Nocturne

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace EkosRemote
{
/**
 * Read-only view of the Pi's capture storage for the phone's file manager.
 *
 * Every drive keeps its frames in one folder called Nocturne, at the same relative layout
 * (<Nocturne>/<Target>/<night>/<FrameType>/<Filter>/<file>): the SD card's is ~/Nocturne, a USB
 * drive's is /mnt/<name>/Nocturne or /media/<label>/Nocturne. Nothing outside those folders is
 * listed, created or removed, whatever path a client sends.
 */
namespace FileBrowser
{
/** ~/Nocturne, the SD card's capture root. */
QString sdRoot();

/**
 * Whether @p path is inside a Nocturne root (or is one, when @p allowRoot). The check is made on
 * the canonical path, so symlinks cannot lead out; a path with a `..` part is refused outright.
 * A path that does not exist yet is judged by its nearest existing ancestor.
 */
bool isAllowedPath(const QString &path, bool allowRoot = true);

/** The canonical form of @p path, or the path itself when it cannot be resolved. */
QString canonical(const QString &path);

/**
 * One entry per drive: `{id, kind: "sd"|"usb", label, fs, path, total, free, readOnly, hasRoot}`.
 * `path` is the drive's Nocturne folder and `hasRoot` says whether it exists yet.
 */
QJsonArray listDrives();

/**
 * Lists @p path, newest first. Entries: `{name, isDir, size, modified}` and, for folders, `count`
 * (direct children), `bytes` (recursive, from a budget of entries shared by the whole request) and
 * `bytesPartial` when the budget ran out. A folder that holds @p activeDir (the folder a running
 * capture writes to) has `inUse: true`. Symlinks and hidden files are skipped, at most 5,000 entries
 * come back (`truncated`). Failure is `{path, error}` with `not_allowed`, `not_found` or `not_dir`.
 * Blocks on the drive, so call it off the GUI thread.
 */
QJsonObject listFolder(const QString &path, const QString &activeDir, bool capturing);
}
}
