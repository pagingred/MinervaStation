#ifndef JOBPHASE_H
#define JOBPHASE_H

enum class JobPhase
{
    Queued,
    Downloading,
    Uploading,
    Done,
    Failed
};

#endif // JOBPHASE_H
