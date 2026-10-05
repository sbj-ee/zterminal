#include "SessionExport.hpp"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace zterminal {

namespace {

void putString(QJsonObject &o, const char *key, const QString &value)
{
    if (!value.isEmpty()) {
        o.insert(QLatin1String(key), value);
    }
}

void putIntIfNotDefault(QJsonObject &o, const char *key, int value, int def)
{
    if (value != def) {
        o.insert(QLatin1String(key), value);
    }
}

void putBoolIfTrue(QJsonObject &o, const char *key, bool value)
{
    if (value) {
        o.insert(QLatin1String(key), true);
    }
}

// Returns false and sets *error when the key is present but not a string.
bool readOptionalString(const QJsonObject &o, const char *key, QString *out, QString *error)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined() || v.isNull()) {
        *out = {};
        return true;
    }
    if (!v.isString()) {
        if (error) {
            *error = QStringLiteral("\"%1\" must be a string.").arg(QLatin1String(key));
        }
        return false;
    }
    *out = v.toString();
    return true;
}

int intOr(const QJsonObject &o, const char *key, int def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toInt(def) : def;
}

bool boolOr(const QJsonObject &o, const char *key, bool def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : def;
}

} // namespace

QJsonObject sessionToJson(const SessionConfig &s)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), s.name);
    o.insert(QStringLiteral("type"), SessionConfig::typeToString(s.type));

    if (s.type == SessionConfig::Type::Ssh) {
        putString(o, "host", s.host);
        putString(o, "user", s.user);
        o.insert(QStringLiteral("port"), s.port);
        putString(o, "keyFile", s.keyFile);
        putString(o, "jumpHost", s.jumpHost);
        putString(o, "extraArgs", s.extraArgs);
        // Flag only — never the password itself (vault-only).
        o.insert(QStringLiteral("passwordStored"), s.useStoredPassword);
        o.insert(QStringLiteral("keepaliveInterval"), s.keepaliveInterval);
        o.insert(QStringLiteral("keepaliveCountMax"), s.keepaliveCountMax);
    }

    if (s.type == SessionConfig::Type::Serial) {
        const SessionConfig d;
        putString(o, "serialDevice", s.serialDevice);
        o.insert(QStringLiteral("baudRate"), s.baudRate);
        o.insert(QStringLiteral("dataBits"), s.dataBits);
        o.insert(QStringLiteral("parity"), s.parity);
        o.insert(QStringLiteral("stopBits"), s.stopBits);
        o.insert(QStringLiteral("flowControl"), s.flowControl);
        o.insert(QStringLiteral("localEcho"), s.localEcho);
        o.insert(QStringLiteral("enterSends"), s.enterSends);
        putIntIfNotDefault(o, "charDelayMs", s.charDelayMs, d.charDelayMs);
        putIntIfNotDefault(o, "lineDelayMs", s.lineDelayMs, d.lineDelayMs);
        o.insert(QStringLiteral("breakMs"), s.breakMs);
        putString(o, "loginUser", s.loginUser);
        // Serial login passwords stay in the vault; only the user name is exported.
    }

    putBoolIfTrue(o, "autoLog", s.autoLog);
    putBoolIfTrue(o, "autoReconnect", s.autoReconnect);
    putString(o, "fontFamily", s.fontFamily);
    putIntIfNotDefault(o, "fontSize", s.fontSize, 0);
    putString(o, "colorScheme", s.colorScheme);
    return o;
}

