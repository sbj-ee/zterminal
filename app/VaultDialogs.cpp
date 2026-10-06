#include "VaultDialogs.hpp"

#include "SecureBuffer.hpp"
#include "Vault.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace zterminal {

namespace {

QLineEdit *passwordEdit(const char *name)
{
    auto *e = new QLineEdit;
    e->setObjectName(QString::fromLatin1(name));
    e->setEchoMode(QLineEdit::Password);
    // No undo history, no drag-out, no context-menu copy of the secret.
    e->setContextMenuPolicy(Qt::NoContextMenu);
    e->setDragEnabled(false);
    e->setMinimumWidth(260);
    // Keep input methods from learning/suggesting it (Password echo sets these too; be explicit).
    e->setInputMethodHints(e->inputMethodHints() | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhHiddenText);
    return e;
}

QLabel *errorLabel()
{
    auto *l = new QLabel;
    l->setObjectName(QStringLiteral("error"));
    l->setStyleSheet(QStringLiteral("color: #c0392b"));
    l->setWordWrap(true);
    l->hide();
    return l;
}

void showError(QLabel *l, const QString &e)
{
    l->setText(e);
    l->setVisible(!e.isEmpty());
}

// Copies the field straight into secure memory, then clears the field.
SecureBuffer takeSecret(QLineEdit *e)
{
    SecureBuffer b = SecureBuffer::fromQString(e->text());
    e->clear();
    return b;
}

QString tildePath(const QString &p)
{
    QString s = QDir::toNativeSeparators(p);
    if (s.startsWith(QDir::homePath())) {
        s.replace(0, QDir::homePath().size(), QStringLiteral("~"));
    }
    return s;
}

struct WaitCursor {
    WaitCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
    ~WaitCursor() { QApplication::restoreOverrideCursor(); }
};

} // namespace

QString noRecoveryWarning()
{
    return QStringLiteral(
        "<b>There is NO recovery.</b> If you forget the master password, every password stored in "
        "the vault is lost for good: nobody, including zterminal, can decrypt it or reset it.");
}

// --- Unlock ---------------------------------------------------------------

