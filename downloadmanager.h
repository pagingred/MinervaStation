#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QObject>

class QNetworkAccessManager;
class QProcess;
class QNetworkReply;
class QFile;

class DownloadManager : public QObject
{
    Q_OBJECT

public:
    explicit DownloadManager(QNetworkAccessManager *aManager, QObject *aParent = nullptr);

    void Download(const QString &aUrlStr,
                  const QString &aDestPath,
                  int aAria2cConnections,
                  qint64 aKnownSize);
    void Abort();

    static bool HasAria2c();

signals:
    void Progress(qint64 aReceived, qint64 aTotal);
    void Finished(bool aOk, const QString &aError);

private:
    void DownloadAria2c(const QString &aUrl,
                        const QString &aDestPath,
                        int aConns,
                        qint64 aKnownSize);
    void StartAria2c(const QString &aUrl,
                     const QString &aDestPath,
                     int aConns);
    void DownloadHttp(const QString &aUrl,
                      const QString &aDestPath);

    QNetworkAccessManager *mManager;
    QProcess *mAria2c;
    QNetworkReply *mReply;
    QFile *mFile;
    QTimer *mPollTimer;
    QString mDestPath;
    qint64 mKnownSize;
    qint64 mResumeOffset;
    bool mAborted;
    QByteArray mStderrBuf;
};

#endif // DOWNLOADMANAGER_H
