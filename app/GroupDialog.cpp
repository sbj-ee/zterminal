#include "GroupDialog.hpp"

#include "Session.hpp"

#include <QBrush>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace zterminal {

namespace {
QPushButton *button(const char *name, const QString &text)
{
    auto *b = new QPushButton(text);
    b->setObjectName(QString::fromLatin1(name));
    return b;
}
} // namespace

GroupDialog::GroupDialog(const SessionStore &sessions, const GroupStore &groups, QWidget *parent)
    : QDialog(parent)
    , m_sessions(sessions)
    , m_groups(groups)
{
    setObjectName(QStringLiteral("groupDialog"));
    setWindowTitle(QStringLiteral("Manage Groups"));

    // Saved groups.
    auto *listBox = new QGroupBox(QStringLiteral("Saved groups"));
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("groupList"));
    m_new = button("newGroup", QStringLiteral("&New"));
    m_delete = button("deleteGroup", QStringLiteral("&Delete"));
    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(m_new);
    listButtons->addWidget(m_delete);
    auto *listLayout = new QVBoxLayout(listBox);
    listLayout->addWidget(m_list, 1);
    listLayout->addLayout(listButtons);

    // The selected group.
    auto *editBox = new QGroupBox(QStringLiteral("Group"));
    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("groupName"));
    m_name->setPlaceholderText(QStringLiteral("name, e.g. lab"));
    m_openIn = new QComboBox;
    m_openIn->setObjectName(QStringLiteral("openIn"));
    m_openIn->addItem(QStringLiteral("A new window"), int(GroupConfig::OpenIn::NewWindow));
    m_openIn->addItem(QStringLiteral("This window"), int(GroupConfig::OpenIn::ThisWindow));
    m_stagger = new QSpinBox;
    m_stagger->setObjectName(QStringLiteral("stagger"));
    m_stagger->setRange(0, GroupConfig::kMaxStaggerMs);
    m_stagger->setSingleStep(50);
    m_stagger->setSuffix(QStringLiteral(" ms"));
    m_stagger->setValue(GroupConfig::kDefaultStaggerMs);
    m_stagger->setToolTip(QStringLiteral("Delay between starting one session and the next"));
    m_focus = new QSpinBox;
    m_focus->setObjectName(QStringLiteral("focusIndex"));
    m_focus->setRange(1, 1);
    m_focus->setToolTip(QStringLiteral("The tab that has the focus once the group is open (1 = the first)"));
    m_members = new QListWidget;
    m_members->setObjectName(QStringLiteral("memberList"));
    m_members->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_members->setDragDropMode(QAbstractItemView::InternalMove);
    m_members->setDefaultDropAction(Qt::MoveAction);
    m_add = button("addSession", QStringLiteral("&Add Session\u2026"));
    m_remove = button("removeMember", QStringLiteral("&Remove"));
    m_up = button("moveUp", QStringLiteral("Move &Up"));
    m_down = button("moveDown", QStringLiteral("Move Do&wn"));
    auto *memberButtons = new QVBoxLayout;
    memberButtons->addWidget(m_add);
    memberButtons->addWidget(m_remove);
    memberButtons->addWidget(m_up);
    memberButtons->addWidget(m_down);
    memberButtons->addStretch(1);
    auto *memberRow = new QHBoxLayout;
    memberRow->addWidget(m_members, 1);
    memberRow->addLayout(memberButtons);
    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Name:"), m_name);
    form->addRow(QStringLiteral("Open in:"), m_openIn);
    form->addRow(QStringLiteral("Start each session after:"), m_stagger);
    form->addRow(QStringLiteral("Focus tab:"), m_focus);
    auto *editLayout = new QVBoxLayout(editBox);
    editLayout->addLayout(form);
    editLayout->addWidget(new QLabel(QStringLiteral("Sessions, in tab order (drag to reorder):")));
    editLayout->addLayout(memberRow, 1);

    m_error = new QLabel;
    m_error->setObjectName(QStringLiteral("error"));
    m_error->setStyleSheet(QStringLiteral("color: #c0392b"));
    m_error->setWordWrap(true);
    m_error->hide();

    m_open = button("openGroup", QStringLiteral("&Open Group"));
    m_open->setDefault(true);
    m_save = button("saveGroup", QStringLiteral("&Save"));
    auto *close = button("close", QStringLiteral("&Close"));
    auto *bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(m_open);
    bottom->addWidget(m_save);
    bottom->addWidget(close);

    auto *columns = new QHBoxLayout;
    columns->addWidget(listBox, 1);
    columns->addWidget(editBox, 2);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(columns, 1);
    layout->addWidget(m_error);
    layout->addLayout(bottom);

    connect(m_new, &QPushButton::clicked, this, &GroupDialog::newGroup);
    connect(m_delete, &QPushButton::clicked, this, &GroupDialog::deleteSelected);
    connect(m_save, &QPushButton::clicked, this, &GroupDialog::saveCurrent);
    connect(m_open, &QPushButton::clicked, this, &GroupDialog::openGroup);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_add, &QPushButton::clicked, this, &GroupDialog::pickSessions);
    connect(m_remove, &QPushButton::clicked, this, &GroupDialog::removeSelectedMembers);
    connect(m_up, &QPushButton::clicked, this, [this]() { moveSelected(-1); });
    connect(m_down, &QPushButton::clicked, this, [this]() { moveSelected(+1); });
    connect(m_list, &QListWidget::currentTextChanged, this, [this](const QString &t) {
        if (const auto g = t.isEmpty() ? std::nullopt : m_groups.load(t)) {
            setConfig(*g);
            setError({});
        }
        updateEnabled();
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() { openGroup(); });
    connect(m_members->model(), &QAbstractItemModel::rowsInserted, this, &GroupDialog::updateEnabled);
    connect(m_members->model(), &QAbstractItemModel::rowsRemoved, this, &GroupDialog::updateEnabled);
    connect(m_members->model(), &QAbstractItemModel::rowsMoved, this, &GroupDialog::updateEnabled);
    connect(m_members, &QListWidget::itemSelectionChanged, this, &GroupDialog::updateEnabled);
    connect(m_name, &QLineEdit::textChanged, this, &GroupDialog::updateEnabled);

    refreshList();
    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
    updateEnabled();
    resize(sizeHint().expandedTo(QSize(720, 420)));
}