std::optional<SessionConfig> sessionFromJson(const QJsonObject &o, QString *error)
{
    auto fail = [error](const QString &m) -> std::optional<SessionConfig> {
        if (error) {
            *error = m;
        }
        return std::nullopt;
    };

    if (!o.contains(QStringLiteral("name")) || !o.value(QStringLiteral("name")).isString()) {
        return fail(QStringLiteral("Session object is missing a string \"name\"."));
    }
    SessionConfig s;
    s.name = o.value(QStringLiteral("name")).toString();
    if (const QString e = validateSessionName(s.name); !e.isEmpty()) {
        return fail(e);
    }

    if (o.contains(QStringLiteral("type"))) {
        if (!o.value(QStringLiteral("type")).isString()) {
            return fail(QStringLiteral("\"type\" must be a string."));
        }
        s.type = SessionConfig::typeFromString(o.value(QStringLiteral("type")).toString());
    }

    if (s.type == SessionConfig::Type::Ssh) {
        if (!readOptionalString(o, "host", &s.host, error)
            || !readOptionalString(o, "user", &s.user, error)
            || !readOptionalString(o, "keyFile", &s.keyFile, error)
            || !readOptionalString(o, "jumpHost", &s.jumpHost, error)
            || !readOptionalString(o, "extraArgs", &s.extraArgs, error)) {
            return std::nullopt;
        }
        s.port = intOr(o, "port", 22);
        if (s.port < 1 || s.port > 65535) {
            return fail(QStringLiteral("SSH port must be 1–65535."));
        }
        // Accept both passwordStored (export name) and authFromVault (INI key alias).
        if (o.contains(QStringLiteral("passwordStored"))) {
            s.useStoredPassword = boolOr(o, "passwordStored", false);
        } else {
            s.useStoredPassword = boolOr(o, "authFromVault", false);
        }
        const SessionConfig d;
        s.keepaliveInterval = intOr(o, "keepaliveInterval", d.keepaliveInterval);
        s.keepaliveCountMax = intOr(o, "keepaliveCountMax", d.keepaliveCountMax);
    }

    if (s.type == SessionConfig::Type::Serial) {
        const SessionConfig d;
        if (!readOptionalString(o, "serialDevice", &s.serialDevice, error)
            || !readOptionalString(o, "parity", &s.parity, error)
            || !readOptionalString(o, "flowControl", &s.flowControl, error)
            || !readOptionalString(o, "enterSends", &s.enterSends, error)
            || !readOptionalString(o, "loginUser", &s.loginUser, error)) {
            return std::nullopt;
        }
        if (s.parity.isEmpty()) {
            s.parity = d.parity;
        }
        if (s.flowControl.isEmpty()) {
            s.flowControl = d.flowControl;
        }
        if (s.enterSends.isEmpty()) {
            s.enterSends = d.enterSends;
        }
        s.baudRate = intOr(o, "baudRate", d.baudRate);
        s.dataBits = intOr(o, "dataBits", d.dataBits);
        s.stopBits = intOr(o, "stopBits", d.stopBits);
        s.localEcho = boolOr(o, "localEcho", d.localEcho);
        s.charDelayMs = intOr(o, "charDelayMs", d.charDelayMs);
        s.lineDelayMs = intOr(o, "lineDelayMs", d.lineDelayMs);
        s.breakMs = intOr(o, "breakMs", d.breakMs);
    }

    s.autoLog = boolOr(o, "autoLog", false);
    s.autoReconnect = boolOr(o, "autoReconnect", false);
    if (!readOptionalString(o, "fontFamily", &s.fontFamily, error)
        || !readOptionalString(o, "colorScheme", &s.colorScheme, error)) {
        return std::nullopt;
    }
    s.fontSize = intOr(o, "fontSize", 0);
    return s;
}

