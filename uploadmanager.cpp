#include "uploadmanager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrlQuery>
#include <QRandomGenerator>
#include <QFileInfo>

static const QSet<int> RETRIABLE_CODES = {408, 425, 429, 500, 502, 503, 504, 520, 521, 522, 523, 524};

bool UploadManager::IsRetryable(int aCode)
{
    return RETRIABLE_CODES.contains(aCode);
}

double UploadManager::RetrySleep(int aAttempt, double aCap)
{
    return std::min(aCap, (0.85 * aAttempt) + QRandomGenerator::global()->generateDouble() * 1.25);
}

UploadManager::UploadManager(QNetworkAccessManager *aNam, QObject *aParent)
    : QObject(aParent)
    , mNam(aNam)
    , mFileId(0)
    , mFile(nullptr)
    , mFileSize(0)
    , mSent(0)
    , mChunkSending(0)
    , mAborted(false)
{

}

QNetworkRequest UploadManager::MakeRequest(const QUrl &aUrl) const
{
    QNetworkRequest req(aUrl);
    req.setRawHeader("Authorization", ("Bearer " + mToken).toUtf8());
    req.setRawHeader("X-Minerva-Worker-Version", "1.2.4");
    req.setTransferTimeout(300000);
    return req;
}

void UploadManager::Upload(const QString &aUploadServerUrl, const QString &aToken,
                           int aFileId, const QString &aLocalPath)
{
    mUploadUrl = aUploadServerUrl;
    mToken = aToken;
    mFileId = aFileId;
    mLocalPath = aLocalPath;
    mSent = 0;
    mChunkSending = 0;
    mAborted = false;
    mHasher.reset();

    mFile = new QFile(aLocalPath, this);
    if (!mFile->open(QIODevice::ReadOnly))
    {
        emit Finished(false, "Cannot open file: " + aLocalPath);
        return;
    }
    mFileSize = mFile->size();
    emit Log(QString("[UL %1] Starting upload, %2").arg(aFileId).arg(QLocale().formattedDataSize(mFileSize)));

    emit Progress(0, mFileSize);

    StartSession();
}

void UploadManager::Abort()
{
    mAborted = true;
    if (mFile)
    {
        mFile->close();
        mFile->deleteLater();
        mFile = nullptr;
    }
}

void UploadManager::StartSession()
{
    QUrl url(QString("%1/api/upload/%2/start").arg(mUploadUrl).arg(mFileId));
    emit Log(QString("[UL %1] POST %2").arg(mFileId).arg(url.toString()));
    DoPost(url, {}, "application/json", mStartRetries, mDefaultRetryCap,
        [this](const QByteArray &aBody)
        {
            QJsonDocument doc = QJsonDocument::fromJson(aBody);
            mSessionId = doc.object().value("session_id").toString();
            if (mSessionId.isEmpty())
            {
                emit Log(QString("[UL %1] No session_id in response").arg(mFileId));
                emit Finished(false, "No session_id in start response");
                return;
            }
            emit Log(QString("[UL %1] Session started: %2").arg(mFileId).arg(mSessionId.left(12)));
            SendNextChunk();
        },
        [this](const QString &aError)
        {
            emit Log(QString("[UL %1] Start failed: %2").arg(mFileId).arg(aError));
            emit Finished(false, "upload start failed: " + aError);
        }
    );
}

void UploadManager::SendNextChunk()
{
    if (mAborted || !mFile)
    {
        return;
    }

    QByteArray data = mFile->read(mChunkSize);
    if (data.isEmpty())
    {
        FinishUpload();
        return;
    }
    mHasher.addData(data);

    QUrl url(QString("%1/api/upload/%2/chunk").arg(mUploadUrl).arg(mFileId));
    QUrlQuery q;
    q.addQueryItem("session_id", mSessionId);
    url.setQuery(q);

    DoPost(url, data, "application/octet-stream", mChunkRetries, mChunkRetryCap,
        [this, dataLen = data.size()](const QByteArray &)
        {
            mSent += dataLen;
            mChunkSending = 0;
            emit Progress(mSent, mFileSize);
            SendNextChunk();
        },
        [this](const QString &aError)
        {
            emit Log(QString("[UL %1] Chunk failed: %2").arg(mFileId).arg(aError));
            emit Finished(false, "upload chunk failed: " + aError);
        }
    );
}

void UploadManager::FinishUpload()
{
    if (mFile)
    {
        mFile->close();
        mFile->deleteLater();
        mFile = nullptr;
    }

    QString sha256 = QString::fromLatin1(mHasher.result().toHex());

    QUrl url(QString("%1/api/upload/%2/finish").arg(mUploadUrl).arg(mFileId));
    QUrlQuery q;
    q.addQueryItem("session_id", mSessionId);
    q.addQueryItem("expected_sha256", sha256);
    url.setQuery(q);

    emit Log(QString("[UL %1] Finishing, sha256=%2").arg(mFileId).arg(sha256.left(16)));
    DoPost(url, {}, "application/json", mFinishRetries, mChunkRetryCap,
        [this](const QByteArray &)
        {
            emit Log(QString("[UL %1] Upload complete").arg(mFileId));
            emit Finished(true, {});
        },
        [this](const QString &aError)
        {
            emit Log(QString("[UL %1] Finish failed: %2").arg(mFileId).arg(aError));
            emit Finished(false, "upload finish failed: " + aError);
        }
    );
}

void UploadManager::DoPost(const QUrl &aUrl, const QByteArray &aBody, const QByteArray &aContentType,
                           int aMaxRetries, float aRetryCap,
                           std::function<void(const QByteArray &)> aOnSuccess,
                           std::function<void(const QString &)> aOnFail,
                           int aAttempt)
{
    if (mAborted)
    {
        return;
    }

    QNetworkRequest req = MakeRequest(aUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, aContentType);

    QNetworkReply *reply = mNam->post(req, aBody);

    connect(reply, &QNetworkReply::uploadProgress, this, [this](qint64 bytesSent, qint64)
            {
                mChunkSending = bytesSent;
                if (mFileSize > 0)
                {
                    emit Progress(mSent + mChunkSending, mFileSize);
                }
            });

    connect(reply, &QNetworkReply::finished, this, [=, this]()
            {
                mChunkSending = 0;
                reply->deleteLater();
                if (mAborted)
                {
                    return;
                }

                int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

                if (httpCode == 426)
                {
                    QByteArray respBody = reply->readAll();
                    QJsonDocument doc = QJsonDocument::fromJson(respBody);
                    QString detail = doc.object().value("detail").toString("Worker update required");
                    aOnFail(detail);
                    return;
                }

                if (httpCode == 409)
                {
                    aOnFail("HTTP 409: duplicate");
                    return;
                }

                if (reply->error() != QNetworkReply::NoError || IsRetryable(httpCode))
                {
                    if (aAttempt >= aMaxRetries)
                    {
                        aOnFail(QString("HTTP %1: %2").arg(httpCode).arg(reply->errorString()));
                        return;
                    }
                    if (aAttempt <= 3)
                    {
                        emit Log(QString("[UL %1] Retry %2/%3 (HTTP %4)")
                                 .arg(mFileId).arg(aAttempt).arg(aMaxRetries).arg(httpCode));
                    }
                    int delayMs = static_cast<int>(RetrySleep(aAttempt, aRetryCap) * 1000);
                    QTimer::singleShot(delayMs, this, [=, this]()
                            {
                                DoPost(aUrl, aBody, aContentType, aMaxRetries, aRetryCap, aOnSuccess, aOnFail, aAttempt + 1);
                            });
                    return;
                }

                aOnSuccess(reply->readAll());
            });
}
