#include "SessionDialog.hpp"

#include "AppSettings.hpp"
#include "ColorScheme.hpp"
#include "SecureBuffer.hpp"
#include "SerialBackend.hpp"
#include "Vault.hpp"
#include "VaultManager.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace zterminal {

namespace {
QLineEdit *lineEdit(const char *name, const QString &placeholder)
{
    auto *e = new QLineEdit;
    e->setObjectName(QString::fromLatin1(name));
    e->setPlaceholderText(placeholder);
    return e;
}

QLineEdit *secretEdit(const char *name)
{
    QLineEdit *e = lineEdit(name, QStringLiteral("(unchanged)"));
    e->setEchoMode(QLineEdit::Password);
    e->setContextMenuPolicy(Qt::NoContextMenu);
    e->setToolTip(QStringLiteral("Saved to the encrypted vault when you press Save, never to the session file. "
                                 "Leave empty to keep the stored one."));
    return e;
}
} // namespace

SessionDialog::SessionDialog(const SessionStore &store, const SessionConfig &initial, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
{
    setWindowTitle(QStringLiteral("zterminal Sessions"));
    setObjectName(QStringLiteral("sessionDialog"));

    // Left: saved sessions.
    auto *savedBox = new QGroupBox(QStringLiteral("Saved Sessions"));
    auto *savedLayout = new QVBoxLayout(savedBox);
    m_name = lineEdit("name", QStringLiteral("Session name"));
    savedLayout->addWidget(m_name);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("sessionList"));
    m_list->setMinimumWidth(190);
    savedLayout->addWidget(m_list, 1);
    auto *savedButtons = new QHBoxLayout;
    m_load = new QPushButton(QStringLiteral("&Load"));
    m_save = new QPushButton(QStringLiteral("&Save"));
    m_delete = new QPushButton(QStringLiteral("&Delete"));
    m_load->setObjectName(QStringLiteral("load"));
    m_save->setObjectName(QStringLiteral("save"));
    m_delete->setObjectName(QStringLiteral("delete"));
    for (QPushButton *b : {m_load, m_save, m_delete}) {
        b->setAutoDefault(false);
        savedButtons->addWidget(b);
    }
    savedLayout->addLayout(savedButtons);
    auto *where = new QLabel(QStringLiteral("<small>Stored in %1 (no passwords; stored passwords live only in the encrypted vault)</small>")
                                 .arg(QDir::toNativeSeparators(m_store.directory()).replace(QDir::homePath(), QStringLiteral("~"))));
    where->setWordWrap(true);
    savedLayout->addWidget(where);

    // Right: connection.
    auto *connBox = new QGroupBox(QStringLiteral("Connection"));
    auto *connLayout = new QVBoxLayout(connBox);
    auto *typeForm = new QFormLayout;
    m_type = new QComboBox;
    m_type->setObjectName(QStringLiteral("type"));
    m_type->addItem(QStringLiteral("Local shell"), SessionConfig::typeToString(SessionConfig::Type::LocalShell));
    m_type->addItem(QStringLiteral("SSH"), SessionConfig::typeToString(SessionConfig::Type::Ssh));
    m_type->addItem(QStringLiteral("Serial"), SessionConfig::typeToString(SessionConfig::Type::Serial));
    typeForm->addRow(QStringLiteral("Type:"), m_type);
    m_autoLog = new QCheckBox(QStringLiteral("Log this session to a file automatically"));
    m_autoLog->setObjectName(QStringLiteral("autoLog"));
    m_autoLog->setToolTip(QStringLiteral("Starts Session > Start Logging when the session opens "
                                         "(folder and timestamps: Preferences > Session logs)"));
    typeForm->addRow(m_autoLog);
    m_autoReconnect = new QCheckBox(QStringLiteral("Reconnect automatically after a drop"));
    m_autoReconnect->setObjectName(QStringLiteral("autoReconnect"));
    m_autoReconnect->setToolTip(QStringLiteral(
        "SSH: when the connection drops (network error), retry after 2, 4, 8 \u2026 up to 60 s, with Cancel. "
        "Never after a clean exit. Serial ports always reopen when the device comes back."));
    typeForm->addRow(m_autoReconnect);
    connLayout->addLayout(typeForm);

    m_sshBox = new QGroupBox(QStringLiteral("SSH (runs the system ssh)"));
    m_sshBox->setObjectName(QStringLiteral("sshGroup"));
    auto *sshForm = new QFormLayout(m_sshBox);
    m_host = lineEdit("host", QStringLiteral("host name or address"));
    sshForm->addRow(QStringLiteral("Host:"), m_host);
    m_user = lineEdit("user", QStringLiteral("(ssh default)"));
    sshForm->addRow(QStringLiteral("User:"), m_user);
    m_port = new QSpinBox;
    m_port->setObjectName(QStringLiteral("port"));
    m_port->setRange(1, 65535);
    m_port->setValue(22);
    sshForm->addRow(QStringLiteral("Port:"), m_port);
    auto *keyRow = new QHBoxLayout;
    m_key = lineEdit("keyFile", QStringLiteral("(agent / ssh config)"));
    auto *browse = new QPushButton(QStringLiteral("Browse\u2026"));
    browse->setAutoDefault(false);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Private key"), QDir::homePath() + QStringLiteral("/.ssh"));
        if (!f.isEmpty()) {
            m_key->setText(f);
        }
    });
    keyRow->addWidget(m_key);
    keyRow->addWidget(browse);
    sshForm->addRow(QStringLiteral("Key file:"), keyRow);
    m_jump = lineEdit("jumpHost", QStringLiteral("[user@]host[:port] (ssh -J)"));
    sshForm->addRow(QStringLiteral("Jump host:"), m_jump);
    m_extra = lineEdit("extraArgs", QStringLiteral("e.g. -4 -o Compression=yes"));
    m_extra->setToolTip(QStringLiteral("ssh options only. Split like a command line (use double quotes for spaces); "
                                       "never passed to a shell."));
    sshForm->addRow(QStringLiteral("Extra options:"), m_extra);
    auto *keepRow = new QHBoxLayout;
    m_keepInterval = new QSpinBox;
    m_keepInterval->setObjectName(QStringLiteral("keepaliveInterval"));
    m_keepInterval->setRange(0, 3600);
    m_keepInterval->setSuffix(QStringLiteral(" s"));
    m_keepInterval->setSpecialValueText(QStringLiteral("off"));
    m_keepInterval->setValue(SessionConfig{}.keepaliveInterval);
    m_keepInterval->setToolTip(QStringLiteral("ssh -o ServerAliveInterval: send a keepalive after this many idle seconds"));
    m_keepCount = new QSpinBox;
    m_keepCount->setObjectName(QStringLiteral("keepaliveCountMax"));
    m_keepCount->setRange(1, 100);
    m_keepCount->setPrefix(QStringLiteral("\u00d7 "));
    m_keepCount->setValue(SessionConfig{}.keepaliveCountMax);
    m_keepCount->setToolTip(QStringLiteral("ssh -o ServerAliveCountMax: give up (drop) after this many unanswered keepalives"));
    keepRow->addWidget(m_keepInterval);
    keepRow->addWidget(m_keepCount);
    keepRow->addStretch(1);
    sshForm->addRow(QStringLiteral("Keepalive:"), keepRow);
    connect(m_keepInterval, &QSpinBox::valueChanged, this, [this](int v) { m_keepCount->setEnabled(v > 0); });
    m_useStored = new QCheckBox(QStringLiteral("Use stored password"));
    m_useStored->setObjectName(QStringLiteral("useStoredPassword"));
    m_useStored->setToolTip(QStringLiteral("Answer ssh's first password prompt from the encrypted vault (via SSH_ASKPASS). "
                                           "If it is rejected, ssh asks you in the terminal."));
    sshForm->addRow(m_useStored);
    m_sshPassword = secretEdit("sshPassword");
    sshForm->addRow(QStringLiteral("Password:"), m_sshPassword);
    // Read-only line edit: long commands scroll instead of being clipped.
    m_preview = new QLineEdit;
    m_preview->setObjectName(QStringLiteral("commandPreview"));
    m_preview->setReadOnly(true);
    m_preview->setFrame(false);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_preview->setToolTip(QStringLiteral("The exact argv zterminal runs (no shell is involved)"));
    sshForm->addRow(QStringLiteral("Runs:"), m_preview);
    connLayout->addWidget(m_sshBox);

    m_serialBox = new QGroupBox(QStringLiteral("Serial"));
    m_serialBox->setObjectName(QStringLiteral("serialGroup"));
    auto *serForm = new QFormLayout(m_serialBox);
    auto *devRow = new QHBoxLayout;
    m_device = new QComboBox;
    m_device->setObjectName(QStringLiteral("serialDevice"));
    m_device->setEditable(true);
    m_device->setInsertPolicy(QComboBox::NoInsert);
    m_device->lineEdit()->setPlaceholderText(QStringLiteral("/dev/ttyUSB0"));
    m_device->setMinimumContentsLength(16);
    auto *rescan = new QPushButton(QStringLiteral("Rescan"));
    rescan->setObjectName(QStringLiteral("rescanPorts"));
    rescan->setAutoDefault(false);
    connect(rescan, &QPushButton::clicked, this, &SessionDialog::refreshPorts);
    devRow->addWidget(m_device, 1);
    devRow->addWidget(rescan);
    serForm->addRow(QStringLiteral("Device:"), devRow);
    m_baud = new QComboBox;
    m_baud->setObjectName(QStringLiteral("baudRate"));
    m_baud->setEditable(true);
    for (int b : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600}) {
        m_baud->addItem(QString::number(b));
    }
    m_baud->setValidator(new QIntValidator(50, 4000000, m_baud));
    auto *lineRow = new QHBoxLayout;
    m_dataBits = new QComboBox;
    m_dataBits->setObjectName(QStringLiteral("dataBits"));
    for (int d : {8, 7, 6, 5}) {
        m_dataBits->addItem(QString::number(d), d);
    }
    m_parity = new QComboBox;
    m_parity->setObjectName(QStringLiteral("parity"));
    for (const auto &[label, id] : {std::pair{"None", "none"}, {"Even", "even"}, {"Odd", "odd"}, {"Mark", "mark"}, {"Space", "space"}}) {
        m_parity->addItem(QString::fromLatin1(label), QString::fromLatin1(id));
    }
    m_stopBits = new QComboBox;
    m_stopBits->setObjectName(QStringLiteral("stopBits"));
    m_stopBits->addItem(QStringLiteral("1"), 1);
    m_stopBits->addItem(QStringLiteral("2"), 2);
    lineRow->addWidget(m_baud, 2);
    lineRow->addWidget(new QLabel(QStringLiteral("Data")));
    lineRow->addWidget(m_dataBits);
    lineRow->addWidget(new QLabel(QStringLiteral("Parity")));
    lineRow->addWidget(m_parity);
    lineRow->addWidget(new QLabel(QStringLiteral("Stop")));
    lineRow->addWidget(m_stopBits);
    serForm->addRow(QStringLiteral("Baud:"), lineRow);
    m_flow = new QComboBox;
    m_flow->setObjectName(QStringLiteral("flowControl"));
    m_flow->addItem(QStringLiteral("None"), QStringLiteral("none"));
    m_flow->addItem(QStringLiteral("RTS/CTS (hardware)"), QStringLiteral("rtscts"));
    m_flow->addItem(QStringLiteral("XON/XOFF (software)"), QStringLiteral("xonxoff"));
    serForm->addRow(QStringLiteral("Flow control:"), m_flow);
    auto *keyRow2 = new QHBoxLayout;
    m_enter = new QComboBox;
    m_enter->setObjectName(QStringLiteral("enterSends"));
    m_enter->addItem(QStringLiteral("CR"), QStringLiteral("cr"));
    m_enter->addItem(QStringLiteral("CR+LF"), QStringLiteral("crlf"));
    m_enter->addItem(QStringLiteral("LF"), QStringLiteral("lf"));
    m_localEcho = new QCheckBox(QStringLiteral("Local echo"));
    m_localEcho->setObjectName(QStringLiteral("localEcho"));
    keyRow2->addWidget(m_enter);
    keyRow2->addSpacing(12);
    keyRow2->addWidget(m_localEcho);
    keyRow2->addStretch(1);
    serForm->addRow(QStringLiteral("Enter sends:"), keyRow2);
    auto *paceRow = new QHBoxLayout;
    m_charDelay = new QSpinBox;
    m_charDelay->setObjectName(QStringLiteral("charDelayMs"));
    m_charDelay->setRange(0, 1000);
    m_charDelay->setSuffix(QStringLiteral(" ms/char"));
    m_lineDelay = new QSpinBox;
    m_lineDelay->setObjectName(QStringLiteral("lineDelayMs"));
    m_lineDelay->setRange(0, 10000);
    m_lineDelay->setSingleStep(50);
    m_lineDelay->setSuffix(QStringLiteral(" ms/line"));
    paceRow->addWidget(m_charDelay);
    paceRow->addWidget(m_lineDelay);
    serForm->addRow(QStringLiteral("Paste pacing:"), paceRow);
    m_breakMs = new QSpinBox;
    m_breakMs->setObjectName(QStringLiteral("breakMs"));
    m_breakMs->setRange(10, 5000);
    m_breakMs->setSingleStep(50);
    m_breakMs->setSuffix(QStringLiteral(" ms"));
    serForm->addRow(QStringLiteral("Send Break:"), m_breakMs);
    m_loginUser = lineEdit("loginUser", QStringLiteral("(none)"));
    m_loginUser->setToolTip(QStringLiteral("For Session > Send Stored Login"));
    serForm->addRow(QStringLiteral("Login user:"), m_loginUser);
    m_loginPassword = secretEdit("loginPassword");
    serForm->addRow(QStringLiteral("Login password:"), m_loginPassword);
    auto *paceNote = new QLabel(QStringLiteral(
        "<small>Pacing slows pastes for consoles that drop characters (e.g. Cisco: 5 ms/char, 100 ms/line).</small>"));
    serForm->addRow(paceNote);
    connLayout->addWidget(m_serialBox);
    refreshPorts();

    auto *lookBox = new QGroupBox(QStringLiteral("Appearance for this session"));
    auto *lookForm = new QFormLayout(lookBox);
    m_overrideFont = new QCheckBox(QStringLiteral("Use a different font"));
    m_overrideFont->setObjectName(QStringLiteral("overrideFont"));
    lookForm->addRow(m_overrideFont);
    m_font = new QFontComboBox;
    m_font->setObjectName(QStringLiteral("fontFamily"));
    m_font->setFontFilters(QFontComboBox::MonospacedFonts);
    m_fontSize = new QSpinBox;
    m_fontSize->setObjectName(QStringLiteral("fontSize"));
    m_fontSize->setRange(0, 48);
    m_fontSize->setSpecialValueText(QStringLiteral("Default"));
    auto *fontRow = new QHBoxLayout;
    fontRow->addWidget(m_font, 1);
    fontRow->addWidget(m_fontSize);
    lookForm->addRow(QStringLiteral("Font:"), fontRow);
    m_scheme = new QComboBox;
    m_scheme->setObjectName(QStringLiteral("colorScheme"));
    m_scheme->addItem(QStringLiteral("Default (Preferences)"), QString());
    for (const ColorScheme &cs : ColorScheme::builtIn()) {
        m_scheme->addItem(cs.name, cs.id);
    }
    lookForm->addRow(QStringLiteral("Color scheme:"), m_scheme);
    connLayout->addWidget(lookBox);
    connLayout->addStretch(1);

    m_error = new QLabel;
    m_error->setObjectName(QStringLiteral("error"));
    m_error->setStyleSheet(QStringLiteral("color: #c0392b"));
    m_error->setWordWrap(true);

    auto *buttons = new QDialogButtonBox;
    QPushButton *open = buttons->addButton(QStringLiteral("&Open"), QDialogButtonBox::AcceptRole);
    open->setObjectName(QStringLiteral("open"));
    open->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SessionDialog::openSession);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *columns = new QHBoxLayout;
    columns->addWidget(savedBox, 2);
    columns->addWidget(connBox, 3);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(columns);
    layout->addWidget(m_error);
    layout->addWidget(buttons);

    connect(m_load, &QPushButton::clicked, this, &SessionDialog::loadSelected);
    connect(m_save, &QPushButton::clicked, this, &SessionDialog::saveCurrent);
    connect(m_delete, &QPushButton::clicked, this, &SessionDialog::deleteSelected);
    connect(m_list, &QListWidget::currentTextChanged, this, [this](const QString &t) {
        if (!t.isEmpty()) {
            m_name->setText(t);
        }
        updateEnabled();
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() {
        if (loadSelected()) {
            openSession();
        }
    });
    connect(m_type, &QComboBox::currentIndexChanged, this, [this]() {
        updateEnabled();
        updatePreview();
    });
    connect(m_overrideFont, &QCheckBox::toggled, this, &SessionDialog::updateEnabled);
    connect(m_useStored, &QCheckBox::toggled, this, &SessionDialog::updateEnabled);
    for (QLineEdit *e : {m_host, m_user, m_key, m_jump, m_extra}) {
        connect(e, &QLineEdit::textChanged, this, &SessionDialog::updatePreview);
    }
    connect(m_port, &QSpinBox::valueChanged, this, &SessionDialog::updatePreview);
    connect(m_keepInterval, &QSpinBox::valueChanged, this, &SessionDialog::updatePreview);
    connect(m_keepCount, &QSpinBox::valueChanged, this, &SessionDialog::updatePreview);

    setConfig(initial);
    refreshList(initial.name);
    resize(sizeHint().expandedTo(QSize(760, 0)));
}

