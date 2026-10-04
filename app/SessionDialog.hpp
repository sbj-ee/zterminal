#pragma once

#include "Session.hpp"
#include "SessionStore.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QWidget;

namespace zterminal {

// PuTTY-style session dialog: saved sessions on the left (Load / Save / Delete),
// connection settings on the right, Open starts the session in a new window.
class SessionDialog : public QDialog
{
    Q_OBJECT
public:
    SessionDialog(const SessionStore &store, const SessionConfig &initial, QWidget *parent = nullptr);

    SessionConfig config() const;
    void setConfig(const SessionConfig &s);
    QString errorText() const;

    // Button actions (public for tests). Each returns false and shows the reason
    // in the dialog on failure.
    bool loadSelected();
    bool saveCurrent();
    bool deleteSelected();
    // Validates; on success accept()s.
    bool openSession();

    static QString shellQuote(const QStringList &argv); // display only

private:
    void refreshList(const QString &select = {});
    void updateEnabled();
    void updatePreview();
    void setError(const QString &e);
    void refreshPorts();
    // Writes the typed password(s) to the vault (unlocking/creating it if
    // needed) and clears the fields. Never touches the session file.
    bool storeSecrets(const SessionConfig &c);

    SessionStore m_store;
    QLineEdit *m_name = nullptr;
    QListWidget *m_list = nullptr;
    QPushButton *m_load = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_delete = nullptr;
    QComboBox *m_type = nullptr;
    QWidget *m_sshBox = nullptr;
    QLineEdit *m_host = nullptr;
    QLineEdit *m_user = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_key = nullptr;
    QLineEdit *m_jump = nullptr;
    QLineEdit *m_extra = nullptr;
    QCheckBox *m_useStored = nullptr;
    QCheckBox *m_autoLog = nullptr;
    QLineEdit *m_sshPassword = nullptr;
    QLineEdit *m_loginUser = nullptr;
    QLineEdit *m_loginPassword = nullptr;
    QLineEdit *m_preview = nullptr;
    QWidget *m_serialBox = nullptr;
    QComboBox *m_device = nullptr;
    QComboBox *m_baud = nullptr;
    QComboBox *m_dataBits = nullptr;
    QComboBox *m_parity = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_flow = nullptr;
    QCheckBox *m_localEcho = nullptr;
    QComboBox *m_enter = nullptr;
    QSpinBox *m_charDelay = nullptr;
    QSpinBox *m_lineDelay = nullptr;
    QSpinBox *m_breakMs = nullptr;
    QCheckBox *m_overrideFont = nullptr;
    QFontComboBox *m_font = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QComboBox *m_scheme = nullptr;
    QLabel *m_error = nullptr;
};

} // namespace zterminal