void GroupDialog::refreshList(const QString &select)
{
    const QSignalBlocker block(m_list);
    m_list->clear();
    m_list->addItems(m_groups.names());
    const auto found = m_list->findItems(select, Qt::MatchExactly);
    if (!select.isEmpty() && !found.isEmpty()) {
        m_list->setCurrentItem(found.front());
    }
    updateEnabled();
}

void GroupDialog::addMemberItem(const QString &name)
{
    auto *item = new QListWidgetItem;
    item->setData(Qt::UserRole, name);
    if (m_sessions.contains(name)) {
        item->setText(name);
    } else {
        item->setText(QStringLiteral("%1  (missing)").arg(name));
        item->setForeground(QBrush(QColor(0xc0, 0x39, 0x2b)));
        item->setToolTip(QStringLiteral("No saved session has this name; it is skipped when the group opens."));
    }
    m_members->addItem(item);
}

void GroupDialog::setMembers(const QStringList &names)
{
    m_members->clear();
    for (const QString &n : names) {
        addMemberItem(n);
    }
}

QStringList GroupDialog::memberNames() const
{
    QStringList out;
    for (int i = 0; i < m_members->count(); ++i) {
        out << m_members->item(i)->data(Qt::UserRole).toString();
    }
    return out;
}

GroupConfig GroupDialog::config() const
{
    GroupConfig g;
    g.name = m_name->text().trimmed();
    g.openIn = static_cast<GroupConfig::OpenIn>(m_openIn->currentData().toInt());
    g.staggerMs = m_stagger->value();
    g.focusIndex = m_focus->value() - 1;
    g.members = memberNames();
    return g;
}

void GroupDialog::setConfig(const GroupConfig &g)
{
    m_name->setText(g.name);
    m_openIn->setCurrentIndex(std::max(0, m_openIn->findData(int(g.openIn))));
    m_stagger->setValue(g.staggerMs);
    setMembers(g.members);
    m_focus->setValue(std::clamp(g.focusIndex + 1, 1, m_focus->maximum()));
    updateEnabled();
}

QString GroupDialog::errorText() const
{
    return m_error->isHidden() ? QString() : m_error->text();
}

