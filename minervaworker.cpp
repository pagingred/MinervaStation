#include "minervaworker.h"
#include "hyperscrapeprotocol.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QDesktopServices>
#include <QTimer>

QString MinervaWorker::TokenPathLocal()
{
    return QCoreApplication::applicationDirPath() + "/token";
}

QString MinervaWorker::LoadToken()
{
    QFile f(TokenPathLocal());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QString t = QString::fromUtf8(f.readAll()).trimmed();
        if (!t.isEmpty())
        {
            return t;
        }
    }
    return {};
}

void MinervaWorker::SaveToken(const QString &aToken)
{
    QFile local(TokenPathLocal());
    if (local.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        local.write(aToken.toUtf8());
    }
}

const MinervaConfig &MinervaWorker::Config() const
{
    return mCfg;
}

static constexpr int kRetryDelayMs = 5000;

MinervaWorker::MinervaWorker(QObject *aParent)
    : QObject(aParent)
{
    mRequestChunksTimer = new QTimer(this);
    mRequestChunksTimer->setSingleShot(true);
    mRequestChunksTimer->setInterval(500);
    connect(mRequestChunksTimer, &QTimer::timeout, this, &MinervaWorker::RequestChunks);
}

MinervaWorker::~MinervaWorker()
{
    mRunning = false;

    for (auto it = mChunkReplies.begin(); it != mChunkReplies.end(); ++it)
    {
        if (it.value())
        {
            it.value()->abort();
        }
    }

    if (mRequestChunksTimer)
    {
        mRequestChunksTimer->stop();
    }

    if (mReconnectTimer)
    {
        mReconnectTimer->stop();
    }

    if (mSocket)
    {
        disconnect(mSocket, nullptr, this, nullptr);
        mSocket->close();
    }
}

bool MinervaWorker::IsRunning() const
{
    return mRunning;
}

bool MinervaWorker::IsConnected() const
{
    return mSocket && mSocket->state() == QAbstractSocket::ConnectedState;
}

void MinervaWorker::EstablishConnection(const MinervaConfig &aCfg)
{
    if (mReconnectTimer)
    {
        mReconnectTimer->stop();
        mReconnectTimer->deleteLater();
        mReconnectTimer = nullptr;
    }
    if (mSocket)
    {
        disconnect(mSocket, nullptr, this, nullptr);
        mSocket->close();
        mSocket->deleteLater();
        mSocket = nullptr;
    }
    mRegistered = false;

    mCfg = aCfg;

    if (!mNam)
    {
        mNam = new QNetworkAccessManager(this);
    }

    mSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(mSocket, &QWebSocket::connected, this, &MinervaWorker::OnConnected);
    connect(mSocket, &QWebSocket::disconnected, this, &MinervaWorker::OnDisconnected);
    connect(mSocket, &QWebSocket::binaryMessageReceived, this, &MinervaWorker::OnBinaryMessage);
    connect(mSocket, &QWebSocket::errorOccurred, this, &MinervaWorker::OnError);

    mReconnectTimer = new QTimer(this);
    mReconnectTimer->setSingleShot(true);
    connect(mReconnectTimer, &QTimer::timeout, this, &MinervaWorker::ConnectWebSocket);

    ConnectWebSocket();
}

void MinervaWorker::CloseConnection()
{
    if (mRunning)
    {
        Stop();
    }

    mRegistered = false;

    if (mReconnectTimer)
    {
        mReconnectTimer->stop();
        mReconnectTimer->deleteLater();
        mReconnectTimer = nullptr;
    }

    if (mSocket)
    {
        disconnect(mSocket, nullptr, this, nullptr);
        mSocket->close();
        mSocket->deleteLater();
        mSocket = nullptr;
    }

    emit Log("Disconnected from firehose server.");
}

