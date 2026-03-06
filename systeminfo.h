#ifndef SYSTEMINFO_H
#define SYSTEMINFO_H

#include <QString>

struct SystemInfo
{
    int cpuCores = 0;
    QString cpuArch;
    QString osName;
    qint64 totalRamBytes = 0;
    qint64 availRamBytes = 0;
};

#endif // SYSTEMINFO_H
