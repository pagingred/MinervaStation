#ifndef MINERVACONFIG_H
#define MINERVACONFIG_H

#include <QString>

struct MinervaConfig
{
    QString serverUrl = "https://api.minerva-archive.org";
    QString uploadUrl = "https://gate.minerva-archive.org";
    QString token;
    QString tempDir;
    QString downloadsDir;
    int dlConcurrency = 5;
    int ulConcurrency = 5;
    int batchSize = 10;
    int aria2cConnections = 8;
    bool keepFiles = false;
    int downloadRetryDelaySec = 5;
    int uploadRetryDelaySec = 10;
    qint64 diskReserveBytes = 50LL * 1024 * 1024 * 1024;
    int reportRetries = 20;
    int queuePrefetch = 2;
    qint64 uploadChunkSize = 8LL * 1024 * 1024;
    int uploadStartRetries = 12;
    int uploadChunkRetries = 30;
    int uploadFinishRetries = 12;
    float uploadRetryCap = 25.0f;
    float uploadChunkRetryCap = 20.0f;
};

#endif // MINERVACONFIG_H
