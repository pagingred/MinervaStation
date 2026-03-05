#ifndef SPEEDTESTRESULT_H
#define SPEEDTESTRESULT_H

#include <QString>

struct SpeedTestResult
{
    double downloadMbps = 0.0;
    double uploadMbps = 0.0;
    int errors = 0;
    bool success = false;
    QString errorMessage;
};

#endif // SPEEDTESTRESULT_H
