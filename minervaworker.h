#ifndef MINERVAWORKER_H
#define MINERVAWORKER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSet>
#include <QJsonArray>
#include <QTcpServer>
#include <QDateTime>

#include "filemanifest.h"
#include "minervaconfig.h"
#include "jobstate.h"
#include "uploadretryentry.h"
#include "uploadqueueentry.h"

class MinervaWorker : public QObject
{
    Q_OBJECT

public:
    explicit MinervaWorker(QObject *aParent = nullptr);
    ~MinervaWorker() override;

    bool IsRunning() const;
    void Start(const MinervaConfig &aCfg);
    void Stop();
    void UpdateConfig(const MinervaConfig &aCfg);

    void ForceRetryUpload(int aFileId);

    void DoLogin(const QString &aServerUrl);
    void CheckVersion(const QString &aServerUrl);
    void FetchLeaderboard(const QString &aServerUrl,
                          const QString &aToken,
                          int aPage = 1);

    static QString TokenPathLocal();
    static QString LoadToken();
    static void SaveToken(const QString &aToken);

    static QString PrettyPath(const QString &aRawPath);

    const MinervaConfig &Config() const;

signals:
    void Log(const QString &aMsg);
    void Started();
    void Stopped();
    void ConfigChanged();
    void JobUpdated(int aFileId,
                    const JobState &aState);
    void JobRemoved(int aFileId);
    void StatsChanged(int aOk,
                      int aFailed,
                      qint64 aBytesUp,
                      qint64 aBytesDown);
    void LoginResult(bool aOk,
                     const QString &aDetail);
    void VersionResult(const QString &aRemote);
    void LeaderboardResult(const QJsonArray &aRows,
                           int aPage);

private:
    void FetchJobs();
    void TryDispatchDownloads();
    void TryDispatchUploads();
    void ProcessJob(const JobInfo &aJob);
    void DoDownload(const JobInfo &aJob,
                    const QString &aLocalPath,
                    int aAttempt);
    void EnqueueUpload(const JobInfo &aJob,
                       const QString &aLocalPath,
                       qint64 aFileSize,
                       int aUploadRetryCount = 0);
    void DoUpload(const JobInfo &aJob,
                  const QString &aLocalPath,
                  qint64 aFileSize,
                  int aUploadRetryCount = 0);
    void DoReport(int aFileId,
                  const QString &aStatus,
                  qint64 aBytesDownloaded,
                  const QString &aError,
                  const QString &aLocalPath,
                  bool aKeepFile);
    void ReportWithRetry(int aFileId,
                         const QString &aStatus,
                         qint64 aBytes,
                         const QString &aError,
                         int aAttempt,
                         int aMaxAttempts,
                         std::function<void(bool)> aDone);

    void ScheduleUploadRetry(const JobInfo &aJob,
                             const QString &aLocalPath,
                             qint64 aFileSize,
                             int aRetryCount,
                             const QString &aError);
    void ExecuteUploadRetry(int aFileId);
    void ProbeFileNeeded(const FileEntry &aEntry);
    void MoveToDownloadsDir(const QString &aLocalPath);
    void RecoverFromManifest();
    void CleanOrphanFiles();

    QNetworkRequest AuthRequest(const QUrl &aUrl) const;
    QString LocalPathForJob(const QString &aUrl,
                            const QString &aDestPath) const;
    static QString SanitizeComponent(const QString &aPart);
    static QString LabelForPath(const QString &aPath);

    MinervaConfig mCfg;
    QNetworkAccessManager *mNam = nullptr;
    QTimer *mFetchTimer = nullptr;
    bool mRunning = false;
    QString mRemoteVersion = "1.0.0";
    QQueue<JobInfo> mQueue;
    QSet<int> mSeenIds;
    bool mNoJobsWarned = false;
    int mDownloadSlots = 0;
    QQueue<UploadQueueEntry> mUploadQueue;
    int mUploadSlots = 0;
    int mOkCount = 0;
    int mFailCount = 0;
    qint64 mBytesUp = 0;
    qint64 mBytesDown = 0;
    QVector<QNetworkAccessManager*> mUploadNamPool;
    int mUploadNamIndex = 0;
    void EnsureUploadNamPool();
    QMap<int, UploadRetryEntry> mUploadRetryQueue;
    FileManifest mManifest;
    QTcpServer *mLoginServer = nullptr;
};

#endif
