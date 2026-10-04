#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace zterminal {

class Vault;

// All three dialogs do the vault operation themselves when OK is pressed and
// stay open (showing the reason) if it fails. Password fields are cleared as
// soon as their contents have been copied into secure memory. Residual risk:
// QLineEdit keeps its own (non-locked) QString while text is typed; see README.

class UnlockVaultDialog : public QDialog
{
    Q_OBJECT
public:
    UnlockVaultDialog(Vault &vault, const QString &why, QWidget *parent = nullptr);
    bool tryUnlock(); // public for tests
    QString errorText() const;

private:
    Vault &m_vault;
    QLineEdit *m_password = nullptr;
    QLabel *m_error = nullptr;
};

class CreateVaultDialog : public QDialog
{
    Q_OBJECT
public:
    static constexpr int kMinLength = 8;
    CreateVaultDialog(Vault &vault, QWidget *parent = nullptr);
    bool tryCreate();
    QString errorText() const;

private:
    void updateEnabled();
    Vault &m_vault;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_confirm = nullptr;
    QCheckBox *m_understand = nullptr;
    QPushButton *m_create = nullptr;
    QLabel *m_hint = nullptr;
    QLabel *m_error = nullptr;
};

class ChangeMasterPasswordDialog : public QDialog
{
    Q_OBJECT
public:
    ChangeMasterPasswordDialog(Vault &vault, QWidget *parent = nullptr);
    bool tryChange();
    QString errorText() const;

private:
    void updateEnabled();
    Vault &m_vault;
    QLineEdit *m_current = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_confirm = nullptr;
    QPushButton *m_change = nullptr;
    QLabel *m_error = nullptr;
};

// The warning shown when creating a vault (also used in tests/README).
QString noRecoveryWarning();

} // namespace zterminal