void SessionDialog::refreshList(const QString &select)
{
    const QSignalBlocker block(m_list);
    m_list->clear();
    m_list->addItems(m_store.names());
    const auto found = m_list->findItems(select, Qt::MatchExactly);
    if (!found.isEmpty()) {
        m_list->setCurrentItem(found.front());
    }
    updateEnabled();
}

void SessionDialog::updateEnabled()
{
    const bool ssh = m_type->currentData().toString() == SessionConfig::typeToString(SessionConfig::Type::Ssh);
    const bool serial = m_type->currentData().toString() == SessionConfig::typeToString(SessionConfig::Type::Serial);
    m_sshBox->setEnabled(ssh);
    m_sshBox->setVisible(ssh);
    m_serialBox->setEnabled(serial);
    m_serialBox->setVisible(serial);
    m_font->setEnabled(m_overrideFont->isChecked());
    m_sshPassword->setEnabled(m_useStored->isChecked());
    m_load->setEnabled(m_list->currentItem() != nullptr);
    m_delete->setEnabled(m_list->currentItem() != nullptr);
}

void SessionDialog::updatePreview()
{
    const SessionConfig c = config();
    if (c.type != SessionConfig::Type::Ssh) {
        m_preview->clear();
        return;
    }
    const SshCommand cmd = buildSshCommand(c);
    m_preview->setText(cmd.ok() ? shellQuote(QStringList{cmd.program} + cmd.args) : cmd.error);
    m_preview->setCursorPosition(0);
}

