#include "MouseSettings.hpp"

#include <QSettings>

namespace zterminal {

QString MouseSettings::toString(MiddleClickAction a)
{
    switch (a) {
    case MiddleClickAction::PasteClipboard:
        return QStringLiteral("paste-clipboard");
    case MiddleClickAction::Off:
        return QStringLiteral("off");
    case MiddleClickAction::PastePrimary:
        break;
    }
    return QStringLiteral("paste-primary");
}

MiddleClickAction MouseSettings::middleClickFromString(const QString &s)
{
    if (s == QLatin1String("paste-clipboard")) {
        return MiddleClickAction::PasteClipboard;
    }
    if (s == QLatin1String("off")) {
        return MiddleClickAction::Off;
    }
    return MiddleClickAction::PastePrimary;
}

void MouseSettings::load(const QSettings &s)
{
    const MouseSettings d;
    middleClick = middleClickFromString(
        s.value(QStringLiteral("mouse/middleClick"), toString(d.middleClick)).toString());
    copyOnSelectToClipboard =
        s.value(QStringLiteral("mouse/copyOnSelectToClipboard"), d.copyOnSelectToClipboard).toBool();
    wordDelimiters = s.value(QStringLiteral("mouse/wordDelimiters"), d.wordDelimiters).toString();
}

void MouseSettings::save(QSettings &s) const
{
    s.setValue(QStringLiteral("mouse/middleClick"), toString(middleClick));
    s.setValue(QStringLiteral("mouse/copyOnSelectToClipboard"), copyOnSelectToClipboard);
    s.setValue(QStringLiteral("mouse/wordDelimiters"), wordDelimiters);
}

} // namespace zterminal
