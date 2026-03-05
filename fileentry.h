#ifndef FILEENTRY_H
#define FILEENTRY_H

#include <QString>
#include <QDateTime>

#include "filestate.h"

struct FileEntry
{
    int fileId = 0;
    QString url;
    QString destPath;
    QString localPath;
    qint64 size = 0;
    FileState state = FileState::Downloading;
    int uploadRetryCount = 0;
    QDateTime timestamp;
};

#endif // FILEENTRY_H
