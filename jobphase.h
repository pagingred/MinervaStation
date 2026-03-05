#ifndef JOBPHASE_H
#define JOBPHASE_H

#include <QString>

enum class JobPhase
{
    Queued,
    Downloading,
    QueuedUpload,
    Uploading,
    UploadRetryWait,
    Reporting,
    Done,
    Failed
};

#endif // JOBPHASE_H
