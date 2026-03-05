#include "minervaworker.h"
#include "downloadmanager.h"
#include "uploadmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QDesktopServices>
#include <QDirIterator>
#include <QStorageInfo>

static const QSet<int> RETRIABLE_CODES = {408, 425, 429, 500, 502, 503, 504, 520, 521, 522, 523, 524};

static double retrySleep(int aAttempt, double aCap = 25.0)
{
    return std::min(aCap, (0.85 * aAttempt) + QRandomGenerator::global()->generateDouble() * 1.25);
}

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

QString MinervaWorker::SanitizeComponent(const QString &aPart)
{
    static const QString bad = "<>:\"/\\|?*";
    QString out;
    out.reserve(aPart.size());
    for (QChar ch : aPart)
    {
        if (bad.contains(ch) || ch.unicode() < 32)
        {
            out += '_';
        }
        else
        {
            out += ch;
        }
    }
    out = out.trimmed();
    while (out.endsWith('.'))
    {
        out.chop(1);
    }
    return out.isEmpty() ? "_" : out;
}

QString MinervaWorker::LocalPathForJob(const QString &aUrl, const QString &aDestPath) const
{
    QUrl parsed(aUrl);
    QString host = parsed.host();
    if (host.isEmpty())
    {
        host = "unknown-host";
    }

    QString decoded = QUrl::fromPercentEncoding(aDestPath.toUtf8());
    if (decoded.startsWith('/'))
    {
        decoded = decoded.mid(1);
    }

    QStringList parts;
    for (const QString &p : decoded.split('/', Qt::SkipEmptyParts))
    {
        parts << SanitizeComponent(p);
    }

    return mCfg.tempDir + "/" + host + "/" + parts.join("/");
}

QString MinervaWorker::PrettyPath(const QString &aRawPath)
{
    QString decoded = QUrl::fromPercentEncoding(aRawPath.toUtf8());
    if (decoded.startsWith('/'))
    {
        decoded = decoded.mid(1);
    }

    QStringList parts = decoded.split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty())
    {
        return decoded;
    }

    if (parts.size() > 4)
    {
        parts = parts.mid(parts.size() - 4);
        parts.prepend("...");
    }
    return parts.join(" > ");
}

const MinervaConfig &MinervaWorker::Config() const
{
    return mCfg;
}

QString MinervaWorker::LabelForPath(const QString &aPath)
{
    return PrettyPath(aPath);
}

QNetworkRequest MinervaWorker::AuthRequest(const QUrl &aUrl) const
{
    QNetworkRequest req(aUrl);
    req.setRawHeader("Authorization", ("Bearer " + mCfg.token).toUtf8());
    req.setRawHeader("X-Minerva-Worker-Version", mRemoteVersion.toUtf8());
    req.setTransferTimeout(30000);
    return req;
}

MinervaWorker::MinervaWorker(QObject *aParent)
    : QObject(aParent)
{
}

MinervaWorker::~MinervaWorker()
{
    Stop();
}

bool MinervaWorker::IsRunning() const
{
    return mRunning;
}

void MinervaWorker::MoveToDownloadsDir(const QString &aLocalPath)
{
    if (mCfg.downloadsDir.isEmpty())
    {
        return;
    }

    QString rel = aLocalPath;
    if (rel.startsWith(mCfg.tempDir))
    {
        rel = rel.mid(mCfg.tempDir.length());
        if (rel.startsWith('/'))
        {
            rel = rel.mid(1);
        }
    }

    QString destPath = mCfg.downloadsDir + "/" + rel;
    QDir().mkpath(QFileInfo(destPath).absolutePath());

    QFile::remove(destPath);
    if (QFile::rename(aLocalPath, destPath))
    {
        emit Log(QString("  Moved to downloads: %1").arg(destPath));
    }
    else
    {
        if (QFile::copy(aLocalPath, destPath))
        {
            QFile::remove(aLocalPath);
            emit Log(QString("  Copied to downloads: %1").arg(destPath));
        }
        else
        {
            emit Log(QString("  Warning: could not move file to downloads dir"));
        }
    }
}

void MinervaWorker::RecoverFromManifest()
{
    mManifest.Load();
    QMap<int, FileEntry> entries = mManifest.Entries();
    if (entries.isEmpty())
    {
        return;
    }

    emit Log(QString("Manifest recovery: %1 entries found").arg(entries.size()));

    int cleaned = 0, uploads = 0, downloads = 0, missing = 0, stale = 0;

    for (QMap<int, FileEntry>::const_iterator it = entries.constBegin(); it != entries.constEnd(); ++it)
    {
        const FileEntry &e = it.value();

        switch (e.state)
        {
        case FileState::Uploaded:
        case FileState::Duplicate:
            QFile::remove(e.localPath);
            mManifest.RemoveEntry(e.fileId);
            ++cleaned;
            break;

        case FileState::Downloaded:
        case FileState::Uploading:
        case FileState::UploadRetrying:
        {
            if (QFile::exists(e.localPath))
            {
                JobInfo job;
                job.fileId = e.fileId;
                job.url = e.url;
                job.destPath = e.destPath;
                job.size = e.size;
                mSeenIds.insert(e.fileId);
                EnqueueUpload(job, e.localPath, QFileInfo(e.localPath).size(), e.uploadRetryCount);
                ++uploads;
            }
            else
            {
                mManifest.RemoveEntry(e.fileId);
                ++missing;
            }
            break;
        }

        case FileState::Downloading:
        {
            bool hasPartial = QFile::exists(e.localPath + ".aria2");
            bool hasFile = QFile::exists(e.localPath);

            if (!hasPartial && !hasFile)
            {
                mManifest.RemoveEntry(e.fileId);
                ++stale;
            }
            else
            {
                mSeenIds.insert(e.fileId);
                ProbeFileNeeded(e);
                ++downloads;
            }
            break;
        }
        }
    }

    emit Log(QString("  Recovery: %1 uploads, %2 downloads, %3 cleaned, %4 missing, %5 stale")
             .arg(uploads).arg(downloads).arg(cleaned).arg(missing).arg(stale));
}

