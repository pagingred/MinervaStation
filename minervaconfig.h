#ifndef MINERVACONFIG_H
#define MINERVACONFIG_H

#include <QString>

struct MinervaConfig
{
    QString firehoseUrl = "wss://firehose.minerva-archive.org/worker";
    QString serverUrl = "https://api.minerva-archive.org";
    QString token;
    int concurrency = 5;
    int subchunkRetries = 5;
    int reconnectDelaySec = 5;
};

#endif // MINERVACONFIG_H