void MinervaWorker::Start(const MinervaConfig &aCfg)
{
    if (mRunning)
    {
        return;
    }

    bool needReconnect = (aCfg.firehoseUrl != mCfg.firehoseUrl
                          || aCfg.token != mCfg.token);
    mCfg = aCfg;
    mRunning = true;
    mOkCount = 0;
    mFailCount = 0;
    mBytesUp = 0;
    mBytesDown = 0;
    mActiveStreams = 0;
    mNextJobIndex = 0;
    mJobs.clear();
    mChunkToJob.clear();
    mChunkAssignments.clear();
    mChunkBuffers.clear();
    mChunkBytesReceived.clear();
    mChunkWaitingOk.clear();
    mChunkPendingSubchunk.clear();
    mChunkHttpDone.clear();
    mChunkReplies.clear();
    mSeenChunkIds.clear();

    emit Log(QString("Worker started (concurrency=%1).").arg(mCfg.concurrency));
    emit Started();

    if (needReconnect || !mSocket)
    {
        EstablishConnection(mCfg);
    }
    else if (mRegistered)
    {
        RequestChunks();
    }
}

void MinervaWorker::ConnectWebSocket()
{
    if (!mSocket)
    {
        return;
    }
    emit Log(QString("Connecting to %1...").arg(mCfg.firehoseUrl));
    mSocket->open(QUrl(mCfg.firehoseUrl));
}

void MinervaWorker::OnConnected()
{
    emit Log("WebSocket connected. Sending Register...");
    QByteArray msg = HyperscrapeProtocol::BuildRegister(mCfg.token, mCfg.concurrency);
    mSocket->sendBinaryMessage(msg);
}

void MinervaWorker::OnDisconnected()
{
    mRegistered = false;

    for (auto it = mChunkReplies.begin(); it != mChunkReplies.end(); ++it)
    {
        if (it.value())
        {
            it.value()->abort();
        }
    }
    mChunkReplies.clear();
    mChunkAssignments.clear();
    mChunkBuffers.clear();
    mChunkBytesReceived.clear();
    mChunkWaitingOk.clear();
    mChunkPendingSubchunk.clear();
    mChunkHttpDone.clear();

    for (auto it = mChunkToJob.begin(); it != mChunkToJob.end(); ++it)
    {
        int jobIdx = it.value();
        emit JobRemoved(jobIdx);
    }
    mChunkToJob.clear();
    mJobs.clear();
    mSeenChunkIds.clear();
    mActiveStreams = 0;

    if (mRequestChunksTimer)
    {
        mRequestChunksTimer->stop();
    }

    if (mReconnectTimer)
    {
        emit Log(QString("WebSocket disconnected (code=%1 reason='%2'). Reconnecting in %3s...")
                 .arg(mSocket ? mSocket->closeCode() : -1)
                 .arg(mSocket ? mSocket->closeReason() : QString())
                 .arg(mCfg.reconnectDelaySec));
        mReconnectTimer->start(mCfg.reconnectDelaySec * 1000);
    }
}

void MinervaWorker::OnError(QAbstractSocket::SocketError aError)
{
    Q_UNUSED(aError)
    if (mSocket)
    {
        emit Log(QString("WebSocket error: %1").arg(mSocket->errorString()));
    }
}

void MinervaWorker::OnBinaryMessage(const QByteArray &aMsg)
{
    if (aMsg.isEmpty())
    {
        return;
    }

    quint8 type = static_cast<quint8>(aMsg[0]);
    switch (type)
    {
    case static_cast<quint8>(HyperscrapeProtocol::MsgType::RegisterResponse):
        HandleRegisterResponse(aMsg);
        break;
    case static_cast<quint8>(HyperscrapeProtocol::MsgType::ChunkResponse):
        HandleChunkResponse(aMsg);
        break;
    case static_cast<quint8>(HyperscrapeProtocol::MsgType::Ok):
        HandleOk(aMsg);
        break;
    case static_cast<quint8>(HyperscrapeProtocol::MsgType::Error):
        HandleError(aMsg);
        break;
    default:
        emit Log(QString("Unknown message type: 0x%1").arg(type, 2, 16, QChar('0')));
        break;
    }
}

