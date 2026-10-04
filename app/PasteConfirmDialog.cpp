#include "PasteConfirmDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace zterminal {

namespace {
QString seconds(qint64 ms)
{
    if (ms < 1000) {
        return QStringLiteral("under a second");
    }
    const qint64 s = (ms + 500) / 1000;
    return s < 120 ? QStringLiteral("about %1 s").arg(s) : QStringLiteral("about %1 min").arg((s + 30) / 60);
}
} // namespace

PasteConfirmDialog::PasteConfirmDialog(const PasteInfo &info, const Context &ctx, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("pasteConfirmDialog"));
    setWindowTitle(QStringLiteral("Confirm paste"));
    auto *layout = new QVBoxLayout(this);

    const QString what = info.lines == 1 ? QStringLiteral("1 line") : QStringLiteral("%1 lines").arg(info.lines);
    auto *head = new QLabel(QStringLiteral("<b>Paste %1 (%2 bytes)?</b>").arg(what).arg(info.bytes));
    head->setObjectName(QStringLiteral("pasteSummary"));
    layout->addWidget(head);

    QStringList notes;
    if (ctx.bracketedPaste) {
        notes << QStringLiteral("The program turned on bracketed paste, so it receives this as one paste "
                                "(a shell waits for Enter).");
    } else if (info.endsWithNewline || info.lines > 1) {
        notes << QStringLiteral("Each line break acts like pressing Enter: lines may run as they arrive.");
    }
    if (ctx.serialPaced) {
        notes << QStringLiteral("Sent with serial pacing (%1 ms/char, %2 ms/line): %3. Cancel from the bar "
                                "or Session > Cancel Paste.")
                     .arg(ctx.charDelayMs)
                     .arg(ctx.lineDelayMs)
                     .arg(seconds(ctx.pacedMs));
    }
    if (info.controlChars > 0) {
        notes << QStringLiteral("Contains %1 control character(s), shown as symbols below.").arg(info.controlChars);
    }
    if (!notes.isEmpty()) {
        auto *n = new QLabel(notes.join(QStringLiteral("\n")));
        n->setObjectName(QStringLiteral("pasteNotes"));
        n->setWordWrap(true);
        layout->addWidget(n);
    }

    auto *preview = new QPlainTextEdit;
    preview->setObjectName(QStringLiteral("pastePreview"));
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    QString shown = info.preview;
    if (info.previewLines < info.lines) {
        shown += QStringLiteral("\n\u2026 %1 more line(s)").arg(info.lines - info.previewLines);
    }
    preview->setPlainText(shown);
    preview->setMinimumSize(520, 200);
    layout->addWidget(preview, 1);

    m_dontAsk = new QCheckBox(QStringLiteral("Don't ask again for this session"));
    m_dontAsk->setObjectName(QStringLiteral("dontAskAgain"));
    layout->addWidget(m_dontAsk);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->setObjectName(QStringLiteral("buttons"));
    QPushButton *paste = buttons->addButton(QStringLiteral("&Paste"), QDialogButtonBox::AcceptRole);
    paste->setObjectName(QStringLiteral("pasteButton"));
    // Cancel is the default: an Enter still held from the shell doesn't paste.
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setFocus();
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

bool PasteConfirmDialog::dontAskAgain() const
{
    return m_dontAsk->isChecked();
}

} // namespace zterminal
