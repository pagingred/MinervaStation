#include "downloadmanager.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QDir>
#include <QTimer>

static bool sAria2cChecked = false;
static bool sHasAria2c = false;

DownloadManager::DownloadManager(QNetworkAccessManager *aManager,
                                 QObject *aParent)
    : QObject(aParent)
    , mManager(aManager)
    , mAria2c(nullptr)
    , mReply(nullptr)
    , mFile(nullptr)
    , mPollTimer(nullptr)
    , mDestPath()
    , mKnownSize(0)
    , mResumeOffset(0)
    , mAborted(false)
    , mStderrBuf()
{

}

void DownloadManager::Download(const QString &aUrlStr,
                               const QString &aDestPath,
                               int aAria2cConnections,
                               qint64 aKnownSize)
{
    QFileInfo fi(aDestPath);
    QDir().mkpath(fi.absolutePath());

    mDestPath = aDestPath;
    mKnownSize = aKnownSize;

    bool useAria2c = HasAria2c() && (aKnownSize == 0 || aKnownSize >= (5 * 1024 * 1024));
    if (useAria2c)
    {
        DownloadAria2c(aUrlStr, aDestPath, aAria2cConnections, aKnownSize);
    }
    else
    {
        DownloadHttp(aUrlStr, aDestPath);
    }
}

void DownloadManager::Abort()
{
    mAborted = true;
    if (mPollTimer)
    {
        mPollTimer->stop();
        mPollTimer->deleteLater();
        mPollTimer = nullptr;
    }
    if (mAria2c)
    {
        mAria2c->kill();
        mAria2c->waitForFinished(3000);
    }
    if (mReply)
    {
        mReply->abort();
    }
}

bool DownloadManager::HasAria2c()
{
    if (!sAria2cChecked)
    {
        sAria2cChecked = true;
        QString path = QStandardPaths::findExecutable("aria2c");
        sHasAria2c = !path.isEmpty();
    }
    return sHasAria2c;
}

void DownloadManager::DownloadAria2c(const QString &aUrl,
                                     const QString &aDestPath,
                                     int aConns,
                                     qint64 aKnownSize)
{
    if (aKnownSize > 0)
    {
        StartAria2c(aUrl, aDestPath, aConns);
        return;
    }

    QUrl requestUrl(aUrl);
    QNetworkRequest req(requestUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, static_cast<int>(QNetworkRequest::NoLessSafeRedirectPolicy));
    req.setTransferTimeout(15000);

    QNetworkReply *reply = mManager->head(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, aUrl, aDestPath, aConns]()
            {
                reply->deleteLater();
                if (!mAborted)
                {
                    qint64 cl = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
                    if (cl > 0)
                    {
                        mKnownSize = cl;
                    }

                    StartAria2c(aUrl, aDestPath, aConns);
                }
            });
}