void MinervaWorker::ProbeFileNeeded(const FileEntry &aEntry)
{
    QUrl url(QString("%1/api/upload/%2/start").arg(mCfg.uploadUrl).arg(aEntry.fileId));
    QNetworkRequest req = AuthRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = mNam->post(req, QByteArray("{}"));
    int fileId = aEntry.fileId;
    FileEntry captured = aEntry;

    connect(reply, &QNetworkReply::finished, this, [this, reply, fileId, captured]()
    {
        reply->deleteLater();
        if (!mRunning)
        {
            return;
        }

        int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (httpCode == 409)
        {
            emit Log(QString("  Probe: server already has file %1 — removing")
                     .arg(LabelForPath(captured.destPath)));
            QFile::remove(captured.localPath);
            QFile::remove(captured.localPath + ".aria2");
            mManifest.RemoveEntry(fileId);
            mSeenIds.remove(fileId);
            return;
        }

        bool hasAria2 = QFile::exists(captured.localPath + ".aria2");
        bool hasFile = QFile::exists(captured.localPath);

        if (hasFile && !hasAria2)
        {
            qint64 fileSize = QFileInfo(captured.localPath).size();
            emit Log(QString("  Probe: resuming upload for %1 (%2)").arg(LabelForPath(captured.destPath)).arg(QLocale().formattedDataSize(fileSize)));

            JobInfo job;
            job.fileId = captured.fileId;
            job.url = captured.url;
            job.destPath = captured.destPath;
            job.size = captured.size;
            EnqueueUpload(job, captured.localPath, fileSize, captured.uploadRetryCount);
        }
        else
        {
            emit Log(QString("  Probe: resuming download for %1").arg(LabelForPath(captured.destPath)));

            JobInfo job;
            job.fileId = captured.fileId;
            job.url = captured.url;
            job.destPath = captured.destPath;
            job.size = captured.size;
            mQueue.enqueue(job);

            JobState state;
            state.info = job;
            state.phase = JobPhase::Queued;
            emit JobUpdated(fileId, state);
            TryDispatchDownloads();
        }
    });
}

