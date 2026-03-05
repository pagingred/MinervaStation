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
        case JobPhase::Downloading:
            return 0;
        case JobPhase::Reporting:
            return 1;
        case JobPhase::UploadRetryWait:
            return 2;
        case JobPhase::QueuedUpload:
        case JobPhase::Queued:
            return 3;
        default:
            return 99;
        }
    }

    static int SortKey(JobPhase aPhase, qint64 aProgress = 0, qint64 aTotal = 0)
    {
        int permyriad = 0;
        if (aTotal > 0)
        {
            permyriad = static_cast<int>(aProgress * 10000 / aTotal);
        }
        return Priority(aPhase) * 100000 + (10000 - permyriad);
    }

    static QColor Color(JobPhase aPhase)
    {
        switch (aPhase)
        {
        case JobPhase::Queued:
            return QColor("#888888");
        case JobPhase::Downloading:
            return QColor("#2980b9");
        case JobPhase::QueuedUpload:
            return QColor("#d4a017");
        case JobPhase::Uploading:
            return QColor("#e67e22");
        case JobPhase::UploadRetryWait:
            return QColor("#f39c12");
        case JobPhase::Done:
            return QColor("#27ae60");
        case JobPhase::Failed:
            return QColor("#c0392b");
        case JobPhase::Reporting:
            return QColor("#e67e22");
        }
        return QColor("#888888");
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
        case JobPhase::QueuedUpload:
            color = "#d4a017";
            break;
        case JobPhase::Uploading:
            color = "#e67e22";
            break;
        case JobPhase::UploadRetryWait:
            color = "#f39c12";
            break;
        case JobPhase::Done:
            color = "#27ae60";
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