void MinervaWorker::HandleRegisterResponse(const QByteArray &aMsg)
{
    Q_UNUSED(aMsg)
    mRegistered = true;
    emit Log("Registered with server.");
    if (mRunning)
    {
        RequestChunks();
    }
}

void MinervaWorker::HandleChunkResponse(const QByteArray &aMsg)
{
    auto chunks = HyperscrapeProtocol::ParseChunkResponse(aMsg);
    if (chunks.isEmpty())
    {
        emit Log("No chunks assigned, will retry shortly...");
        QTimer::singleShot(5000, this, &MinervaWorker::RequestChunks);
        return;
    }

    int accepted = 0;
    for (const auto &chunk : chunks)
    {
        if (mSeenChunkIds.contains(chunk.chunkId))
        {
            if (mSocket && mSocket->isValid())
            {
                mSocket->sendBinaryMessage(HyperscrapeProtocol::BuildDetachChunk(chunk.chunkId));
            }
            continue;
        }
        mSeenChunkIds.insert(chunk.chunkId);

        int jobIdx = mNextJobIndex++;
        mActiveStreams++;
        accepted++;

        JobInfo info;
        info.chunkId = chunk.chunkId;
        info.fileId = chunk.fileId;
        info.url = chunk.url;
        info.rangeStart = chunk.rangeStart;
        info.rangeEnd = chunk.rangeEnd;

        JobState state;
        state.info = info;
        state.phase = JobPhase::Queued;
        state.total = static_cast<qint64>(chunk.rangeEnd - chunk.rangeStart);
        mJobs[jobIdx] = state;
        mChunkToJob[chunk.chunkId] = jobIdx;
        mChunkAssignments[jobIdx] = chunk;

        emit JobUpdated(jobIdx, state);
        StreamChunk(chunk, jobIdx);
    }

    emit Log(QString("Received %1 chunk assignments (%2 accepted).")
             .arg(chunks.size()).arg(accepted));
}

void MinervaWorker::HandleOk(const QByteArray &aMsg)
{
    auto kv = HyperscrapeProtocol::ParseKvResponse(aMsg);

    if (kv.hasError())
    {
        QString cid = kv.chunkId();
        emit Log(QString("Server error: %1 (chunk_id=%2)")
                 .arg(kv.error(), cid));
        if (!cid.isEmpty() && mChunkToJob.contains(cid))
        {
            int jobIdx = mChunkToJob[cid];
            FailChunk(jobIdx, cid, kv.error(), false);
        }
        ScheduleRequestChunks();
        return;
    }

    QString chunkId = kv.chunkId();
    if (!mChunkToJob.contains(chunkId))
    {
        return;
    }

    int jobIdx = mChunkToJob[chunkId];

    if (mChunkWaitingOk.value(jobIdx, false))
    {
        mChunkWaitingOk[jobIdx] = false;

        if (mJobs.contains(jobIdx))
        {
            JobState &js = mJobs[jobIdx];
            qint64 pending = mChunkPendingSubchunk.value(jobIdx).size()
                           + mChunkBuffers.value(jobIdx).size();
            js.progress = js.total - pending;
            if (mChunkHttpDone.value(jobIdx, false) && js.phase == JobPhase::Downloading)
            {
                js.phase = JobPhase::Uploading;
            }
            emit JobUpdated(jobIdx, js);
        }

        if (mChunkPendingSubchunk.contains(jobIdx) && !mChunkPendingSubchunk[jobIdx].isEmpty())
        {
            QByteArray &pending = mChunkPendingSubchunk[jobIdx];
            QByteArray subchunk = pending.left(HyperscrapeProtocol::SubchunkSize);
            pending.remove(0, subchunk.size());
            if (pending.isEmpty())
            {
                mChunkPendingSubchunk.remove(jobIdx);
            }
            SendSubchunk(jobIdx, chunkId, mJobs[jobIdx].info.fileId, subchunk);
            return;
        }
    }

    if (mChunkHttpDone.value(jobIdx, false)
        && !mChunkPendingSubchunk.contains(jobIdx))
    {
        JobState &js = mJobs[jobIdx];
        js.phase = JobPhase::Done;
        js.progress = js.total;
        emit JobUpdated(jobIdx, js);
        emit Log(QString("Chunk %1 done (%2)")
                 .arg(chunkId)
                 .arg(QLocale().formattedDataSize(js.total, 1, QLocale::DataSizeSIFormat)));

        mOkCount++;
        mBytesUp += js.total;
        mBytesDown += js.total;
        emit StatsChanged(mOkCount, mFailCount, mBytesUp, mBytesDown);

        mSeenChunkIds.remove(chunkId);
        mChunkToJob.remove(chunkId);
        mChunkAssignments.remove(jobIdx);
        mChunkBuffers.remove(jobIdx);
        mChunkBytesReceived.remove(jobIdx);
        mChunkWaitingOk.remove(jobIdx);
        mChunkHttpDone.remove(jobIdx);
        mActiveStreams--;
        ScheduleRequestChunks();
    }
}

