#include "LogTextFilter.hpp"

#include <algorithm>

namespace zterminal {

namespace {
constexpr int kMaxLine = 64 * 1024; // a runaway line can't eat memory
}

LogTextFilter::LogTextFilter(LineSink sink)
    : m_sink(std::move(sink))
{
}

QStringList LogTextFilter::filterAll(const QByteArray &bytes, int columns)
{
    QStringList out;
    LogTextFilter f([&out](const QString &l) { out << l; });
    f.setColumns(columns);
    f.feed(bytes);
    f.flush();
    return out;
}

void LogTextFilter::put(QChar c)
{
    if (m_alt) {
        return;
    }
    if (m_col >= kMaxLine) {
        endLine();
    }
    if (m_columns > 0 && m_col - m_rowStart >= m_columns) {
        m_rowStart += m_columns; // autowrap onto the next row of the same logical line
        m_col = std::max(m_col, m_rowStart);
    }
    if (m_col < m_line.size()) {
        m_line[m_col] = c;
    } else {
        if (m_col > m_line.size()) {
            m_line += QString(m_col - m_line.size(), QLatin1Char(' '));
        }
        m_line += c;
    }
    ++m_col;
}

void LogTextFilter::endLine()
{
    if (!m_alt) {
        qsizetype end = m_line.size();
        while (end > 0 && m_line.at(end - 1) == QLatin1Char(' ')) {
            --end;
        }
        m_sink(m_line.left(end));
    }
    m_line.clear();
    m_col = 0;
    m_rowStart = 0;
}

void LogTextFilter::flush()
{
    if (!m_line.trimmed().isEmpty()) {
        endLine();
    }
    m_line.clear();
    m_col = 0;
    m_rowStart = 0;
}

int LogTextFilter::param(int index, int dflt) const
{
    const QStringList parts = m_params.split(QLatin1Char(';'));
    if (index >= parts.size()) {
        return dflt;
    }
    bool ok = false;
    const int v = parts.at(index).toInt(&ok);
    return ok && v > 0 ? std::min(v, kMaxLine) : dflt;
}

void LogTextFilter::csi(QChar final)
{
    const bool priv = m_params.startsWith(QLatin1Char('?'));
    if (priv) {
        // Alternate screen: ?47, ?1047, ?1049 (h = enter, l = leave).
        if (final == QLatin1Char('h') || final == QLatin1Char('l')) {
            const QStringList modes = m_params.mid(1).split(QLatin1Char(';'));
            for (const QString &m : modes) {
                if (m == QLatin1String("47") || m == QLatin1String("1047") || m == QLatin1String("1049")) {
                    const bool enter = final == QLatin1Char('h');
                    if (enter && !m_alt) {
                        flush();
                    }
                    m_alt = enter;
                    m_line.clear();
                    m_col = 0;
                }
            }
        }
        return;
    }
    if (m_params.contains(QLatin1Char('>')) || m_params.contains(QLatin1Char('=')) || m_params.contains(QLatin1Char(' '))) {
        return;
    }
    switch (final.unicode()) {
    case 'K': {
        const int mode = m_params.isEmpty() ? 0 : m_params.toInt();
        if (mode == 0) {
            m_line.truncate(m_col);
        } else if (mode == 1) {
            for (int i = 0; i <= m_col && i < m_line.size(); ++i) {
                m_line[i] = QLatin1Char(' ');
            }
        } else if (mode == 2) {
            m_line = QString(std::min<qsizetype>(m_col, m_line.size()), QLatin1Char(' '));
        }
        break;
    }
    case 'C':
        m_col = std::min(m_col + param(0, 1), kMaxLine);
        break;
    case 'D':
        m_col = std::max(m_rowStart, m_col - param(0, 1));
        break;
    case 'G':
        m_col = m_rowStart + param(0, 1) - 1;
        break;
    case 'A': // up within a wrapped logical line (readline editing long input)
        if (m_columns > 0) {
            const int rows = std::min(param(0, 1), m_rowStart / m_columns);
            m_rowStart -= rows * m_columns;
            m_col -= rows * m_columns;
        }
        break;
    case 'B':
        if (m_columns > 0) {
            m_rowStart += param(0, 1) * m_columns;
            m_col += param(0, 1) * m_columns;
        }
        break;
    case 'P':
        if (m_col < m_line.size()) {
            m_line.remove(m_col, param(0, 1));
        }
        break;
    case '@':
        if (m_col < m_line.size()) {
            m_line.insert(m_col, QString(param(0, 1), QLatin1Char(' ')));
        }
        break;
    case 'H':
    case 'f':
    case 'd':
        // Moved to another row (full-screen style output): finish this line.
        flush();
        if (final != QLatin1Char('d')) {
            m_col = param(1, 1) - 1;
        }
        break;
    default:
        break; // SGR (m) and everything else: no text
    }
}

void LogTextFilter::feed(const QByteArray &bytes)
{
    const QString text = m_decoder.decode(bytes);
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        switch (m_state) {
        case State::Ground:
            if (u == 0x1B) {
                m_state = State::Esc;
            } else if (u == u'\n' || u == 0x0B || u == 0x0C) {
                endLine();
            } else if (u == u'\r') {
                m_col = m_rowStart;
            } else if (u == u'\b') {
                m_col = std::max(m_rowStart, m_col - 1);
            } else if (u == u'\t') {
                m_col = std::min((m_col / 8 + 1) * 8, kMaxLine);
            } else if (u == 0x9B) { // C1 CSI
                m_params.clear();
                m_state = State::Csi;
            } else if (u == 0x90 || u == 0x9D || u == 0x98 || u == 0x9E || u == 0x9F) { // C1 DCS/OSC/SOS/PM/APC
                m_state = State::String;
            } else if (u < 0x20 || u == 0x7F || (u >= 0x80 && u < 0xA0)) {
                // other controls (BEL, SO/SI, ...): nothing to log
            } else {
                put(c);
            }
            break;
        case State::Esc:
            if (u == u'[') {
                m_params.clear();
                m_state = State::Csi;
            } else if (u == u']' || u == u'P' || u == u'X' || u == u'^' || u == u'_') {
                m_state = State::String;
            } else if (u >= 0x20 && u <= 0x2F) { // ESC ( B, ESC # 8, ...
                m_state = State::EscIntermediate;
            } else {
                m_state = State::Ground; // ESC 7, ESC 8, ESC =, ESC M, ...
            }
            break;
        case State::EscIntermediate:
            if (!(u >= 0x20 && u <= 0x2F)) {
                m_state = State::Ground;
            }
            break;
        case State::Csi:
            if (u >= 0x40 && u <= 0x7E) {
                csi(c);
                m_state = State::Ground;
            } else if (u == 0x1B) {
                m_state = State::Esc; // aborted sequence
            } else if (u < 0x20) {
                // C0 inside CSI is executed; keep it simple and ignore it
            } else if (m_params.size() < 64) {
                m_params += c;
            }
            break;
        case State::String: // OSC / DCS / SOS / PM / APC: until BEL or ST
            if (u == 0x07 || u == 0x9C) {
                m_state = State::Ground;
            } else if (u == 0x1B) {
                m_state = State::StringEsc;
            }
            break;
        case State::StringEsc:
            m_state = u == u'\\' ? State::Ground : State::String;
            break;
        }
    }
}

} // namespace zterminal