QString SessionDialog::shellQuote(const QStringList &argv)
{
    QStringList out;
    for (const QString &a : argv) {
        static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_./:@%+=,-]+$"));
        if (safe.match(a).hasMatch()) {
            out << a;
        } else {
            QString q = a;
            q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            out << QLatin1Char('\'') + q + QLatin1Char('\'');
        }
    }
    return out.join(QLatin1Char(' '));
}

SessionConfig SessionDialog::config() const
{
    SessionConfig c;
    c.name = m_name->text().trimmed();
    c.type = SessionConfig::typeFromString(m_type->currentData().toString());
    c.autoLog = m_autoLog->isChecked();
    c.autoReconnect = m_autoReconnect->isChecked();
    if (c.type == SessionConfig::Type::Ssh) {
        c.keepaliveInterval = m_keepInterval->value();
        c.keepaliveCountMax = m_keepCount->value();
        c.host = m_host->text().trimmed();
        c.user = m_user->text().trimmed();
        c.port = m_port->value();
        c.keyFile = m_key->text().trimmed();
        c.jumpHost = m_jump->text().trimmed();
        c.extraArgs = m_extra->text().trimmed();
        c.useStoredPassword = m_useStored->isChecked();
    }
    if (c.type == SessionConfig::Type::Serial) {
        c.serialDevice = m_device->currentText().trimmed();
        c.baudRate = m_baud->currentText().toInt();
        c.dataBits = m_dataBits->currentData().toInt();
        c.parity = m_parity->currentData().toString();
        c.stopBits = m_stopBits->currentData().toInt();
        c.flowControl = m_flow->currentData().toString();
        c.localEcho = m_localEcho->isChecked();
        c.enterSends = m_enter->currentData().toString();
        c.charDelayMs = m_charDelay->value();
        c.lineDelayMs = m_lineDelay->value();
        c.breakMs = m_breakMs->value();
        c.loginUser = m_loginUser->text().trimmed();
    }
    if (m_overrideFont->isChecked()) {
        c.fontFamily = m_font->currentFont().family();
    }
    c.fontSize = m_fontSize->value();
    c.colorScheme = m_scheme->currentData().toString();
    return c;
}