QByteArray sessionsToExportJson(const QList<SessionConfig> &sessions)
{
    QJsonArray arr;
    for (const SessionConfig &s : sessions) {
        arr.append(sessionToJson(s));
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QLatin1String(kSessionsExportFormat));
    root.insert(QStringLiteral("formatVersion"), kSessionsExportFormatVersion);
    root.insert(QStringLiteral("exportedAt"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("sessions"), arr);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::optional<SessionsExportDocument> sessionsFromExportJson(const QByteArray &json, QString *error)
{
    auto fail = [error](const QString &m) -> std::optional<SessionsExportDocument> {
        if (error) {
            *error = m;
        }
        return std::nullopt;
    };

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("Not valid JSON: %1").arg(pe.errorString()));
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String(kSessionsExportFormat)) {
        return fail(QStringLiteral("Not a zterminal sessions export (expected format \"%1\").")
                        .arg(QLatin1String(kSessionsExportFormat)));
    }
    const int ver = root.value(QStringLiteral("formatVersion")).toInt(0);
    if (ver < 1 || ver > kSessionsExportFormatVersion) {
        return fail(QStringLiteral("Unsupported sessions export formatVersion %1 (this build reads 1–%2).")
                        .arg(ver)
                        .arg(kSessionsExportFormatVersion));
    }
    if (!root.contains(QStringLiteral("sessions")) || !root.value(QStringLiteral("sessions")).isArray()) {
        return fail(QStringLiteral("Export is missing a \"sessions\" array."));
    }

    SessionsExportDocument out;
    out.formatVersion = ver;
    out.exportedAt = root.value(QStringLiteral("exportedAt")).toString();
    const QJsonArray arr = root.value(QStringLiteral("sessions")).toArray();
    for (int i = 0; i < arr.size(); ++i) {
        if (!arr.at(i).isObject()) {
            return fail(QStringLiteral("sessions[%1] is not an object.").arg(i));
        }
        QString err;
        const auto s = sessionFromJson(arr.at(i).toObject(), &err);
        if (!s) {
            return fail(QStringLiteral("sessions[%1]: %2").arg(i).arg(err));
        }
        out.sessions.append(*s);
    }
    return out;
}

bool exportSessionsToFile(const SessionStore &store, const QString &path, QString *error)
{
    auto fail = [error](const QString &m) {
        if (error) {
            *error = m;
        }
        return false;
    };
    QList<SessionConfig> sessions;
    for (const QString &name : store.names()) {
        const auto s = store.load(name);
        if (!s) {
            return fail(QStringLiteral("Can't load saved session \"%1\".").arg(name));
        }
        sessions.append(*s);
    }
    const QByteArray bytes = sessionsToExportJson(sessions);
    const QFileInfo fi(path);
    if (!fi.dir().exists() && !QDir().mkpath(fi.dir().absolutePath())) {
        return fail(QStringLiteral("Can't create directory for %1").arg(path));
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("Can't write %1: %2").arg(path, f.errorString()));
    }
    if (f.write(bytes) != bytes.size()) {
        return fail(QStringLiteral("Short write to %1").arg(path));
    }
    return true;
}

SessionImportResult importSessionsFromJson(SessionStore &store, const QByteArray &json,
                                           SessionImportConflict conflict, QString *error)
{
    SessionImportResult result;
    QString parseErr;
    const auto doc = sessionsFromExportJson(json, &parseErr);
    if (!doc) {
        if (error) {
            *error = parseErr;
        }
        return result;
    }
    for (const SessionConfig &s : doc->sessions) {
        const bool exists = store.contains(s.name);
        if (exists && conflict == SessionImportConflict::Skip) {
            ++result.skipped;
            continue;
        }
        QString saveErr;
        if (!store.save(s, &saveErr)) {
            result.errors << QStringLiteral("%1: %2").arg(s.name, saveErr);
            continue;
        }
        if (exists) {
            ++result.overwritten;
        } else {
            ++result.imported;
        }
    }
    return result;
}

bool importSessionsFromFile(SessionStore &store, const QString &path,
                            SessionImportConflict conflict, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Can't read %1: %2").arg(path, f.errorString());
        }
        return false;
    }
    const QByteArray bytes = f.readAll();
    QString err;
    const SessionImportResult r = importSessionsFromJson(store, bytes, conflict, &err);
    if (!err.isEmpty() && r.imported == 0 && r.overwritten == 0 && r.skipped == 0) {
        // Parse failure (or every session failed before any write).
        if (error) {
            *error = err.isEmpty() ? r.errors.join(QLatin1Char('\n')) : err;
        }
        return false;
    }
    if (!r.errors.isEmpty() && r.imported == 0 && r.overwritten == 0) {
        if (error) {
            *error = r.errors.join(QLatin1Char('\n'));
        }
        return false;
    }
    if (error && !r.errors.isEmpty()) {
        *error = r.errors.join(QLatin1Char('\n'));
    }
    return true;
}

QString defaultSessionsExportFileName()
{
    return QStringLiteral("zterminal-sessions-%1.json")
        .arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
}

} // namespace zterminal
