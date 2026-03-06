#ifndef MINERVAWORKER_H
#define MINERVAWORKER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QWebSocket>

#include "minervaconfig.h"
#include "jobstate.h"
#include "hyperscrapeprotocol.h"

class QTimer;

class MinervaWorker : public QObject
{
    Q_OBJECT

public:
    explicit MinervaWorker(QObject *aParent = nullptr);
    ~MinervaWorker() override;

    bool IsRunning() const;
    bool IsConnected() const;
    void EstablishConnection(const MinervaConfig &aCfg);
    void CloseConnection();
    void Start(const MinervaConfig &aCfg);
    void Stop();
    void UpdateConfig(const MinervaConfig &aCfg);

    void DoLogin(const QString &aServerUrl);
    void SubmitToken(const QString &aToken);
    void CheckVersion(const QString &aServerUrl);
    void FetchLeaderboard(const QString &aServerUrl,
                          const QString &aToken,
                          int aPage = 1);
    void FetchStats(const QString &aApiBase);

    static QString TokenPathLocal();
    static QString LoadToken();
    static void SaveToken(const QString &aToken);

    const MinervaConfig &Config() const;

signals:
    void Log(const QString &aMsg);
    void Started();
    void Stopped();
    void ConfigChanged();
    void JobUpdated(int aJobIndex,
                    const JobState &aState);
    void JobRemoved(int aJobIndex);
    void StatsChanged(int aOk,
                      int aFailed,
                      qint64 aBytesUp,
                      qint64 aBytesDown);
    void LoginResult(bool aOk,
                     const QString &aDetail);
    void LoginPrompt();
    void VersionResult(const QString &aRemote);
    void LeaderboardResult(const QJsonArray &aRows,
                           int aPage);
    void NetworkStatsResult(const QJsonObject &aStats);

private:
    void ConnectWebSocket();
    void OnConnected();
    void OnDisconnected();
    void OnBinaryMessage(const QByteArray &aMsg);
    void OnError(QAbstractSocket::SocketError aError);
    void HandleRegisterResponse(const QByteArray &aMsg);
    void HandleChunkResponse(const QByteArray &aMsg);
    void HandleOk(const QByteArray &aMsg);
    void HandleError(const QByteArray &aMsg);
    void ScheduleRequestChunks();
    void RequestChunks();
    void StreamChunk(const HyperscrapeProtocol::ChunkAssignment &aChunk, int aJobIndex);
    void RetryChunk(int aJobIndex);
    void FailChunk(int aJobIndex, const QString &aChunkId, const QString &aError, bool aDetach);
    void SendSubchunk(int aJobIndex, const QString &aChunkId,
                      const QString &aFileId, const QByteArray &aData);

    MinervaConfig mCfg;
    QWebSocket *mSocket = nullptr;
    QNetworkAccessManager *mNam = nullptr;
    QTimer *mReconnectTimer = nullptr;
    QTimer *mRequestChunksTimer = nullptr;
    bool mRunning = false;
    bool mRegistered = false;
    int mActiveStreams = 0;
    int mNextJobIndex = 0;
    QMap<int, JobState> mJobs;
    QMap<QString, int> mChunkToJob;
    QMap<int, HyperscrapeProtocol::ChunkAssignment> mChunkAssignments;
    QMap<int, QByteArray> mChunkBuffers;
    QMap<int, qint64> mChunkBytesReceived;
    QMap<int, bool> mChunkWaitingOk;
    QMap<int, QByteArray> mChunkPendingSubchunk;
    QMap<int, bool> mChunkHttpDone;
    QMap<int, QNetworkReply*> mChunkReplies;
    QSet<QString> mSeenChunkIds;
    int mOkCount = 0;
    int mFailCount = 0;
    qint64 mBytesUp = 0;
    qint64 mBytesDown = 0;
};

#endif
