#ifndef SYSTEMPROFILE_H
#define SYSTEMPROFILE_H

#include <QObject>
#include <QString>

#include "systeminfo.h"
#include "speedtestresult.h"
#include "recommendedsettings.h"

class QNetworkReply;
class QNetworkAccessManager;

class SystemProfiler : public QObject
{
    Q_OBJECT

public:
    explicit SystemProfiler(QObject *aParent = nullptr);

    SystemInfo DetectSystemInfo(const QString &aPath = ".");

    void RunSpeedTest();
    void AbortSpeedTest();

    static RecommendedSettings Calculate(const SystemInfo &aSys, const SpeedTestResult &aSpeed);

signals:
    void SpeedTestProgress(const QString &aPhase, int aPercent);
    void SpeedTestFinished(const SpeedTestResult &aResult);

private:
    void StartDownloadTest();
    void StartUploadTest();

    QNetworkAccessManager *mNam = nullptr;
    QNetworkReply *mActiveReply = nullptr;
    SpeedTestResult mResult;
    bool mAborted = false;
};

#endif // SYSTEMPROFILE_H
