#ifndef JOBINFO_H
#define JOBINFO_H

#include <QString>

struct JobInfo
{
    QString chunkId;
    QString fileId;
    QString url;
    quint64 rangeStart = 0;
    quint64 rangeEnd = 0;
};

#endif // JOBINFO_H
