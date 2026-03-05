#ifndef UPLOADQUEUEENTRY_H
#define UPLOADQUEUEENTRY_H

#include "jobinfo.h"

struct UploadQueueEntry
{
    JobInfo info;
    QString localPath;
    qint64 fileSize = 0;
    int uploadRetryCount = 0;
};

#endif // UPLOADQUEUEENTRY_H
