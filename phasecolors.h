#ifndef PHASECOLORS_H
#define PHASECOLORS_H

#include "jobphase.h"
#include <QColor>
#include <QString>

class PhaseColors
{
public:
    static int Priority(JobPhase aPhase)
    {
        switch (aPhase)
        {
        case JobPhase::Uploading:
            return 0;
        case JobPhase::Downloading:
            return 1;
        case JobPhase::Queued:
            return 2;
        default:
            return 99;
        }
    }

    static int SortKey(JobPhase aPhase, qint64 = 0, qint64 = 0)
    {
        return Priority(aPhase);
    }

    static QColor Color(JobPhase aPhase)
    {
        switch (aPhase)
        {
        case JobPhase::Queued:
            return QColor(0x888888);
        case JobPhase::Downloading:
            return QColor(0x2980b9);
        case JobPhase::Uploading:
            return QColor(0xe67e22);
        case JobPhase::Done:
            return QColor(0x27ae60);
        case JobPhase::Failed:
            return QColor(0xc0392b);
        }
        return QColor(0x888888);
    }

    static QColor Background(JobPhase aPhase)
    {
        QColor c = Color(aPhase);
        c.setAlpha(25);
        return c;
    }

    static QString BarStyle(JobPhase aPhase)
    {
        QString color;
        switch (aPhase)
        {
        case JobPhase::Downloading:
            color = "#2980b9";
            break;
        case JobPhase::Uploading:
            color = "#e67e22";
            break;
        case JobPhase::Done:
            color = "#27ae60";
            break;
        case JobPhase::Failed:
            color = "#c0392b";
            break;
        default:
            color = "#3498db";
            break;
        }
        return QString(
                   "QProgressBar { text-align: center; border: none; background: #2a2a2a; height: 18px; }"
                   "QProgressBar::chunk { background-color: %1; border: none; margin: 0; }"
                   ).arg(color);
    }
};

#endif // PHASECOLORS_H
