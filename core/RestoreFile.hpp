#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace zterminal {

// What an update restart reopens: every window, each with every tab's
// launch arguments (what Duplicate would open) and which tab was current.
// Written by UpdateManager::restart() to a private temporary file, read and
// deleted by the new process (`zterminal --restore FILE`).
struct RestoreWindow {
    QList<QStringList> tabs;
    int current = 0;
};

// Arguments a tab may be restored with: a local shell, a saved session,
// ad-hoc ssh/serial or a command. Anything else (--restore, --group,
// --version...) is not a tab and is skipped.
bool isRestorableTab(const QStringList &args);

QByteArray restoreToJson(const QList<RestoreWindow> &windows);
// Windows with no restorable tab are dropped; nullopt (and *error) for a
// malformed file.
std::optional<QList<RestoreWindow>> restoreFromJson(const QByteArray &json, QString *error = nullptr);

// Writes the file (0600, in the temp directory) and returns its path, or an
// empty string on failure.
QString writeRestoreFile(const QList<RestoreWindow> &windows);
// Reads and deletes the file. Refuses a file that isn't a regular file owned
// by this user.
std::optional<QList<RestoreWindow>> takeRestoreFile(const QString &path, QString *error = nullptr);

} // namespace zterminal