void SessionDialog::setConfig(const SessionConfig &s)
{
    m_name->setText(s.name);
    m_type->setCurrentIndex(std::max(0, m_type->findData(SessionConfig::typeToString(s.type))));
    m_autoLog->setChecked(s.autoLog);
    m_autoReconnect->setChecked(s.autoReconnect);
    m_keepInterval->setValue(s.keepaliveInterval);
    m_keepCount->setValue(s.keepaliveCountMax);
    m_keepCount->setEnabled(s.keepaliveInterval > 0);
    m_host->setText(s.host);
    m_user->setText(s.user);
    m_port->setValue(s.port);
    m_key->setText(s.keyFile);
    m_jump->setText(s.jumpHost);
    m_extra->setText(s.extraArgs);
    m_useStored->setChecked(s.useStoredPassword);
    m_sshPassword->clear();
    m_loginUser->setText(s.loginUser);
    m_loginPassword->clear();
    m_device->setCurrentText(s.serialDevice);
    m_baud->setCurrentText(QString::number(s.baudRate));
    m_dataBits->setCurrentIndex(std::max(0, m_dataBits->findData(s.dataBits)));
    m_parity->setCurrentIndex(std::max(0, m_parity->findData(s.parity)));
    m_stopBits->setCurrentIndex(std::max(0, m_stopBits->findData(s.stopBits)));
    m_flow->setCurrentIndex(std::max(0, m_flow->findData(s.flowControl)));
    m_localEcho->setChecked(s.localEcho);
    m_enter->setCurrentIndex(std::max(0, m_enter->findData(s.enterSends)));
    m_charDelay->setValue(s.charDelayMs);
    m_lineDelay->setValue(s.lineDelayMs);
    m_breakMs->setValue(s.breakMs);
    m_overrideFont->setChecked(!s.fontFamily.isEmpty());
    m_font->setCurrentFont(s.fontFamily.isEmpty() ? AppSettings::load().font() : QFont(s.fontFamily));
    m_fontSize->setValue(s.fontSize);
    m_scheme->setCurrentIndex(std::max(0, m_scheme->findData(s.colorScheme)));
    setError({});
    updateEnabled();
    updatePreview();
}

