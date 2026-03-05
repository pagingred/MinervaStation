#ifndef UPLOADMANAGER_H
#define UPLOADMANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QCryptographicHash>

class UploadManager : public QObject
{
    Q_OBJECT

public:
    explicit UploadManager(QNetworkAccessManager *aNam,
                           QObject *aParent = nullptr);

    void Upload(const QString &aUploadServerUrl,
                const QString &aToken,
                int aFileId, const
                QString &aLocalPath);
    void Abort();

    qint64 mChunkSize = 8 * 1024 * 1024;
    int mStartRetries = 12;
    int mChunkRetries = 30;
    int mFinishRetries = 12;
    float mDefaultRetryCap = 25.0f;
    float mChunkRetryCap = 20.0f;

signals:
    void Progress(qint64 aSent,
                  qint64 aTotal);
    void Finished(bool aOk,
                  const QString &aError);
    void Log(const QString &aMsg);

private:
    void StartSession();
    void SendNextChunk();
    void FinishUpload();
    void DoPost(const QUrl &aUrl,
                const QByteArray &aBody,
                const QByteArray &aContentType,
                int aMaxRetries,
                float aRetryCap,
                std::function<void(const QByteArray &responseBody)> aOnSuccess,
                std::function<void(const QString &error)> aOnFail,
                int aAttempt = 1);
    QNetworkRequest MakeRequest(const QUrl &aUrl) const;
    static bool IsRetryable(int aCode);
    static double RetrySleep(int aAttempt,
                             double aCap);

    QNetworkAccessManager *mNam;
    QString mUploadUrl;
    QString mToken;
    int mFileId;
    QString mLocalPath;

    QString mSessionId;
    QFile *mFile;
    qint64 mFileSize;
    qint64 mSent;
    qint64 mChunkSending;
    QCryptographicHash mHasher{QCryptographicHash::Sha256};

    bool mAborted;
};

#endif // UPLOADMANAGER_H
