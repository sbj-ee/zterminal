#pragma once

#include "Session.hpp"

#include <QDialog>
#include <QList>

class QPushButton;
class QTreeWidget;

namespace zterminal {

// Shown before Import Sessions writes anything: every session in the file
// with its target, extra ssh options, jump host and any other non-default
// setting, which ones replace an existing session, and which are refused
// (validateImportedSession). Nothing is saved unless the user presses
// "Import" (objectName "approveImport"); approved sessions are saved as
// approved, so they can launch. "use stored password" is always off.
class ImportReviewDialog : public QDialog
{
    Q_OBJECT
public:
    struct Entry {
        SessionConfig session;
        bool replaces = false; // a saved session with this name exists and will be overwritten
        bool skipped = false;  // exists and the user chose to keep the existing one
        QString refusal;       // non-empty: will not be imported, and why
    };
    ImportReviewDialog(const QList<Entry> &entries, QWidget *parent = nullptr);

    int importableCount() const { return m_importable; }
    QTreeWidget *tree() const { return m_tree; }

private:
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_approve = nullptr;
    int m_importable = 0;
};

} // namespace zterminal