QString SessionDialog::errorText() const
{
    return m_error->text();
}

void SessionDialog::setError(const QString &e)
{
    m_error->setText(e);
    m_error->setVisible(!e.isEmpty());
}

void SessionDialog::refreshPorts()
{
    const QString current = m_device->currentText();
    m_device->clear();
    m_device->addItems(SerialBackend::availablePorts());
    m_device->setCurrentText(current);
}

bool SessionDialog::loadSelected()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item) {
        setError(QStringLiteral("Select a saved session to load."));
        return false;
    }
    const auto s = m_store.load(item->text());
    if (!s) {
        setError(QStringLiteral("Can't read session \"%1\".").arg(item->text()));
        return false;
    }
    setConfig(*s);
    return true;
}

bool SessionDialog::saveCurrent()
{
    const SessionConfig c = config();
    if (const QString e = validateSessionName(c.name); !e.isEmpty()) {
        setError(e);
        return false;
    }
    if (c.type == SessionConfig::Type::Ssh) {
        if (const SshCommand cmd = buildSshCommand(c); !cmd.ok()) {
            setError(cmd.error);
            return false;
        }
    }
    if (c.type == SessionConfig::Type::Serial) {
        if (const QString e = validateSerial(c); !e.isEmpty()) {
            setError(e);
            return false;
        }
    }
    QString err;
    if (!m_store.save(c, &err)) {
        setError(err);
        return false;
    }
    setError({});
    refreshList(c.name);
    return storeSecrets(c);
}

