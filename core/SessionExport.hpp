#pragma once

#include "Session.hpp"
#include "SessionStore.hpp"

#include <QByteArray>
#include <QList>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace zterminal {

// JSON export/import of saved sessions (re-importable format).
//
// Document shape (formatVersion 1):
//   {
//     "format": "zterminal-sessions",
//     "formatVersion": 1,
//     "exportedAt": "<ISO-8601 UTC>",
//     "sessions": [ { ... SessionConfig fields ... } ]
//   }
//
// Secrets are NEVER included. passwordStored mirrors SessionConfig::useStoredPassword
// (SSH vault flag) on export, for information only: importing never turns
// "use stored password" on (see importSessionsFromJson). Serial login
// passwords live only in the vault and are not exported.
//
// An import file is untrusted input. Importing:
//   - refuses sessions whose ssh options could run local commands, load
//     config files/libraries, defeat host-key checking or forward credentials
//     (validateImportedSession), listing them in SessionImportResult::errors;
//   - switches "use stored password" off on every imported or overwritten
//     session, so an import can never point an existing stored password at a
//     new host (vault secrets are also bound to their target, VaultBindings);
//   - saves the sessions as unapproved unless the caller had the user review
//     and approve them (ImportApproval::Approved); unapproved sessions don't launch.
//
// TODO(import): a future UI may offer selective import / rename-on-conflict;
// the schema and parse path below are the contract for that.

inline constexpr const char *kSessionsExportFormat = "zterminal-sessions";
inline constexpr int kSessionsExportFormatVersion = 1;

QJsonObject sessionToJson(const SessionConfig &s);
// Unknown/missing type -> LocalShell (same as SessionConfig::typeFromString).
// Returns nullopt and sets *error on hard failures (empty/invalid name, bad port).
std::optional<SessionConfig> sessionFromJson(const QJsonObject &o, QString *error = nullptr);

// Pretty-printed JSON document for the given sessions (any order; names unique
// is the caller's job). Never includes password material.
QByteArray sessionsToExportJson(const QList<SessionConfig> &sessions);

struct SessionsExportDocument {
    QList<SessionConfig> sessions;
    int formatVersion = 0;
    QString exportedAt; // may be empty on older/hand-written files
};

// Parses a document produced by sessionsToExportJson (or a compatible hand edit).
// Rejects unknown format / unsupported formatVersion.
std::optional<SessionsExportDocument> sessionsFromExportJson(const QByteArray &json,
                                                             QString *error = nullptr);

// Write every saved session from the store to path. Creates parent dirs as needed.
bool exportSessionsToFile(const SessionStore &store, const QString &path, QString *error = nullptr);

enum class SessionImportConflict {
    Overwrite, // replace existing sessions with the same name
    Skip,      // leave existing sessions alone; only add new names
};

enum class ImportApproval {
    Pending,  // saved with approved=false (default): must be reviewed before use
    Approved, // the user reviewed exactly these sessions in the import review dialog
};

// What an imported session is saved as: stored-password use off, approval as given.
SessionConfig sanitizeImportedSession(SessionConfig s, ImportApproval approval);

struct SessionImportResult {
    int imported = 0;    // newly written (did not exist before)
    int overwritten = 0; // replaced an existing name
    int skipped = 0;     // conflict + Skip policy
    int rejected = 0;    // refused by validateImportedSession (also listed in errors)
    QStringList errors;  // per-session save/validation failures (non-fatal)
};

// Merge sessions from a JSON document into the store. Does not touch the vault.
SessionImportResult importSessionsFromJson(SessionStore &store, const QByteArray &json,
                                           SessionImportConflict conflict, QString *error = nullptr,
                                           ImportApproval approval = ImportApproval::Pending);
bool importSessionsFromFile(SessionStore &store, const QString &path,
                            SessionImportConflict conflict, QString *error = nullptr,
                            ImportApproval approval = ImportApproval::Pending);

// Default save-dialog name: zterminal-sessions-YYYYMMDD.json (local date).
QString defaultSessionsExportFileName();

} // namespace zterminal