void MinervaWorker::CleanOrphanFiles()
{
    QSet<QString> tracked;
    QMap<int, FileEntry> entries = mManifest.Entries();
    for (QMap<int, FileEntry>::const_iterator it = entries.constBegin(); it != entries.constEnd(); ++it)
    {
        tracked.insert(QDir::cleanPath(it.value().localPath));
        tracked.insert(QDir::cleanPath(it.value().localPath + ".aria2"));
    }

    QString manifestFile = QDir::cleanPath(mCfg.tempDir + "/manifest.json");
    QString manifestTmp = QDir::cleanPath(mCfg.tempDir + "/manifest.json.tmp");

    int removed = 0;
    qint64 freedBytes = 0;

    QDirIterator dit(mCfg.tempDir, QDir::Files, QDirIterator::Subdirectories);
    while (dit.hasNext())
    {
        dit.next();
        QString path = QDir::cleanPath(dit.filePath());

        if (path == manifestFile || path == manifestTmp)
        {
            continue;
        }

        if (!tracked.contains(path))
        {
            qint64 size = dit.fileInfo().size();
            if (QFile::remove(path))
            {
                freedBytes += size;
                removed++;
            }
        }
    }

    QDirIterator ddit(mCfg.tempDir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    QStringList dirs;
    while (ddit.hasNext())
    {
        ddit.next();
        dirs.append(ddit.filePath());
    }
    std::sort(dirs.begin(), dirs.end(), [](const QString &aA, const QString &aB)
    {
        return aA.length() > aB.length();
    });
    for (const QString &dir : dirs)
    {
        QDir d(dir);
        if (d.isEmpty())
        {
            d.rmdir(".");
        }
    }

    if (removed > 0)
    {
        emit Log(QString("Orphan cleanup: removed %1 file(s), freed %2").arg(removed).arg(QLocale().formattedDataSize(freedBytes)));
    }
}

void MinervaWorker::Start(const MinervaConfig &aCfg)
{
    if (mRunning)
    {
        return;
    }
    mCfg = aCfg;
    mRunning = true;
    mOkCount = mFailCount = 0;
    mBytesUp = 0;
    mBytesDown = 0;
    mDownloadSlots = 0;
    mUploadSlots = 0;
    mQueue.clear();
    mUploadQueue.clear();
    mSeenIds.clear();
    mNoJobsWarned = false;

    if (mCfg.tempDir.isEmpty())
    {
        mCfg.tempDir = QCoreApplication::applicationDirPath() + "/temp";
    }
    QDir().mkpath(mCfg.tempDir);

    if (mCfg.downloadsDir.isEmpty() && mCfg.keepFiles)
    {
        mCfg.downloadsDir = QCoreApplication::applicationDirPath() + "/downloads";
    }
    if (!mCfg.downloadsDir.isEmpty())
    {
        QDir().mkpath(mCfg.downloadsDir);
    }

    mNam = new QNetworkAccessManager(this);

    mManifest.SetDirectory(mCfg.tempDir);

    emit Log("Minerva Worker (native Qt)");
    emit Log(QString("  Server:      %1").arg(mCfg.serverUrl));
    emit Log(QString("  Upload API:  %1").arg(mCfg.uploadUrl));
    emit Log(QString("  Concurrency: %1 DL + %2 UL").arg(mCfg.dlConcurrency).arg(mCfg.ulConcurrency));

    if (mCfg.keepFiles)
    {
        emit Log("  Keep files:  yes");
    }
    else
    {
        emit Log("  Keep files:  no");
    }

    if (DownloadManager::HasAria2c())
    {
        emit Log(QString("  aria2c:      yes (%1 conns/file, httpx for <5MB)").arg(mCfg.aria2cConnections));
    }
    else
    {
        emit Log("  aria2c:      no (using Qt HTTP)");
    }

    emit Log(QString("  Temp dir:    %1").arg(mCfg.tempDir));

    if (!mCfg.downloadsDir.isEmpty())
    {
        emit Log(QString("  Downloads:   %1").arg(mCfg.downloadsDir));
    }

    emit Log({});
    emit Started();

    QUrl vUrl(mCfg.serverUrl + "/worker/version");
    emit Log(QString("[NET] GET %1").arg(vUrl.toString()));

    QNetworkReply *vReply = mNam->get(QNetworkRequest(vUrl));
    connect(vReply, &QNetworkReply::finished, this, [this, vReply]()
    {
        vReply->deleteLater();
        if (!mRunning)
        {
            return;
        }

        if (vReply->error() == QNetworkReply::NoError)
        {
            QJsonDocument doc = QJsonDocument::fromJson(vReply->readAll());
            QString ver = doc.object().value("version").toString();
            if (!ver.isEmpty())
            {
                mRemoteVersion = ver;
                emit Log(QString("  Server expects version: %1").arg(ver));
            }
        }

        RecoverFromManifest();

        CleanOrphanFiles();

        TryDispatchDownloads();
        TryDispatchUploads();

        mFetchTimer = new QTimer(this);
        connect(mFetchTimer, &QTimer::timeout, this, [this]()
        {
            TryDispatchUploads();
            FetchJobs();
        });

        mFetchTimer->start(500);
        FetchJobs();
    });
}

void MinervaWorker::UpdateConfig(const MinervaConfig &aCfg)
{
    if (!mRunning)
    {
        return;
    }

    int oldDl = mCfg.dlConcurrency;
    int oldUl = mCfg.ulConcurrency;
    mCfg.dlConcurrency = aCfg.dlConcurrency;
    mCfg.ulConcurrency = aCfg.ulConcurrency;
    mCfg.batchSize = aCfg.batchSize;
    mCfg.aria2cConnections = aCfg.aria2cConnections;
    mCfg.downloadRetryDelaySec = aCfg.downloadRetryDelaySec;
    mCfg.uploadRetryDelaySec = aCfg.uploadRetryDelaySec;
    mCfg.diskReserveBytes = aCfg.diskReserveBytes;
    mCfg.reportRetries = aCfg.reportRetries;
    mCfg.queuePrefetch = aCfg.queuePrefetch;
    mCfg.uploadChunkSize = aCfg.uploadChunkSize;
    mCfg.uploadStartRetries = aCfg.uploadStartRetries;
    mCfg.uploadChunkRetries = aCfg.uploadChunkRetries;
    mCfg.uploadFinishRetries = aCfg.uploadFinishRetries;
    mCfg.uploadRetryCap = aCfg.uploadRetryCap;
    mCfg.uploadChunkRetryCap = aCfg.uploadChunkRetryCap;

    emit Log(QString("Config updated: DL=%1, UL=%2, batch=%3, aria2c=%4").arg(mCfg.dlConcurrency).arg(mCfg.ulConcurrency).arg(mCfg.batchSize).arg(mCfg.aria2cConnections));

    if (mCfg.dlConcurrency > oldDl)
    {
        TryDispatchDownloads();
    }

    if (mCfg.ulConcurrency > oldUl)
    {
        TryDispatchUploads();
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

    if (mFetchTimer)
    {
        mFetchTimer->stop();
        mFetchTimer->deleteLater();
        mFetchTimer = nullptr;
    }

    for (QMap<int, UploadRetryEntry>::iterator it = mUploadRetryQueue.begin(); it != mUploadRetryQueue.end(); ++it)
    {
        if (it.value().timer)
        {
            it.value().timer->stop();
            it.value().timer->deleteLater();
        }
    }
    mUploadRetryQueue.clear();

    for (DownloadManager *dl : findChildren<DownloadManager*>())
    {
        dl->Abort();
    }
    mInFlightDownloadBytes = 0;

    for (UploadManager *ul : findChildren<UploadManager*>())
    {
        ul->Abort();
    }

    mManifest.Save();

    for (QNetworkAccessManager *nam : mUploadNamPool)
    {
        nam->deleteLater();
    }
    mUploadNamPool.clear();
    mUploadNamIndex = 0;

    emit Log("Worker stopped.");
    if (mNam)
    {
        mNam->deleteLater();
        mNam = nullptr;
    }
    emit Stopped();
}

void MinervaWorker::FetchJobs()
{
    if (!mRunning || !mNam)
    {
        return;
    }

    int maxQueue = mCfg.dlConcurrency * mCfg.queuePrefetch;
    if (mQueue.size() + mDownloadSlots >= maxQueue)
    {
        return;
    }

    QStorageInfo si(mCfg.tempDir);
    qint64 availableBytes = si.isValid() ? si.bytesAvailable() : -1;
    if (availableBytes >= 0)
    {
        qint64 committedBytes = mInFlightDownloadBytes;
        for (const JobInfo &qj : mQueue)
        {
            committedBytes += qj.size;
        }

        qint64 effectiveAvailable = availableBytes - committedBytes - mCfg.diskReserveBytes;
        if (effectiveAvailable <= 0)
        {
            if (!mNoJobsWarned)
            {
                QString availableByesStr = QLocale().formattedDataSize(availableBytes);
                QString committedBytesStr = QLocale().formattedDataSize(committedBytes);
                emit Log(QString("Disk space low (%1 available, %2 committed) — pausing job fetch").arg(availableByesStr, committedBytesStr));
                mNoJobsWarned = true;
            }
            return;
        }
    }

    int count = std::min(4, std::max(1, static_cast<int>(maxQueue - mQueue.size()) - mDownloadSlots));

    QUrl url(mCfg.serverUrl + "/api/jobs");
    QUrlQuery q;
    q.addQueryItem("count", QString::number(count));
    url.setQuery(q);

    QNetworkReply *reply = mNam->get(AuthRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        reply->deleteLater();
        if (!mRunning)
        {
            return;
        }

        int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (httpCode == 426)
        {
            emit Log("ERROR: Worker version outdated — server requires update!");
            Stop();
            return;
        }
        if (httpCode == 401)
        {
            emit Log("ERROR: Token expired — please log in again.");
            Stop();
            return;
        }
        if (reply->error() != QNetworkReply::NoError)
        {
            emit Log(QString("Server error fetching jobs: %1").arg(reply->errorString()));
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray jobs = doc.object().value("jobs").toArray();

        if (jobs.isEmpty())
        {
            if (!mNoJobsWarned)
            {
                emit Log("No jobs available, waiting...");
                mNoJobsWarned = true;
            }
            return;
        }

        QStorageInfo si(mCfg.tempDir);
        qint64 availableBytes = si.isValid() ? si.bytesAvailable() : -1;
        qint64 committedBytes = mInFlightDownloadBytes;
        for (const JobInfo &qj : mQueue)
        {
            committedBytes += qj.size;
        }

        mNoJobsWarned = false;
        int accepted = 0;
        int skipped = 0;
        for (const QJsonValue &val : jobs)
        {
            QJsonObject obj = val.toObject();
            int fileId = obj.value("file_id").toInt();
            if (mSeenIds.contains(fileId))
            {
                continue;
            }

            qint64 jobSize = obj.value("size").toInteger(0);

            if (availableBytes >= 0 && jobSize > 0)
            {
                qint64 effectiveAvail = availableBytes - committedBytes - mCfg.diskReserveBytes;
                if (effectiveAvail < jobSize)
                {
                    skipped++;
                    continue;
                }
                committedBytes += jobSize;
            }

            mSeenIds.insert(fileId);
            JobInfo job;
            job.fileId = fileId;
            job.url = obj.value("url").toString();
            job.destPath = obj.value("dest_path").toString();
            job.size = jobSize;
            mQueue.enqueue(job);

            JobState state;
            state.info = job;
            state.phase = JobPhase::Queued;
            emit JobUpdated(fileId, state);
            accepted++;
        }

        if (skipped > 0)
        {
            emit Log(QString("Skipped %1 job(s) — insufficient disk space (keeping %2 reserve)")
                     .arg(skipped)
                     .arg(QLocale().formattedDataSize(mCfg.diskReserveBytes)));
        }

        TryDispatchDownloads();
        TryDispatchUploads();
    });
}

void MinervaWorker::TryDispatchDownloads()
{
    while (mRunning && mDownloadSlots < mCfg.dlConcurrency && !mQueue.isEmpty())
    {
        JobInfo job = mQueue.dequeue();
        mDownloadSlots++;
        mInFlightDownloadBytes += job.size;
        ProcessJob(job);
    }
}

void MinervaWorker::EnsureUploadNamPool()
{
    int desired = std::min(mCfg.ulConcurrency, 20);
    while (mUploadNamPool.size() < desired)
    {
        mUploadNamPool.append(new QNetworkAccessManager(this));
    }
    while (mUploadNamPool.size() > desired)
    {
        QNetworkAccessManager *nam = mUploadNamPool.takeLast();
        nam->deleteLater();
    }
}

void MinervaWorker::TryDispatchUploads()
{
    if (!mRunning || mUploadQueue.isEmpty())
    {
        return;
    }
    if (mUploadSlots >= mCfg.ulConcurrency)
    {
        return;
    }

    UploadQueueEntry entry = mUploadQueue.dequeue();
    mUploadSlots++;
    DoUpload(entry.info, entry.localPath, entry.fileSize, entry.uploadRetryCount);

    if (!mUploadQueue.isEmpty() && mUploadSlots < mCfg.ulConcurrency)
    {
        QTimer::singleShot(200, this, &MinervaWorker::TryDispatchUploads);
    }
}

void MinervaWorker::ProcessJob(const JobInfo &aJob)
{
    QString localPath = LocalPathForJob(aJob.url, aJob.destPath);

    QUrl probeUrl(QString("%1/api/upload/%2/start").arg(mCfg.uploadUrl).arg(aJob.fileId));
    QNetworkRequest req = AuthRequest(probeUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = mNam->post(req, QByteArray("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, aJob, localPath]()
    {
        reply->deleteLater();
        if (!mRunning)
        {
            mDownloadSlots--;
            mInFlightDownloadBytes -= aJob.size;
            mSeenIds.remove(aJob.fileId);
            TryDispatchDownloads();
            return;
        }

        int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpCode == 409)
        {
            emit Log(QString("  Skipping %1 — server already has it")
                     .arg(LabelForPath(aJob.destPath)));
            DoReport(aJob.fileId, "failed", 0, "duplicate - server already has file", {}, false);
            mOkCount++;
            emit StatsChanged(mOkCount, mFailCount, mBytesUp, mBytesDown);

            emit JobRemoved(aJob.fileId);
            mDownloadSlots--;
            mInFlightDownloadBytes -= aJob.size;
            mSeenIds.remove(aJob.fileId);
            TryDispatchDownloads();
            return;
        }

        DoDownload(aJob, localPath, 1);
    });
}

void MinervaWorker::DoDownload(const JobInfo &aJob, const QString &aLocalPath, int aAttempt)
{
    if (!mRunning)
    {
        mDownloadSlots--;
        mInFlightDownloadBytes -= aJob.size;
        mSeenIds.remove(aJob.fileId);
        TryDispatchDownloads();
        return;
    }

    QString label = LabelForPath(aJob.destPath);

    JobState state;
    state.info = aJob;
    state.phase = JobPhase::Downloading;
    state.attempt = aAttempt;
    state.total = aJob.size;
    emit JobUpdated(aJob.fileId, state);

    FileEntry fe;
    fe.fileId = aJob.fileId;
    fe.url = aJob.url;
    fe.destPath = aJob.destPath;
    fe.localPath = aLocalPath;
    fe.size = aJob.size;
    fe.state = FileState::Downloading;
    fe.timestamp = QDateTime::currentDateTime();
    mManifest.SetEntry(aJob.fileId, fe);

    DownloadManager *dl = new DownloadManager(mNam, this);
    connect(dl, &DownloadManager::Progress, this, [this, aJob](qint64 aRecv, qint64 aTotal)
    {
        JobState s;
        s.info = aJob;
        s.phase = JobPhase::Downloading;
        s.progress = aRecv;
        s.total = aTotal > 0 ? aTotal : aJob.size;
        emit JobUpdated(aJob.fileId, s);
    });

    connect(dl, &DownloadManager::Finished, this,
            [this, dl, aJob, aLocalPath, aAttempt, label](bool aOk, const QString &aError)
    {
        dl->deleteLater();
        if (!mRunning)
        {
            mDownloadSlots--;
            mInFlightDownloadBytes -= aJob.size;
            mSeenIds.remove(aJob.fileId);
            TryDispatchDownloads();
            return;
        }

        if (!aOk)
        {
            QString shortErr = aError.left(72);
            emit Log(QString("  %1: retry %2 (%3)")
                     .arg(aJob.destPath).arg(aAttempt).arg(shortErr));

            JobState s;
            s.info = aJob;
            s.phase = JobPhase::Downloading;
            s.attempt = aAttempt + 1;
            s.error = shortErr;
            emit JobUpdated(aJob.fileId, s);

            int delayMs = mCfg.downloadRetryDelaySec * aAttempt * 1000;
            QTimer::singleShot(delayMs, this, [this, aJob, aLocalPath, aAttempt]()
            {
                DoDownload(aJob, aLocalPath, aAttempt + 1);
            });
            return;
        }

        mManifest.SetEntry(aJob.fileId, FileState::Downloaded);
        mDownloadSlots--;
        mInFlightDownloadBytes -= aJob.size;

        QFileInfo fi(aLocalPath);
        qint64 fileSize = fi.size();
        mBytesDown += fileSize;
        emit StatsChanged(mOkCount, mFailCount, mBytesUp, mBytesDown);
        emit Log(QString("  DL done %1 (%2) — queuing upload").arg(label).arg(QLocale().formattedDataSize(fileSize)));

        EnqueueUpload(aJob, aLocalPath, fileSize);
        TryDispatchDownloads();
    });

    dl->Download(aJob.url, aLocalPath, mCfg.aria2cConnections, aJob.size);
}

void MinervaWorker::EnqueueUpload(const JobInfo &aJob, const QString &aLocalPath,
                                   qint64 aFileSize, int aUploadRetryCount)
{
    JobState qs;
    qs.info = aJob;
    qs.phase = JobPhase::QueuedUpload;
    qs.total = aFileSize;
    emit JobUpdated(aJob.fileId, qs);

    UploadQueueEntry entry;
    entry.info = aJob;
    entry.localPath = aLocalPath;
    entry.fileSize = aFileSize;
    entry.uploadRetryCount = aUploadRetryCount;
    mUploadQueue.enqueue(entry);
    TryDispatchUploads();
}

void MinervaWorker::DoUpload(const JobInfo &aJob, const QString &aLocalPath, qint64 aFileSize,
                             int aUploadRetryCount)
{
    if (!mRunning)
    {
        mUploadSlots--;
        mSeenIds.remove(aJob.fileId);
        TryDispatchUploads();
        return;
    }

    mManifest.SetEntry(aJob.fileId, FileState::Uploading);

    JobState state;
    state.info = aJob;
    state.phase = JobPhase::Uploading;
    state.total = aFileSize;
    emit JobUpdated(aJob.fileId, state);

    EnsureUploadNamPool();
    QNetworkAccessManager *ulNam = mUploadNamPool[mUploadNamIndex % mUploadNamPool.size()];
    mUploadNamIndex++;

    UploadManager *ul = new UploadManager(ulNam, this);
    ul->mChunkSize = mCfg.uploadChunkSize;
    ul->mStartRetries = mCfg.uploadStartRetries;
    ul->mChunkRetries = mCfg.uploadChunkRetries;
    ul->mFinishRetries = mCfg.uploadFinishRetries;
    ul->mDefaultRetryCap = mCfg.uploadRetryCap;
    ul->mChunkRetryCap = mCfg.uploadChunkRetryCap;
    connect(ul, &UploadManager::Log, this, &MinervaWorker::Log);
    connect(ul, &UploadManager::Progress, this, [this, aJob, aFileSize](qint64 aSent, qint64 aTotal)
    {
        JobState s;
        s.info = aJob;
        s.phase = JobPhase::Uploading;
        s.progress = aSent;
        s.total = aTotal > 0 ? aTotal : aFileSize;
        emit JobUpdated(aJob.fileId, s);
    });

    connect(ul, &UploadManager::Finished, this, [this, ul, aJob, aLocalPath, aFileSize, aUploadRetryCount](bool aOk, const QString &aError)
    {
        ul->deleteLater();

        if (!aOk)
        {
            if (aError.contains("409") || aError.contains("duplicate", Qt::CaseInsensitive))
            {
                emit Log(QString("  Duplicate detected for %1 — server already has it").arg(LabelForPath(aJob.destPath)));

                mManifest.SetEntry(aJob.fileId, FileState::Duplicate);
                QFile::remove(aLocalPath);
                QFile::remove(aLocalPath + ".aria2");
                mManifest.RemoveEntry(aJob.fileId);

                JobState ds;
                ds.info = aJob;
                ds.phase = JobPhase::Done;
                ds.progress = aFileSize;
                ds.total = aFileSize;
                emit JobUpdated(aJob.fileId, ds);

                DoReport(aJob.fileId, "failed", 0, "duplicate - server already has file", aLocalPath, false);
                mOkCount++;
                emit StatsChanged(mOkCount, mFailCount, mBytesUp, mBytesDown);

                mUploadSlots--;
                mSeenIds.remove(aJob.fileId);
                TryDispatchUploads();
                return;
            }

            emit Log(QString("  Upload failed for %1: %2 — scheduling retry").arg(LabelForPath(aJob.destPath), aError));

            mUploadSlots--;
            ScheduleUploadRetry(aJob, aLocalPath, aFileSize, aUploadRetryCount, aError);
            TryDispatchUploads();
            return;
        }

        mManifest.SetEntry(aJob.fileId, FileState::Uploaded);

        JobState ds;
        ds.info = aJob;
        ds.phase = JobPhase::Done;
        ds.progress = aFileSize;
        ds.total = aFileSize;
        emit JobUpdated(aJob.fileId, ds);

        emit Log(QString("  OK %1 (%2)").arg(LabelForPath(aJob.destPath)).arg(QLocale().formattedDataSize(aFileSize)));

        if (mCfg.keepFiles)
        {
            MoveToDownloadsDir(aLocalPath);
        }
        else
        {
            QFile::remove(aLocalPath);
            QFile::remove(aLocalPath + ".aria2");
        }
        mManifest.RemoveEntry(aJob.fileId);

        int capturedFileId = aJob.fileId;
        ReportWithRetry(aJob.fileId, "completed", aFileSize, {}, 1, mCfg.reportRetries, [this, capturedFileId, aFileSize](bool aOk)
            {
                if (aOk)
                {
                    mBytesUp += aFileSize;
                    mOkCount++;
                }
                else
                {
                    mFailCount++;
                    emit Log(QString("  UNCREDITED: file %1 (%2) uploaded but report failed").arg(capturedFileId).arg(QLocale().formattedDataSize(aFileSize)));
                }
                emit StatsChanged(mOkCount, mFailCount, mBytesUp, mBytesDown);
            });

        mUploadSlots--;
        mSeenIds.remove(aJob.fileId);
        TryDispatchUploads();
    });

    ul->Upload(mCfg.uploadUrl, mCfg.token, aJob.fileId, aLocalPath);
}

void MinervaWorker::ScheduleUploadRetry(const JobInfo &aJob,
                                        const QString &aLocalPath,
                                        qint64 aFileSize,
                                        int aRetryCount,
                                        const QString &aError)
{
    emit Log(QString("  Upload retry %1 for %2 in %3s").arg(aRetryCount + 1).arg(LabelForPath(aJob.destPath)).arg(mCfg.uploadRetryDelaySec));

    mManifest.SetEntry(aJob.fileId, FileState::UploadRetrying);
    if (mManifest.Contains(aJob.fileId))
    {
        FileEntry fe = mManifest.Entry(aJob.fileId);
        fe.uploadRetryCount = aRetryCount + 1;
        mManifest.SetEntry(aJob.fileId, fe);
    }

    JobState ws;
    ws.info = aJob;
    ws.phase = JobPhase::UploadRetryWait;
    ws.total = aFileSize;
    ws.attempt = aRetryCount + 1;
    ws.error = QString("Retry in %1s: %2").arg(mCfg.uploadRetryDelaySec).arg(aError.left(60));
    emit JobUpdated(aJob.fileId, ws);

    UploadRetryEntry entry;
    entry.info = aJob;
    entry.localPath = aLocalPath;
    entry.fileSize = aFileSize;
    entry.retryCount = aRetryCount + 1;
    entry.timer = new QTimer(this);
    entry.timer->setSingleShot(true);

    connect(entry.timer, &QTimer::timeout, this, [this, fileId = aJob.fileId]()
    {
        ExecuteUploadRetry(fileId);
    });

    mUploadRetryQueue.insert(aJob.fileId, entry);
    entry.timer->start(mCfg.uploadRetryDelaySec * 1000);
}

void MinervaWorker::ExecuteUploadRetry(int aFileId)
{
    if (!mUploadRetryQueue.contains(aFileId))
    {
        return;
    }

    UploadRetryEntry entry = mUploadRetryQueue.take(aFileId);
    if (entry.timer)
    {
        entry.timer->deleteLater();
    }

    if (!mRunning)
    {
        return;
    }

    if (!QFile::exists(entry.localPath))
    {
        emit Log(QString("  Upload retry for %1: file no longer exists").arg(LabelForPath(entry.info.destPath)));
        mManifest.RemoveEntry(aFileId);
        mSeenIds.remove(aFileId);

        JobState fs;
        fs.info = entry.info;
        fs.phase = JobPhase::Failed;
        fs.error = "File missing during retry";
        emit JobUpdated(aFileId, fs);
        return;
    }

    emit Log(QString("  Upload retry: probing server for %1 before retry %2").arg(LabelForPath(entry.info.destPath)).arg(entry.retryCount));

    QUrl url(QString("%1/api/upload/%2/start").arg(mCfg.uploadUrl).arg(aFileId));
    QNetworkRequest req = AuthRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = mNam->post(req, QByteArray("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, aFileId]()
    {
        reply->deleteLater();
        if (!mRunning)
        {
            return;
        }

        int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (httpCode == 409)
        {
            emit Log(QString("  Upload retry: server already has %1 — skipping").arg(LabelForPath(entry.info.destPath)));

            mManifest.SetEntry(aFileId, FileState::Duplicate);
            QFile::remove(entry.localPath);
            QFile::remove(entry.localPath + ".aria2");
            mManifest.RemoveEntry(aFileId);

            JobState ds;
            ds.info = entry.info;
            ds.phase = JobPhase::Done;
            ds.progress = entry.fileSize;
            ds.total = entry.fileSize;
            emit JobUpdated(aFileId, ds);

            DoReport(aFileId, "failed", 0, "duplicate - server already has file", entry.localPath, false);
            mOkCount++;
            emit StatsChanged(mOkCount, mFailCount, mBytesUp, mBytesDown);
            mSeenIds.remove(aFileId);
            return;
        }

        emit Log(QString("  Retrying upload for %1 (attempt %2)").arg(LabelForPath(entry.info.destPath)).arg(entry.retryCount));
        EnqueueUpload(entry.info, entry.localPath, entry.fileSize, entry.retryCount);
    });
}

void MinervaWorker::ForceRetryUpload(int aFileId)
{
    if (!mRunning)
    {
        return;
    }

    if (mUploadRetryQueue.contains(aFileId))
    {
        emit Log(QString("  Force retry upload for file %1").arg(aFileId));
        ExecuteUploadRetry(aFileId);
        return;
    }

    for (const UploadQueueEntry &e : mUploadQueue)
    {
        if (e.info.fileId == aFileId)
        {
            return;
        }
    }

    if (mManifest.Contains(aFileId))
    {
        FileEntry fe = mManifest.Entry(aFileId);
        if (QFile::exists(fe.localPath))
        {
            emit Log(QString("  Force retry: re-enqueuing upload for file %1").arg(aFileId));
            JobInfo job;
            job.fileId = fe.fileId;
            job.url = fe.url;
            job.destPath = fe.destPath;
            job.size = fe.size;
            mSeenIds.insert(aFileId);
            EnqueueUpload(job, fe.localPath, QFileInfo(fe.localPath).size(), fe.uploadRetryCount);
        }
    }
}

void MinervaWorker::DoReport(int aFileId, const QString &aStatus, qint64 aBytesDownloaded,
                             const QString &aError, const QString &, bool)
{
    ReportWithRetry(aFileId, aStatus, aBytesDownloaded, aError, 1, mCfg.reportRetries, [this, aFileId, aStatus](bool aOk)
    {
        if (!aOk)
        {
            emit Log(QString("  Warning: report for file %1 (%2) failed after retries")
                     .arg(aFileId).arg(aStatus));
        }
    });
}

void MinervaWorker::ReportWithRetry(int aFileId, const QString &aStatus, qint64 aBytes,
                                    const QString &aError, int aAttempt, int aMaxAttempts,
                                    std::function<void(bool)> aDone)
{
    if (!mNam)
    {
        emit Log(QString("[RPT %1] FAILED — no NAM (worker stopped?)").arg(aFileId));
        if (aDone)
        {
            aDone(false);
        }
        return;
    }

    QUrl url(mCfg.serverUrl + "/api/jobs/report");
    QNetworkRequest req = AuthRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["file_id"] = aFileId;
    body["status"] = aStatus;
    body["bytes_downloaded"] = (aBytes > 0) ? QJsonValue(aBytes) : QJsonValue(QJsonValue::Null);
    body["error"] = aError.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(aError);

    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (aAttempt == 1)
    {
        emit Log(QString("[RPT %1] POST %2").arg(aFileId).arg(QString::fromUtf8(payload)));
    }

    QNetworkReply *reply = mNam->post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [=, this]()
    {
        reply->deleteLater();
        int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QByteArray respBody = reply->readAll();

        if (httpCode == 409 && aStatus == "completed")
        {
            QString detail = QString::fromUtf8(respBody).toLower();
            bool isFinalizeRace = detail.contains("not finalized") || detail.contains("upload");
            if (isFinalizeRace && aAttempt < aMaxAttempts)
            {
                if (aAttempt <= 2)
                {
                    emit Log(QString("[RPT %1] 409 finalize race — retry %2/%3").arg(aFileId).arg(aAttempt).arg(aMaxAttempts));
                }
                int delayMs = static_cast<int>(std::min(2000.0, (0.25 + aAttempt * 0.1) * 1000));
                QTimer::singleShot(delayMs, this, [=, this]()
                {
                    ReportWithRetry(aFileId, aStatus, aBytes, aError, aAttempt + 1, aMaxAttempts, aDone);
                });
                return;
            }
            emit Log(QString("[RPT %1] 409 — %2 (attempt %3/%4)").arg(aFileId).arg(QString::fromUtf8(respBody).left(200)).arg(aAttempt).arg(aMaxAttempts));
            if (aDone)
            {
                aDone(false);
            }
            return;
        }

        if (RETRIABLE_CODES.contains(httpCode) || reply->error() != QNetworkReply::NoError)
        {
            if (aAttempt < aMaxAttempts)
            {
                if (aAttempt <= 2)
                {
                    emit Log(QString("[RPT %1] HTTP %2 — retry %3/%4").arg(aFileId).arg(httpCode).arg(aAttempt).arg(aMaxAttempts));
                }
                int delayMs = static_cast<int>(retrySleep(aAttempt) * 1000);
                QTimer::singleShot(delayMs, this, [=, this]()
                {
                    ReportWithRetry(aFileId, aStatus, aBytes, aError, aAttempt + 1, aMaxAttempts, aDone);
                });
                return;
            }
            emit Log(QString("[RPT %1] FAILED — HTTP %2 after %3 retries: %4").arg(aFileId).arg(httpCode).arg(aMaxAttempts).arg(QString::fromUtf8(respBody).left(200)));
            if (aDone)
            {
                aDone(false);
            }
            return;
        }

        if (httpCode >= 200 && httpCode < 300)
        {
            QString resp = QString::fromUtf8(respBody).trimmed();
            emit Log(QString("[RPT %1] OK — %2 reported (HTTP %3) %4").arg(aFileId).arg(aStatus).arg(httpCode).arg(resp.left(120)));
            if (aDone)
            {
                aDone(true);
            }
        }
        else
        {
            emit Log(QString("[RPT %1] REJECTED — HTTP %2: %3").arg(aFileId).arg(httpCode).arg(QString::fromUtf8(respBody).left(200)));
            if (aDone)
            {
                aDone(false);
            }
        }
    });
}

void MinervaWorker::DoLogin(const QString &aServerUrl)
{
    if (mLoginServer)
    {
        mLoginServer->close();
        mLoginServer->deleteLater();
    }

    mLoginServer = new QTcpServer(this);
    if (!mLoginServer->listen(QHostAddress::LocalHost, 19283))
    {
        emit LoginResult(false, "Cannot listen on port 19283");
        mLoginServer->deleteLater();
        mLoginServer = nullptr;
        return;
    }

    connect(mLoginServer, &QTcpServer::newConnection, this, [this]()
    {
        QTcpSocket *sock = mLoginServer->nextPendingConnection();
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]()
        {
            QString data = QString::fromUtf8(sock->readAll());
            int gi = data.indexOf("GET ");
            if (gi < 0)
            {
                sock->disconnectFromHost();
                return;
            }
            int ei = data.indexOf(" HTTP", gi + 4);
            QString path = data.mid(gi + 4, ei - gi - 4);
            QUrlQuery q(QUrl("http://localhost" + path));
            if (q.hasQueryItem("token"))
            {
                QString token = q.queryItemValue("token");
                SaveToken(token);
                sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<h1>Logged in! You can close this tab.</h1>");
                sock->flush();
                sock->disconnectFromHost();
                mLoginServer->close();
                emit LoginResult(true, token);
            }
            else
            {
                sock->write("HTTP/1.1 400 Bad Request\r\n\r\n");
                sock->flush();
                sock->disconnectFromHost();
            }
        });
    });

    QString base = aServerUrl.isEmpty() ? "https://api.minerva-archive.org" : aServerUrl;
    QString loginUrl = base + "/auth/discord/login?worker_callback=http://127.0.0.1:19283/";
    emit Log("Opening browser for Discord login...");
    QDesktopServices::openUrl(QUrl(loginUrl));
}

void MinervaWorker::CheckVersion(const QString &aServerUrl)
{
    if (!mNam)
    {
        mNam = new QNetworkAccessManager(this);
    }
    QString base = aServerUrl.isEmpty() ? "https://api.minerva-archive.org" : aServerUrl;
    QString endpoint = base + "/worker/version";
    emit Log(QString("[NET] GET %1").arg(endpoint));

    QUrl endpointUrl(endpoint);
    QNetworkRequest req(endpointUrl);
    QNetworkReply *reply = mNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, endpoint]()
    {
        reply->deleteLater();
        int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError)
        {
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QString ver = doc.object().value("version").toString("unknown");
            emit Log(QString("[NET] %1 -> %2 (version: %3)").arg(endpoint).arg(code).arg(ver));
            emit VersionResult(ver);
        }
        else
        {
            emit Log(QString("[NET] %1 -> %2 FAILED: %3").arg(endpoint).arg(code).arg(reply->errorString()));
            emit VersionResult({});
        }
    });
}