bool SessionDialog::storeSecrets(const SessionConfig &c)
{
    QLineEdit *field = nullptr;
    QString key;
    if (c.type == SessionConfig::Type::Ssh) {
        field = m_sshPassword;
        key = Vault::secretKeyFor(QStringLiteral("ssh-password"), c.name);
    } else if (c.type == SessionConfig::Type::Serial) {
        field = m_loginPassword;
        key = Vault::secretKeyFor(QStringLiteral("serial-password"), c.name);
    } else {
        return true;
    }
    VaultManager &vm = VaultManager::instance();
    if (c.type == SessionConfig::Type::Ssh && !c.useStoredPassword) {
        // Unticked: forget a stored password if the vault is open (don't prompt just for this).
        if (vm.isUnlocked() && vm.vault().secret(key)) {
            vm.vault().removeSecret(key);
        }
        field->clear();
        return true;
    }
    if (field->text().isEmpty()) {
        return true; // "(unchanged)"
    }
    if (!vm.ensureUnlocked(this, QStringLiteral("Saving the password for \"%1\".").arg(c.name.toHtmlEscaped()), true)) {
        field->clear();
        setError(QStringLiteral("Session saved, but its password was NOT stored: the vault is locked."));
        return false;
    }
    SecureBuffer secret = SecureBuffer::fromQString(field->text());
    field->clear();
    if (!vm.vault().setSecret(key, std::move(secret))) {
        setError(QStringLiteral("Session saved, but storing its password failed: %1").arg(vm.vault().lastError()));
        return false;
    }
    return true;
}

