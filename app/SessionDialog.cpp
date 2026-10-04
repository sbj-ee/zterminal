#include "SessionDialog.hpp"

#include "AppSettings.hpp"
#include "ColorScheme.hpp"

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
    auto *where = new QLabel(QStringLiteral("<small>Stored in %1 (no passwords)</small>")
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
    m_type->addItem(QStringLiteral("Serial (coming soon)"), SessionConfig::typeToString(SessionConfig::Type::Serial));
    if (auto *model = qobject_cast<QStandardItemModel *>(m_type->model())) {
        model->item(2)->setEnabled(false);
    }
    typeForm->addRow(QStringLiteral("Type:"), m_type);
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
    m_extra = lineEdit("extraArgs", QStringLiteral("e.g. -o ServerAliveInterval=30 -4"));
    m_extra->setToolTip(QStringLiteral("ssh options only. Split like a command line (use double quotes for spaces); "
                                       "never passed to a shell."));
    sshForm->addRow(QStringLiteral("Extra options:"), m_extra);
    // Read-only line edit: long commands scroll instead of being clipped.
    m_preview = new QLineEdit;
    m_preview->setObjectName(QStringLiteral("commandPreview"));
    m_preview->setReadOnly(true);
    m_preview->setFrame(false);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_preview->setToolTip(QStringLiteral("The exact argv zterminal runs (no shell is involved)"));
    sshForm->addRow(QStringLiteral("Runs:"), m_preview);
    connLayout->addWidget(m_sshBox);

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
    for (QLineEdit *e : {m_host, m_user, m_key, m_jump, m_extra}) {
        connect(e, &QLineEdit::textChanged, this, &SessionDialog::updatePreview);
    }
    connect(m_port, &QSpinBox::valueChanged, this, &SessionDialog::updatePreview);

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
    m_sshBox->setEnabled(ssh);
    m_font->setEnabled(m_overrideFont->isChecked());
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
    if (c.type == SessionConfig::Type::Ssh) {
        c.host = m_host->text().trimmed();
        c.user = m_user->text().trimmed();
        c.port = m_port->value();
        c.keyFile = m_key->text().trimmed();
        c.jumpHost = m_jump->text().trimmed();
        c.extraArgs = m_extra->text().trimmed();
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
    m_host->setText(s.host);
    m_user->setText(s.user);
    m_port->setValue(s.port);
    m_key->setText(s.keyFile);
    m_jump->setText(s.jumpHost);
    m_extra->setText(s.extraArgs);
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
    QString err;
    if (!m_store.save(c, &err)) {
        setError(err);
        return false;
    }
    setError({});
    refreshList(c.name);
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
    setError({});
    refreshList();
    return true;
}

bool SessionDialog::openSession()
{
    const SessionConfig c = config();
    if (c.type == SessionConfig::Type::Serial) {
        setError(QStringLiteral("Serial sessions are coming in a later release."));
        return false;
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
