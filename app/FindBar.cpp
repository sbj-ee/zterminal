#include "FindBar.hpp"

#include "Terminal.hpp"
#include "TerminalView.hpp"

#include <algorithm>
#include <optional>

#include <QCheckBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QToolButton>

namespace zterminal {

FindBar::FindBar(Terminal *term, TerminalView *view, QWidget *parent)
    : QWidget(parent)
    , m_term(term)
    , m_view(view)
{
    setObjectName(QStringLiteral("findBar"));
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(6, 3, 6, 3);
    row->setSpacing(6);

    auto *label = new QLabel(QStringLiteral("Find:"));
    m_text = new QLineEdit;
    m_text->setObjectName(QStringLiteral("findText"));
    m_text->setPlaceholderText(QStringLiteral("Search scrollback"));
    m_text->setClearButtonEnabled(true);
    m_text->setMinimumWidth(180);
    label->setBuddy(m_text);
    m_case = new QCheckBox(QStringLiteral("Match case"));
    m_case->setObjectName(QStringLiteral("findCase"));
    m_regex = new QCheckBox(QStringLiteral("Regex"));
    m_regex->setObjectName(QStringLiteral("findRegex"));
    m_wrap = new QCheckBox(QStringLiteral("Wrap"));
    m_wrap->setObjectName(QStringLiteral("findWrap"));
    m_wrap->setChecked(true);
    m_count = new QLabel;
    m_count->setObjectName(QStringLiteral("findCount"));
    m_count->setMinimumWidth(110);
    auto mkButton = [](const char *name, const QString &glyph, const QString &tip) {
        auto *b = new QToolButton;
        b->setObjectName(QString::fromLatin1(name));
        b->setText(glyph);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::TabFocus);
        return b;
    };
    m_prev = mkButton("findPrevious", QStringLiteral("\u25BC"), QStringLiteral("Previous (newer) match \u2014 Shift+Enter"));
    m_next = mkButton("findNext", QStringLiteral("\u25B2"), QStringLiteral("Next (older) match \u2014 Enter"));
    m_close = new QToolButton;
    m_close->setObjectName(QStringLiteral("findClose"));
    m_close->setText(QStringLiteral("\u2715"));
    m_close->setToolTip(QStringLiteral("Close \u2014 Esc"));
    m_close->setAutoRaise(true);
    m_close->setFocusPolicy(Qt::TabFocus);

    row->addWidget(label);
    row->addWidget(m_text, 1);
    row->addWidget(m_case);
    row->addWidget(m_regex);
    row->addWidget(m_wrap);
    row->addWidget(m_count);
    row->addWidget(m_next);
    row->addWidget(m_prev);
    row->addWidget(m_close);

    m_typing = new QTimer(this);
    m_typing->setSingleShot(true);
    m_typing->setInterval(150);
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(400);

    connect(m_text, &QLineEdit::textChanged, this, [this]() { m_typing->start(); });
    connect(m_typing, &QTimer::timeout, this, [this]() { runSearch(false); });
    connect(m_case, &QCheckBox::toggled, this, [this]() { runSearch(false); });
    connect(m_regex, &QCheckBox::toggled, this, [this]() { runSearch(false); });
    connect(m_wrap, &QCheckBox::toggled, this, [this]() { updateCount(); });
    connect(m_next, &QToolButton::clicked, this, &FindBar::findNext);
    connect(m_prev, &QToolButton::clicked, this, &FindBar::findPrevious);
    connect(m_close, &QToolButton::clicked, this, &FindBar::close);
    connect(m_idle, &QTimer::timeout, this, [this]() {
        if (isVisible() && m_stale) {
            runSearch(true);
        }
    });
    connect(m_term, &Terminal::damaged, this, &FindBar::onOutput);
    connect(m_term, &Terminal::scrolledIntoHistory, this, [this](int, int dropped) {
        if (dropped > 0) {
            onLinesDropped(dropped);
        }
    });
    connect(m_term, &Terminal::scrollbackCleared, this, [this]() {
        if (isVisible()) {
            runSearch(false);
        }
    });

    for (QWidget *w : std::initializer_list<QWidget *>{m_text, m_case, m_regex, m_wrap, m_prev, m_next, m_close}) {
        w->installEventFilter(this);
    }
    updateCount();
    hide();
}

QString FindBar::text() const { return m_text->text(); }

void FindBar::setText(const QString &t)
{
    m_text->setText(t);
}

FindOptions FindBar::options() const
{
    FindOptions o;
    o.caseSensitive = m_case->isChecked();
    o.regex = m_regex->isChecked();
    return o;
}

bool FindBar::wrap() const { return m_wrap->isChecked(); }

void FindBar::open()
{
    const bool wasHidden = isHidden();
    show();
    m_text->setFocus(Qt::ShortcutFocusReason);
    m_text->selectAll();
    if (wasHidden && !m_text->text().isEmpty()) {
        runSearch(false);
    }
}

void FindBar::close()
{
    m_typing->stop();
    m_idle->stop();
    hide();
    m_anchor.reset();
    m_stale = false;
    m_matches.clear();
    m_heads.clear();
    m_current = -1;
    m_view->clearFindMatches();
    emit closed();
}

void FindBar::search()
{
    m_typing->stop();
    runSearch(false);
}

