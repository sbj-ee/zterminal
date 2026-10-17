#include "GroupPicker.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace zterminal {

GroupPicker::GroupPicker(const GroupStore &groups, QWidget *parent)
    : QDialog(parent)
    , m_groups(groups)
{
    setObjectName(QStringLiteral("groupPicker"));
    setWindowTitle(QStringLiteral("Open Group"));
    m_filter = new QLineEdit;
    m_filter->setObjectName(QStringLiteral("groupFilter"));
    m_filter->setPlaceholderText(QStringLiteral("Type to filter groups"));
    m_filter->setClearButtonEnabled(true);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("groupPickerList"));
    m_list->setFocusPolicy(Qt::NoFocus); // the filter box keeps the keyboard
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_filter);
    layout->addWidget(m_list, 1);
    auto *hint = new QLabel(QStringLiteral("Enter opens the group \u2022 Esc cancels"));
    hint->setEnabled(false);
    layout->addWidget(hint);

    m_filter->installEventFilter(this);
    connect(m_filter, &QLineEdit::textChanged, this, &GroupPicker::applyFilter);
    connect(m_filter, &QLineEdit::returnPressed, this, &GroupPicker::choose);
    connect(m_list, &QListWidget::itemActivated, this, &GroupPicker::choose);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &GroupPicker::choose);
    applyFilter({});
    resize(360, 300);
    m_filter->setFocus();
}

QStringList GroupPicker::visibleGroups() const
{
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i) {
        out << m_list->item(i)->data(Qt::UserRole).toString();
    }
    return out;
}

void GroupPicker::applyFilter(const QString &text)
{
    m_list->clear();
    for (const QString &n : m_groups.names()) {
        if (!text.isEmpty() && !n.contains(text.trimmed(), Qt::CaseInsensitive)) {
            continue;
        }
        const auto g = m_groups.load(n);
        auto *item = new QListWidgetItem(QStringLiteral("%1  (%2 %3)")
                                             .arg(n)
                                             .arg(g ? g->members.size() : 0)
                                             .arg(g && g->members.size() == 1 ? QStringLiteral("session") : QStringLiteral("sessions")));
        item->setData(Qt::UserRole, n);
        m_list->addItem(item);
    }
    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
}

void GroupPicker::choose()
{
    if (const QListWidgetItem *it = m_list->currentItem()) {
        m_chosen = it->data(Qt::UserRole).toString();
        accept();
    }
}

bool GroupPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if ((key == Qt::Key_Down || key == Qt::Key_Up) && m_list->count() > 0) {
            const int row = m_list->currentRow() + (key == Qt::Key_Down ? 1 : -1);
            m_list->setCurrentRow(qBound(0, row, m_list->count() - 1));
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace zterminal
