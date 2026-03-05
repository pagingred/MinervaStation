#ifndef JOBINFO_H
#define JOBINFO_H

#include <QString>

struct JobInfo
{
    int fileId = 0;
    QString url;
    QString destPath;
    qint64 size = 0;
};

#endif // JOBINFO_H