void GroupDialog::setError(const QString &e)
{
    m_error->setText(e);
    m_error->setVisible(!e.isEmpty());
}

void GroupDialog::updateEnabled()
{
    const int n = m_members->count();
    const int focus = m_focus->value();
    m_focus->setRange(1, std::max(1, n));
    m_focus->setValue(std::min(focus, std::max(1, n)));
    const bool sel = !m_members->selectedItems().isEmpty();
    m_remove->setEnabled(sel);
    m_up->setEnabled(sel);
    m_down->setEnabled(sel);
    m_delete->setEnabled(m_list->currentItem() != nullptr);
    m_open->setEnabled(n > 0);
}

void GroupDialog::newGroup()
{
    m_list->clearSelection();
    m_list->setCurrentItem(nullptr);
    setConfig(GroupConfig{});
    setError({});
    m_name->setFocus();
}

void GroupDialog::addSessions(const QStringList &names)
{
    for (const QString &n : names) {
        addMemberItem(n);
    }
    updateEnabled();
}

void GroupDialog::pickSessions()
{
    const QStringList all = m_sessions.names();
    if (all.isEmpty()) {
        setError(QStringLiteral("There are no saved sessions yet. Create one in File > New Session first."));
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("sessionPicker"));
    dlg.setWindowTitle(QStringLiteral("Add Sessions to Group"));
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("pickerList"));
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->addItems(all);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(QStringLiteral("Select one or more saved sessions:")));
    lay->addWidget(list, 1);
    lay->addWidget(buttons);
    dlg.resize(320, 360);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    QStringList chosen;
    for (QListWidgetItem *it : list->selectedItems()) {
        chosen << it->text();
    }
    // The list's own order, not the click order.
    std::sort(chosen.begin(), chosen.end(), [&all](const QString &a, const QString &b) { return all.indexOf(a) < all.indexOf(b); });
    addSessions(chosen);
}

bool GroupDialog::removeSelectedMembers()
{
    const auto items = m_members->selectedItems();
    if (items.isEmpty()) {
        return false;
    }
    for (QListWidgetItem *it : items) {
        delete m_members->takeItem(m_members->row(it));
    }
    updateEnabled();
    return true;
}

void GroupDialog::moveSelected(int delta)
{
    QList<int> rows;
    for (QListWidgetItem *it : m_members->selectedItems()) {
        rows << m_members->row(it);
    }
    if (rows.isEmpty() || (delta != -1 && delta != 1)) {
        return;
    }
    std::sort(rows.begin(), rows.end());
    if (delta > 0) {
        std::reverse(rows.begin(), rows.end());
    }
    // A block that already sits at the edge doesn't move.
    if ((delta < 0 && rows.front() == 0) || (delta > 0 && rows.front() == m_members->count() - 1)) {
        return;
    }
    for (int r : rows) {
        QListWidgetItem *it = m_members->takeItem(r);
        m_members->insertItem(r + delta, it);
        it->setSelected(true);
    }
    updateEnabled();
}

bool GroupDialog::saveCurrent()
{
    const GroupConfig g = config();
    if (const QString e = validateSessionName(g.name); !e.isEmpty()) {
        setError(e);
        return false;
    }
    const QListWidgetItem *cur = m_list->currentItem();
    const bool sameAsSelected = cur && cur->text() == g.name;
    if (!sameAsSelected && m_groups.contains(g.name)
        && QMessageBox::question(this, QStringLiteral("Save Group"), QStringLiteral("Replace the saved group \"%1\"?").arg(g.name))
            != QMessageBox::Yes) {
        return false;
    }
    QString err;
    if (!m_groups.save(g, &err)) {
        setError(err);
        return false;
    }
    setError({});
    refreshList(g.name);
    return true;
}

bool GroupDialog::deleteSelected()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item) {
        return false;
    }
    const QString name = item->text();
    if (!m_groups.remove(name)) {
        setError(QStringLiteral("Can't delete \"%1\".").arg(name));
        return false;
    }
    setError({});
    refreshList();
    newGroup();
    return true;
}

bool GroupDialog::openGroup()
{
    const GroupConfig g = config();
    if (g.members.isEmpty()) {
        setError(QStringLiteral("This group has no sessions yet. Add some first."));
        return false;
    }
    setError({});
    accept();
    return true;
}

} // namespace zterminal