void MinervaWorker::FetchLeaderboard(const QString &aServerUrl, const QString &aToken, int aPage)
{
    if (!mNam)
    {
        mNam = new QNetworkAccessManager(this);
    }
    QString base = aServerUrl.isEmpty() ? "https://api.minerva-archive.org" : aServerUrl;
    QUrl url(base + "/api/leaderboard");
    if (aPage > 1)
    {
        QUrlQuery q;
        q.addQueryItem("offset", QString::number((aPage - 1) * 50));
        url.setQuery(q);
    }

    QString endpoint = url.toString();
    emit Log(QString("[NET] GET %1").arg(endpoint));

    QNetworkRequest req(url);
    if (!aToken.isEmpty())
    {
        req.setRawHeader("Authorization", ("Bearer " + aToken).toUtf8());
        req.setRawHeader("X-Minerva-Worker-Version", mRemoteVersion.toUtf8());
    }
    QNetworkReply *reply = mNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, endpoint, aPage]()
    {
        reply->deleteLater();
        int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError)
        {
            emit Log(QString("[NET] %1 -> %2 FAILED: %3").arg(endpoint).arg(code).arg(reply->errorString()));
            emit LeaderboardResult({}, aPage);
            return;
        }
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonArray rows;
        if (doc.isArray())
        {
            rows = doc.array();
        }
        else if (doc.isObject())
        {
            QJsonObject obj = doc.object();
            for (const auto &key : {"leaderboard", "results", "data", "users"})
            {
                if (obj.contains(key) && obj.value(key).isArray())
                {
                    rows = obj.value(key).toArray();
                    break;
                }
            }
        }
        emit Log(QString("[NET] %1 -> %2 OK (%3 entries)").arg(endpoint).arg(code).arg(rows.size()));
        emit LeaderboardResult(rows, aPage);
    });
}
