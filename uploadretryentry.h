#ifndef UPLOADRETRYENTRY_H
#define UPLOADRETRYENTRY_H

#include <QTimer>

#include "jobinfo.h"

struct UploadRetryEntry
{
    JobInfo info;
    QString localPath;
    qint64 fileSize = 0;
    int retryCount = 0;
    QTimer *timer = nullptr;
};

#endif // UPLOADRETRYENTRY_H