UnlockVaultDialog::UnlockVaultDialog(Vault &vault, const QString &why, QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
{
    setWindowTitle(QStringLiteral("Unlock Password Vault"));
    setObjectName(QStringLiteral("unlockVaultDialog"));
    // Vault is process-wide: unlock must not be WindowModal to one tab's window.
    setWindowModality(Qt::ApplicationModal);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(why.isEmpty() ? QStringLiteral("Enter the master password to unlock the vault.")
                                           : why + QStringLiteral("<br>Enter the master password to unlock the vault."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    m_password = passwordEdit("masterPassword");
    form->addRow(QStringLiteral("Master password:"), m_password);
    layout->addLayout(form);
    auto *where = new QLabel(QStringLiteral("<small>%1</small>").arg(tildePath(vault.path()).toHtmlEscaped()));
    layout->addWidget(where);
    m_error = errorLabel();
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox;
    QPushButton *ok = buttons->addButton(QStringLiteral("&Unlock"), QDialogButtonBox::AcceptRole);
    ok->setObjectName(QStringLiteral("unlock"));
    ok->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &UnlockVaultDialog::tryUnlock);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    m_password->setFocus();
}

QString UnlockVaultDialog::errorText() const
{
    return m_error->isVisible() || !m_error->text().isEmpty() ? m_error->text() : QString();
}

bool UnlockVaultDialog::tryUnlock()
{
    const SecureBuffer pw = takeSecret(m_password);
    bool ok;
    {
        WaitCursor wc;
        ok = m_vault.unlock(pw);
    }
    if (!ok) {
        showError(m_error, m_vault.lastError());
        m_password->setFocus();
        return false;
    }
    showError(m_error, {});
    accept();
    return true;
}

// --- Create ---------------------------------------------------------------

CreateVaultDialog::CreateVaultDialog(Vault &vault, QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
{
    setWindowTitle(QStringLiteral("Create Password Vault"));
    setObjectName(QStringLiteral("createVaultDialog"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral(
        "The vault keeps session passwords encrypted (Argon2id + XChaCha20-Poly1305) in<br><tt>%1</tt>.<br>"
        "SSH keys with ssh-agent remain the better choice where you can use them.")
                                 .arg(tildePath(vault.path()).toHtmlEscaped()));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *warn = new QLabel(noRecoveryWarning());
    warn->setObjectName(QStringLiteral("noRecoveryWarning"));
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("QLabel { background: #7b1d1d; color: white; padding: 8px; border-radius: 4px; }"));
    layout->addWidget(warn);
    auto *form = new QFormLayout;
    m_password = passwordEdit("masterPassword");
    m_confirm = passwordEdit("confirmPassword");
    form->addRow(QStringLiteral("Master password:"), m_password);
    form->addRow(QStringLiteral("Confirm:"), m_confirm);
    layout->addLayout(form);
    m_hint = new QLabel;
    m_hint->setObjectName(QStringLiteral("hint"));
    layout->addWidget(m_hint);
    m_understand = new QCheckBox(QStringLiteral("I understand that a forgotten master password cannot be recovered"));
    m_understand->setObjectName(QStringLiteral("understandNoRecovery"));
    layout->addWidget(m_understand);
    m_error = errorLabel();
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox;
    m_create = buttons->addButton(QStringLiteral("&Create Vault"), QDialogButtonBox::AcceptRole);
    m_create->setObjectName(QStringLiteral("create"));
    m_create->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &CreateVaultDialog::tryCreate);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    for (QLineEdit *e : {m_password, m_confirm}) {
        connect(e, &QLineEdit::textChanged, this, &CreateVaultDialog::updateEnabled);
    }
    connect(m_understand, &QCheckBox::toggled, this, &CreateVaultDialog::updateEnabled);
    updateEnabled();
    m_password->setFocus();
}

void CreateVaultDialog::updateEnabled()
{
    // Length/equality checks read the line edits' own buffers (no copies kept).
    const qsizetype len = m_password->text().size();
    const bool match = m_password->text() == m_confirm->text();
    QString hint;
    if (len > 0 && len < kMinLength) {
        hint = QStringLiteral("At least %1 characters.").arg(kMinLength);
    } else if (!m_confirm->text().isEmpty() && !match) {
        hint = QStringLiteral("The passwords don't match.");
    }
    m_hint->setText(hint);
    m_create->setEnabled(len >= kMinLength && match && m_understand->isChecked());
}

QString CreateVaultDialog::errorText() const
{
    return m_error->text();
}

bool CreateVaultDialog::tryCreate()
{
    updateEnabled();
    if (!m_create->isEnabled()) {
        showError(m_error, QStringLiteral("Enter the password twice (at least %1 characters) and tick the box.").arg(kMinLength));
        return false;
    }
    const SecureBuffer pw = takeSecret(m_password);
    m_confirm->clear();
    bool ok;
    {
        WaitCursor wc;
        ok = m_vault.create(pw);
    }
    if (!ok) {
        showError(m_error, m_vault.lastError());
        return false;
    }
    accept();
    return true;
}

// --- Change master password ---------------------------------------------

ChangeMasterPasswordDialog::ChangeMasterPasswordDialog(Vault &vault, QWidget *parent)
    : QDialog(parent)
    , m_vault(vault)
{
    setWindowTitle(QStringLiteral("Change Master Password"));
    setObjectName(QStringLiteral("changeMasterPasswordDialog"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral(
        "The vault is re-encrypted under a new key (new salt) and replaced atomically.<br>") + noRecoveryWarning());
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    m_current = passwordEdit("currentPassword");
    m_password = passwordEdit("newPassword");
    m_confirm = passwordEdit("confirmPassword");
    form->addRow(QStringLiteral("Current password:"), m_current);
    form->addRow(QStringLiteral("New password:"), m_password);
    form->addRow(QStringLiteral("Confirm new:"), m_confirm);
    layout->addLayout(form);
    m_error = errorLabel();
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox;
    m_change = buttons->addButton(QStringLiteral("C&hange"), QDialogButtonBox::AcceptRole);
    m_change->setObjectName(QStringLiteral("change"));
    m_change->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &ChangeMasterPasswordDialog::tryChange);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    for (QLineEdit *e : {m_current, m_password, m_confirm}) {
        connect(e, &QLineEdit::textChanged, this, &ChangeMasterPasswordDialog::updateEnabled);
    }
    updateEnabled();
}

void ChangeMasterPasswordDialog::updateEnabled()
{
    m_change->setEnabled(!m_current->text().isEmpty() && m_password->text().size() >= CreateVaultDialog::kMinLength
                         && m_password->text() == m_confirm->text());
}

QString ChangeMasterPasswordDialog::errorText() const
{
    return m_error->text();
}

bool ChangeMasterPasswordDialog::tryChange()
{
    updateEnabled();
    if (!m_change->isEnabled()) {
        showError(m_error, QStringLiteral("Enter the current password and the new one twice (at least %1 characters).")
                               .arg(CreateVaultDialog::kMinLength));
        return false;
    }
    const SecureBuffer oldPw = takeSecret(m_current);
    const SecureBuffer newPw = takeSecret(m_password);
    m_confirm->clear();
    bool ok;
    {
        WaitCursor wc;
        ok = m_vault.changePassword(oldPw, newPw);
    }
    if (!ok) {
        showError(m_error, m_vault.lastError());
        return false;
    }
    accept();
    return true;
}

} // namespace zterminal