void MinervaWorker::HandleError(const QByteArray &aMsg)
{
    auto kv = HyperscrapeProtocol::ParseKvResponse(aMsg);
    QString errorMsg = kv.error();
    QString chunkId = kv.chunkId();
    emit Log(QString("Server error for chunk %1: %2").arg(chunkId, errorMsg));

    if (mChunkToJob.contains(chunkId))
    {
        int jobIdx = mChunkToJob[chunkId];
        FailChunk(jobIdx, chunkId, errorMsg, false);
    }
    ScheduleRequestChunks();
}

void MinervaWorker::ScheduleRequestChunks()
{
    if (mRequestChunksTimer && !mRequestChunksTimer->isActive())
    {
        mRequestChunksTimer->start();
    }
}

void MinervaWorker::RequestChunks()
{
    if (!mRunning || !mRegistered || !mSocket)
    {
        return;
    }
    int needed = mCfg.concurrency - mActiveStreams;
    if (needed <= 0)
    {
        return;
    }
    mSocket->sendBinaryMessage(HyperscrapeProtocol::BuildGetChunks(needed));
}

void MinervaWorker::RetryChunk(int aJobIndex)
{
    if (!mRunning || !mJobs.contains(aJobIndex) || !mChunkAssignments.contains(aJobIndex))
    {
        return;
    }

    JobState &js = mJobs[aJobIndex];
    js.attempt++;
    js.progress = 0;
    js.error.clear();
    js.phase = JobPhase::Queued;
    emit JobUpdated(aJobIndex, js);

    mChunkBuffers.remove(aJobIndex);
    mChunkBytesReceived.remove(aJobIndex);
    mChunkWaitingOk.remove(aJobIndex);
    mChunkPendingSubchunk.remove(aJobIndex);
    mChunkHttpDone.remove(aJobIndex);

    emit Log(QString("Retrying chunk %1 (attempt %2/%3)...")
             .arg(mChunkAssignments[aJobIndex].chunkId)
             .arg(js.attempt)
             .arg(mCfg.subchunkRetries));

    StreamChunk(mChunkAssignments[aJobIndex], aJobIndex);
}