void FindBar::runSearch(bool keepCurrent)
{
    std::optional<FindMatch> keep;
    if (keepCurrent && m_anchor) {
        keep = m_anchor;
    } else if (keepCurrent && m_current >= 0) {
        keep = m_matches[std::size_t(currentIndex())];
    }
    m_anchor.reset();
    QElapsedTimer t;
    t.start();
    m_matches = TerminalSearch::findAll(*m_term, m_text->text(), options(), &m_error);
    m_lastSearchMs = t.elapsed();
    m_stale = false;
    rebuildHeads();
    const int n = int(m_heads.size());
    int idx = n == 0 ? -1 : n - 1; // newest
    if (keep && n > 0) {
        auto less = [](const FindMatch &a, const FindMatch &b) { return a.line != b.line ? a.line < b.line : a.col < b.col; };
        auto it = std::lower_bound(m_heads.begin(), m_heads.end(), *keep,
                                   [this, &less](int i, const FindMatch &k) { return less(m_matches[std::size_t(i)], k); });
        if (it != m_heads.end() && m_matches[std::size_t(*it)].line == keep->line
            && m_matches[std::size_t(*it)].col == keep->col) {
            idx = int(it - m_heads.begin());
        } else if (it != m_heads.begin()) {
            idx = int(it - m_heads.begin()) - 1; // nearest older
        } else {
            idx = 0; // everything older is gone: the oldest left
        }
    }
    m_view->setFindMatches(m_matches, -1);
    setCurrent(idx, !keepCurrent);
}

void FindBar::rebuildHeads()
{
    m_heads.clear();
    for (std::size_t i = 0; i < m_matches.size(); ++i) {
        if (!m_matches[i].continued) {
            m_heads.push_back(int(i));
        }
    }
}

void FindBar::setCurrent(int idx, bool scroll)
{
    m_current = idx;
    m_view->setCurrentFindMatch(currentIndex());
    if (scroll && idx >= 0) {
        m_view->scrollToLine(m_matches[std::size_t(currentIndex())].line);
    }
    updateCount();
}

void FindBar::findNext()
{
    if (m_stale || m_typing->isActive()) {
        m_typing->stop();
        runSearch(true);
    }
    if (m_heads.empty()) {
        return;
    }
    if (m_current > 0) {
        setCurrent(m_current - 1, true);
    } else if (wrap()) {
        setCurrent(int(m_heads.size()) - 1, true);
        updateCount(QStringLiteral("wrapped"));
    } else {
        setCurrent(m_current < 0 ? 0 : m_current, true);
        updateCount(QStringLiteral("top reached"));
    }
}

void FindBar::findPrevious()
{
    if (m_stale || m_typing->isActive()) {
        m_typing->stop();
        runSearch(true);
    }
    if (m_heads.empty()) {
        return;
    }
    const int last = int(m_heads.size()) - 1;
    if (m_current >= 0 && m_current < last) {
        setCurrent(m_current + 1, true);
    } else if (wrap()) {
        setCurrent(0, true);
        updateCount(QStringLiteral("wrapped"));
    } else {
        setCurrent(last, true);
        updateCount(QStringLiteral("bottom reached"));
    }
}

QString FindBar::countText() const { return m_count->text(); }

void FindBar::updateCount(const QString &note)
{
    QString s;
    if (!m_error.isEmpty()) {
        s = QStringLiteral("Invalid regex");
        m_count->setToolTip(m_error);
        m_text->setStyleSheet(QStringLiteral("QLineEdit { background: #f8d0d0; }"));
    } else {
        m_count->setToolTip({});
        if (m_text->text().isEmpty()) {
            s.clear();
            m_text->setStyleSheet({});
        } else if (m_heads.empty()) {
            s = QStringLiteral("No matches");
            m_text->setStyleSheet(QStringLiteral("QLineEdit { background: #f8d0d0; }"));
        } else {
            s = QStringLiteral("%1 of %2").arg(currentNumber()).arg(m_heads.size());
            m_text->setStyleSheet({});
        }
    }
    if (!note.isEmpty() && !s.isEmpty()) {
        s += QStringLiteral(" (%1)").arg(note);
    }
    m_count->setText(s);
    m_prev->setEnabled(!m_heads.empty());
    m_next->setEnabled(!m_heads.empty());
}

void FindBar::onOutput()
{
    if (isVisible() && !m_text->text().isEmpty()) {
        m_stale = true;
        m_idle->start();
    }
}

void FindBar::onLinesDropped(int dropped)
{
    if (m_matches.empty()) {
        return;
    }
    const int oldCurrentSeg = currentIndex();
    auto firstKept = std::find_if(m_matches.begin(), m_matches.end(), [dropped](const FindMatch &m) {
        return m.line >= dropped;
    });
    const int removed = int(firstKept - m_matches.begin());
    m_matches.erase(m_matches.begin(), firstKept);
    for (FindMatch &m : m_matches) {
        m.line -= dropped;
    }
    if (!m_matches.empty()) {
        m_matches.front().continued = false; // its first part was dropped
    }
    rebuildHeads();
    if (oldCurrentSeg >= 0) {
        const int seg = oldCurrentSeg - removed;
        if (seg < 0) {
            // The current match scrolled out of the scrollback: continue from the oldest.
            m_current = m_heads.empty() ? -1 : 0;
            m_anchor = FindMatch{0, 0, 0, false};
        } else {
            m_current = int(std::lower_bound(m_heads.begin(), m_heads.end(), seg) - m_heads.begin());
        }
    }
    m_view->setFindMatches(m_matches, currentIndex());
    updateCount();
}

bool FindBar::eventFilter(QObject *obj, QEvent *e)
{
    if (e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Escape) {
            close();
            return true;
        }
        if (obj == m_text && (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)) {
            if (ke->modifiers() & Qt::ShiftModifier) {
                findPrevious();
            } else {
                findNext();
            }
            return true;
        }
    } else if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Escape) {
            ke->accept(); // Esc belongs to the bar even if something binds it
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

} // namespace zterminal