void DownloadManager::StartAria2c(const QString &aUrl, const QString &aDestPath, int aConns)
{
    QFileInfo fi(aDestPath);
    QStringList args =
        {
            QString("--max-connection-per-server=%1").arg(aConns),
            QString("--split=%1").arg(aConns),
            "--min-split-size=1M",
            "--file-allocation=none",
            "--dir", fi.absolutePath(),
            "--out", fi.fileName(),
            "--auto-file-renaming=false",
            "--continue=true",
            "--allow-overwrite=true",
            "--summary-interval=1",
            "--human-readable=false",
            "--retry-wait=3",
            "--max-tries=5",
            "--timeout=60",
            "--connect-timeout=15",
            aUrl
        };

    mAria2c = new QProcess(this);
    mStderrBuf.clear();

    connect(mAria2c, &QProcess::readyReadStandardError, this, [this]()
            {
                QByteArray data = mAria2c->readAllStandardError();
                mStderrBuf.append(data);
                if (mStderrBuf.size() > 2048)
                {
                    mStderrBuf = mStderrBuf.right(2048);
                }

                int idx = data.lastIndexOf('[');
                if (idx >= 0)
                {
                    QByteArray line = data.mid(idx);
                    static QRegularExpression rx(R"((\d+)/(\d+)\()");
                    QRegularExpressionMatch match = rx.match(QString::fromUtf8(line));
                    if (match.hasMatch())
                    {
                        qint64 recv = match.captured(1).toLongLong();
                        qint64 total = match.captured(2).toLongLong();
                        if (total > 0)
                        {
                            mKnownSize = total;
                        }
                        emit Progress(recv, total > 0 ? total : (mKnownSize > 0 ? mKnownSize : -1));
                    }
                }
            });

    mPollTimer = new QTimer(this);
    connect(mPollTimer, &QTimer::timeout, this, [this, aDestPath]()
    {
        QFileInfo partial(aDestPath);
        if (partial.exists())
        {
            if (mKnownSize > 0)
            {
                emit Progress(partial.size(), mKnownSize);
            }
            else
            {
                emit Progress(partial.size(), -1);
            }
        }
    });
    mPollTimer->start(1000);

    connect(mAria2c, &QProcess::finished, this, [this, aDestPath](int exitCode, QProcess::ExitStatus)
            {
                if (mPollTimer)
                {
                    mPollTimer->stop();
                    mPollTimer->deleteLater();
                    mPollTimer = nullptr;
                }
                if (!mAborted)
                {
                    if (exitCode == 0)
                    {
                        QFileInfo fi(aDestPath);
                        qint64 finalSize = fi.exists() ? fi.size() : mKnownSize;
                        if (finalSize > 0)
                        {
                            emit Progress(finalSize, finalSize);
                        }

                        emit Finished(true, {});
                    }
                    else
                    {
                        QString err = QString::fromUtf8(mStderrBuf).trimmed();
                        int nl = err.lastIndexOf('\n');
                        if (nl >= 0)
                        {
                            err = err.mid(nl + 1).trimmed();
                        }

                        emit Finished(false, QString("aria2c exit %1: %2").arg(exitCode).arg(err.left(200)));
                    }
                    mAria2c->deleteLater();
                    mAria2c = nullptr;
                }
            });

    mAria2c->start("aria2c", args);
    if (!mAria2c->waitForStarted(5000))
    {
        if (mPollTimer)
        {
            mPollTimer->stop();
            mPollTimer->deleteLater();
            mPollTimer = nullptr;
        }

        emit Finished(false, "Failed to start aria2c: " + mAria2c->errorString());
        mAria2c->deleteLater();
        mAria2c = nullptr;
    }
}

void DownloadManager::DownloadHttp(const QString &aUrl, const QString &aDestPath)
{
    qint64 existingSize = 0;
    QFileInfo existing(aDestPath);
    if (existing.exists() && existing.size() > 0)
    {
        existingSize = existing.size();
    }

    mFile = new QFile(aDestPath, this);
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    if (existingSize > 0)
    {
        mode = QIODevice::WriteOnly | QIODevice::Append;
    }

    if (!mFile->open(mode))
    {
        emit Finished(false, "Cannot open file for writing: " + aDestPath);
        return;
    }

    mResumeOffset = existingSize;

    QUrl reqUrl(aUrl);
    QNetworkRequest req(reqUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, static_cast<int>(QNetworkRequest::NoLessSafeRedirectPolicy));
    req.setTransferTimeout(300000);

    if (existingSize > 0)
    {
        req.setRawHeader("Range", QString("bytes=%1-").arg(existingSize).toUtf8());
    }

    mReply = mManager->get(req);
    connect(mReply, &QNetworkReply::readyRead, this, [this]()
            {
                if (mReply && mFile)
                {
                    mFile->write(mReply->readAll());
                }
            });

    connect(mReply, &QNetworkReply::downloadProgress, this, [this](qint64 recv, qint64 total)
            {
                qint64 actualRecv = mResumeOffset + recv;
                qint64 actualTotal = total > 0 ? (mResumeOffset + total) : -1;

                emit Progress(actualRecv, actualTotal);
            });

    connect(mReply, &QNetworkReply::finished, this, [this, existingSize, aDestPath, aUrl]()
            {
                if (!mAborted)
                {
                    if (mFile)
                    {
                        mFile->flush();
                        mFile->close();
                    }

                    int httpCode = mReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (httpCode == 200 && existingSize > 0)
                    {
                        mReply->deleteLater();
                        mReply = nullptr;

                        if (mFile)
                        {
                            mFile->deleteLater();
                            mFile = nullptr;
                        }

                        QFile::remove(aDestPath);
                        mResumeOffset = 0;
                        DownloadHttp(aUrl, aDestPath);
                        return;
                    }

                    if (mReply->error() == QNetworkReply::NoError)
                    {
                        emit Finished(true, {});
                    }
                    else
                    {
                        emit Finished(false, QString("HTTP %1: %2").arg(httpCode).arg(mReply->errorString()));
                    }

                    mReply->deleteLater();
                    mReply = nullptr;

                    if (mFile)
                    {
                        mFile->deleteLater();
                        mFile = nullptr;
                    }
                }
            });
}