void MinervaWorker::FailChunk(int aJobIndex, const QString &aChunkId,
                               const QString &aError, bool aDetach)
{
    if (mChunkReplies.contains(aJobIndex) && mChunkReplies[aJobIndex])
    {
        mChunkReplies[aJobIndex]->abort();
    }

    if (mJobs.contains(aJobIndex))
    {
        JobState &js = mJobs[aJobIndex];
        js.phase = JobPhase::Failed;
        js.error = aError;
        emit JobUpdated(aJobIndex, js);
    }

    if (aDetach && mSocket && mSocket->isValid())
    {
        mSocket->sendBinaryMessage(HyperscrapeProtocol::BuildDetachChunk(aChunkId));
    }

    mFailCount++;
    emit StatsChanged(mOkCount, mFailCount, mBytesUp, mBytesDown);

    mSeenChunkIds.remove(aChunkId);
    mChunkToJob.remove(aChunkId);
    mChunkAssignments.remove(aJobIndex);
    mChunkReplies.remove(aJobIndex);
    mChunkBuffers.remove(aJobIndex);
    mChunkBytesReceived.remove(aJobIndex);
    mChunkWaitingOk.remove(aJobIndex);
    mChunkPendingSubchunk.remove(aJobIndex);
    mChunkHttpDone.remove(aJobIndex);
    mActiveStreams--;
}

void MinervaWorker::StreamChunk(const HyperscrapeProtocol::ChunkAssignment &aChunk, int aJobIndex)
{
    if (!mRunning || !mNam)
    {
        return;
    }

    JobState &js = mJobs[aJobIndex];
    js.phase = JobPhase::Downloading;
    emit JobUpdated(aJobIndex, js);

    QNetworkRequest req(QUrl(aChunk.url));
    QString rangeHeader = QString("bytes=%1-%2")
                              .arg(aChunk.rangeStart)
                              .arg(aChunk.rangeEnd - 1);
    req.setRawHeader("Range", rangeHeader.toUtf8());
    req.setTransferTimeout(60000);

    QNetworkReply *reply = mNam->get(req);
    mChunkReplies[aJobIndex] = reply;
    mChunkBuffers[aJobIndex] = QByteArray();
    mChunkBytesReceived[aJobIndex] = 0;
    mChunkWaitingOk[aJobIndex] = false;
    mChunkHttpDone[aJobIndex] = false;

    QString chunkId = aChunk.chunkId;
    QString fileId = aChunk.fileId;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, aJobIndex, chunkId, fileId]()
    {
        if (!mRunning || !mJobs.contains(aJobIndex))
        {
            return;
        }

        QByteArray data = reply->readAll();
        mChunkBytesReceived[aJobIndex] += data.size();
        mChunkBuffers[aJobIndex].append(data);

        while (mChunkBuffers.contains(aJobIndex)
               && mChunkBuffers[aJobIndex].size() >= HyperscrapeProtocol::SubchunkSize)
        {
            QByteArray subchunk = mChunkBuffers[aJobIndex].left(HyperscrapeProtocol::SubchunkSize);
            mChunkBuffers[aJobIndex].remove(0, HyperscrapeProtocol::SubchunkSize);

            if (mChunkWaitingOk.value(aJobIndex, false))
            {
                mChunkPendingSubchunk[aJobIndex].append(subchunk);
            }
            else
            {
                SendSubchunk(aJobIndex, chunkId, fileId, subchunk);
            }
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, aJobIndex, chunkId, fileId]()
    {
        reply->deleteLater();
        mChunkReplies.remove(aJobIndex);

        if (!mRunning || !mJobs.contains(aJobIndex))
        {
            return;
        }

        if (reply->error() != QNetworkReply::NoError
            && reply->error() != QNetworkReply::OperationCanceledError)
        {
            int httpStatus = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();

            if (httpStatus == 404)
            {
                emit Log(QString("Chunk %1: 404 Not Found (permanent, skipping)")
                         .arg(chunkId));
                FailChunk(aJobIndex, chunkId, "Not Found (404)", false);
                ScheduleRequestChunks();
                return;
            }

            JobState &js = mJobs[aJobIndex];
            if (js.attempt < mCfg.subchunkRetries)
            {
                int delayMs = kRetryDelayMs * js.attempt;
                emit Log(QString("HTTP error for chunk %1: %2 (retry in %3s)")
                         .arg(chunkId, reply->errorString())
                         .arg(delayMs / 1000));
                js.phase = JobPhase::Queued;
                js.error = reply->errorString();
                emit JobUpdated(aJobIndex, js);
                QTimer::singleShot(delayMs, this, [this, aJobIndex]()
                {
                    RetryChunk(aJobIndex);
                });
                return;
            }

            emit Log(QString("Chunk %1 failed after %2 attempts: %3")
                     .arg(chunkId)
                     .arg(js.attempt)
                     .arg(reply->errorString()));
            FailChunk(aJobIndex, chunkId, reply->errorString(), true);
            ScheduleRequestChunks();
            return;
        }

        mChunkHttpDone[aJobIndex] = true;

        QByteArray remaining = mChunkBuffers.take(aJobIndex);
        if (!remaining.isEmpty())
        {
            if (mChunkWaitingOk.value(aJobIndex, false))
            {
                mChunkPendingSubchunk[aJobIndex].append(remaining);
            }
            else
            {
                SendSubchunk(aJobIndex, chunkId, fileId, remaining);
            }
        }
        else if (!mChunkWaitingOk.value(aJobIndex, false)
                 && !mChunkPendingSubchunk.contains(aJobIndex))
        {
            JobState &js = mJobs[aJobIndex];
            js.phase = JobPhase::Done;
            js.progress = js.total;
            emit JobUpdated(aJobIndex, js);
            emit Log(QString("Chunk %1 done (%2)")
                     .arg(chunkId)
                     .arg(QLocale().formattedDataSize(js.total, 1, QLocale::DataSizeSIFormat)));

            mOkCount++;
            mBytesUp += js.total;
            mBytesDown += js.total;
            emit StatsChanged(mOkCount, mFailCount, mBytesUp, mBytesDown);

            mSeenChunkIds.remove(chunkId);
            mChunkToJob.remove(chunkId);
            mChunkAssignments.remove(aJobIndex);
            mChunkBytesReceived.remove(aJobIndex);
            mChunkWaitingOk.remove(aJobIndex);
            mChunkHttpDone.remove(aJobIndex);
            mActiveStreams--;
            ScheduleRequestChunks();
        }
    });
}

