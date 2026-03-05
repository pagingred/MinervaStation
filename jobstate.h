#ifndef JOBSTATE_H
#define JOBSTATE_H

#include <QString>

#include "jobinfo.h"
#include "jobphase.h"

struct JobState
{
    JobInfo info;
    JobPhase phase = JobPhase::Queued;
    qint64 progress = 0;
    qint64 total = 0;
    int attempt = 1;
    QString error;
};

#endif // JOBSTATE_H
