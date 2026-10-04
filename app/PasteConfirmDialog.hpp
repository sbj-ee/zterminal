#pragma once

#include "PasteGuard.hpp"

#include <QDialog>

class QCheckBox;

namespace zterminal {

// "Paste N lines?" with a read-only preview, notes on bracketed paste, serial
// pacing and control characters, and "Don't ask again for this session".
class PasteConfirmDialog : public QDialog {
    Q_OBJECT
public:
    struct Context {
        bool bracketedPaste = false;
        bool serialPaced = false;
        int charDelayMs = 0;
        int lineDelayMs = 0;
        qint64 pacedMs = 0;
    };
    PasteConfirmDialog(const PasteInfo &info, const Context &ctx, QWidget *parent = nullptr);
    bool dontAskAgain() const;

private:
    QCheckBox *m_dontAsk = nullptr;
};

} // namespace zterminal