void MinervaWorker::SendSubchunk(int aJobIndex, const QString &aChunkId,
                                  const QString &aFileId, const QByteArray &aData)
{
    if (!mSocket || !mSocket->isValid())
    {
        return;
    }

    mChunkWaitingOk[aJobIndex] = true;
    QByteArray msg = HyperscrapeProtocol::BuildUploadSubchunk(aChunkId, aFileId, aData);
    mSocket->sendBinaryMessage(msg);
}

void MinervaWorker::UpdateConfig(const MinervaConfig &aCfg)
{
    if (!mRunning)
    {
        return;
    }

    int oldConcurrency = mCfg.concurrency;
    mCfg.concurrency = aCfg.concurrency;
    mCfg.subchunkRetries = aCfg.subchunkRetries;
    mCfg.reconnectDelaySec = aCfg.reconnectDelaySec;

    emit Log(QString("Config updated: concurrency=%1").arg(mCfg.concurrency));

    if (mCfg.concurrency > oldConcurrency)
    {
        ScheduleRequestChunks();
    }

    emit ConfigChanged();
}

void MinervaWorker::Stop()
{
    if (!mRunning)
    {
        return;
    }
    mRunning = false;

    for (auto it = mChunkReplies.begin(); it != mChunkReplies.end(); ++it)
    {
        if (it.value())
        {
            it.value()->abort();
        }
    }
    mChunkReplies.clear();

    for (auto it = mChunkToJob.constBegin(); it != mChunkToJob.constEnd(); ++it)
    {
        if (mSocket && mSocket->isValid())
        {
            mSocket->sendBinaryMessage(HyperscrapeProtocol::BuildDetachChunk(it.key()));
        }
    }

    mJobs.clear();
    mChunkToJob.clear();
    mChunkAssignments.clear();
    mChunkBuffers.clear();
    mChunkBytesReceived.clear();
    mChunkWaitingOk.clear();
    mChunkPendingSubchunk.clear();
    mChunkHttpDone.clear();
    mSeenChunkIds.clear();
    mActiveStreams = 0;

    if (mRequestChunksTimer)
    {
        mRequestChunksTimer->stop();
    }

    emit Log("Worker stopped.");
    emit Stopped();
}

