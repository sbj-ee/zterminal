#include "ImportReviewDialog.hpp"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace zterminal {

ImportReviewDialog::ImportReviewDialog(const QList<Entry> &entries, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("importReview"));
    setWindowTitle(QStringLiteral("Review Imported Sessions"));

    for (const Entry &e : entries) {
        if (e.refusal.isEmpty() && !e.skipped) {
            ++m_importable;
        }
    }

    auto *intro = new QLabel(QStringLiteral(
        "Check these sessions before importing them. An import file can come from anyone: make sure every "
        "host, jump host and extra ssh option is one you expect.\n\n"
        "Passwords are never imported, and \"Use stored password\" is switched off on every imported or "
        "replaced session, so nothing from your vault is sent because of this import. Tick it again "
        "afterwards for sessions you trust."));
    intro->setWordWrap(true);

    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("importReviewList"));
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("Session"), QStringLiteral("What it does")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    for (const Entry &e : entries) {
        QString status;
        if (!e.refusal.isEmpty()) {
            status = QStringLiteral("REFUSED: ") + e.refusal;
        } else if (e.skipped) {
            status = QStringLiteral("skipped (a saved session with this name is kept)");
        } else if (e.replaces) {
            status = QStringLiteral("REPLACES the saved session with this name");
        } else {
            status = QStringLiteral("new");
        }
        auto *top = new QTreeWidgetItem(m_tree, {e.session.name, status});
        if (!e.refusal.isEmpty() || e.replaces) {
            QFont f = top->font(1);
            f.setBold(true);
            top->setFont(1, f);
        }
        for (const QString &line : notableSessionSettings(e.session)) {
            new QTreeWidgetItem(top, {QString(), line});
        }
        top->setExpanded(true);
    }

    auto *buttons = new QDialogButtonBox;
    if (m_importable > 0) {
        m_approve = buttons->addButton(
            m_importable == 1 ? QStringLiteral("&Import 1 Session") : QStringLiteral("&Import %1 Sessions").arg(m_importable),
            QDialogButtonBox::AcceptRole);
        m_approve->setObjectName(QStringLiteral("approveImport"));
        m_approve->setAutoDefault(false);
    }
    QPushButton *cancel = buttons->addButton(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("cancelImport"));
    cancel->setDefault(true); // Enter must not approve by accident
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_tree, 1);
    layout->addWidget(buttons);
    resize(720, 460);
}

} // namespace zterminal