bool SessionDialog::deleteSelected()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item) {
        return false;
    }
    const QString name = item->text();
    if (!m_store.remove(name)) {
        setError(QStringLiteral("Can't delete \"%1\".").arg(name));
        return false;
    }
    // Forget its stored passwords too when the vault is open.
    VaultManager &vm = VaultManager::instance();
    if (vm.isUnlocked()) {
        vm.vault().removeSecret(Vault::secretKeyFor(QStringLiteral("ssh-password"), name));
        vm.vault().removeSecret(Vault::secretKeyFor(QStringLiteral("serial-password"), name));
    }
    setError({});
    refreshList();
    return true;
}

bool SessionDialog::openSession()
{
    const SessionConfig c = config();
    if (!m_sshPassword->text().isEmpty() || !m_loginPassword->text().isEmpty()) {
        setError(QStringLiteral("Press Save to store the password in the vault first (Open never stores it)."));
        return false;
    }
    if (c.type == SessionConfig::Type::Serial) {
        if (const QString e = validateSerial(c); !e.isEmpty()) {
            setError(e);
            return false;
        }
    }
    if (c.type == SessionConfig::Type::Ssh) {
        if (const SshCommand cmd = buildSshCommand(c); !cmd.ok()) {
            setError(cmd.error);
            return false;
        }
    }
    setError({});
    accept();
    return true;
}

} // namespace zterminal