void MinervaWorker::DoLogin(const QString &aServerUrl)
{
    Q_UNUSED(aServerUrl)

    QString redirectUri = QUrl::toPercentEncoding(
        QStringLiteral("https://firehose.minerva-archive.org/code"));
    QString oauthUrl = QStringLiteral(
        "https://discord.com/oauth2/authorize"
        "?client_id=1478862142793977998"
        "&response_type=code"
        "&redirect_uri=%1"
        "&scope=identify").arg(redirectUri);

    QDesktopServices::openUrl(QUrl(oauthUrl));
    emit Log("Opened browser for Discord login. Paste the token shown on the page.");
    emit LoginPrompt();
}

void MinervaWorker::SubmitToken(const QString &aToken)
{
    if (aToken.trimmed().isEmpty())
    {
        emit LoginResult(false, "No token provided");
        return;
    }

    if (!mNam)
    {
        mNam = new QNetworkAccessManager(this);
    }

    QString token = aToken.trimmed();
    QNetworkRequest req(QUrl(QStringLiteral("https://discord.com/api/users/@me")));
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setTransferTimeout(10000);

    QNetworkReply *reply = mNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, token]()
    {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
        {
            SaveToken(token);
            emit Log("Token verified with Discord.");
            emit LoginResult(true, {});
        }
        else
        {
            emit Log(QString("Token verification failed: %1").arg(reply->errorString()));
            emit LoginResult(false, "Token could not be verified with Discord");
        }
    });
}

void MinervaWorker::CheckVersion(const QString &aServerUrl)
{
    if (!mNam)
    {
        mNam = new QNetworkAccessManager(this);
    }

    QUrl url(aServerUrl + "/worker/version");
    QNetworkReply *reply = mNam->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
        {
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QString ver = doc.object().value("version").toString();
            emit VersionResult(ver);
        }
    });
}

void MinervaWorker::FetchLeaderboard(const QString &aApiBase,
                                      const QString &aToken,
                                      int aPage)
{
    if (!mNam)
    {
        mNam = new QNetworkAccessManager(this);
    }

    QUrl url(aApiBase + "/api/leaderboard");
    QUrlQuery q;
    q.addQueryItem("page", QString::number(aPage));
    url.setQuery(q);

    QNetworkRequest req(url);
    if (!aToken.isEmpty())
    {
        req.setRawHeader("Authorization", ("Bearer " + aToken).toUtf8());
    }
    req.setTransferTimeout(15000);

    QNetworkReply *reply = mNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, aPage]()
    {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
        {
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonArray rows;
            if (doc.isArray())
            {
                rows = doc.array();
            }
            else if (doc.isObject())
            {
                rows = doc.object().value("leaderboard").toArray();
                if (rows.isEmpty())
                {
                    rows = doc.object().value("data").toArray();
                }
            }
            emit LeaderboardResult(rows, aPage);
        }
        else
        {
            emit LeaderboardResult(QJsonArray(), aPage);
        }
    });
}

void MinervaWorker::FetchStats(const QString &aApiBase)
{
    if (!mNam)
    {
        mNam = new QNetworkAccessManager(this);
    }

    QUrl url(aApiBase + "/api/stats");
    QNetworkRequest req(url);
    req.setTransferTimeout(15000);

    QNetworkReply *reply = mNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
        {
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (doc.isObject())
            {
                emit NetworkStatsResult(doc.object());
            }
        }
    });
}
