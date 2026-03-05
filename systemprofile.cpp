#ifdef _WIN32
#define NOMINMAX
#endif

#include "systemprofile.h"

#include <QThread>
#include <QSysInfo>
#include <QStorageInfo>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <algorithm>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#include <mach/mach.h>
#elif defined(Q_OS_LINUX)
#include <sys/sysinfo.h>
#endif

SystemProfiler::SystemProfiler(QObject *aParent)
    : QObject(aParent)
{

}

SystemInfo SystemProfiler::DetectSystemInfo(const QString &aTempDirPath)
{
    SystemInfo info;

    info.cpuCores = QThread::idealThreadCount();
    info.cpuArch = QSysInfo::currentCpuArchitecture();
    info.osName = QSysInfo::prettyProductName();

#ifdef Q_OS_WIN
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
    {
        info.totalRamBytes = static_cast<qint64>(mem.ullTotalPhys);
        info.availRamBytes = static_cast<qint64>(mem.ullAvailPhys);
    }
#elif defined(Q_OS_MACOS)
    {
        int64_t physMem = 0;
        size_t len = sizeof(physMem);
        if (sysctlbyname("hw.memsize", &physMem, &len, nullptr, 0) == 0)
        {
            info.totalRamBytes = static_cast<qint64>(physMem);
        }
        mach_port_t host = mach_host_self();
        vm_statistics64_data_t vmstat;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(host, HOST_VM_INFO64,
                              reinterpret_cast<host_info64_t>(&vmstat), &count) == KERN_SUCCESS)
        {
            qint64 pageSize = static_cast<qint64>(vm_page_size);
            info.availRamBytes = static_cast<qint64>(vmstat.free_count + vmstat.inactive_count) * pageSize;
        }
    }
#elif defined(Q_OS_LINUX)
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0)
        {
            info.totalRamBytes = static_cast<qint64>(si.totalram) * si.mem_unit;
            info.availRamBytes = static_cast<qint64>(si.freeram + si.bufferram) * si.mem_unit;
        }
    }
#endif

    QString diskPath = aTempDirPath;
    if (aTempDirPath.isEmpty())
    {
        diskPath = ".";
    }

    QStorageInfo si(diskPath);
    if (si.isValid())
    {
        info.diskTotalBytes = si.bytesTotal();
        info.diskFreeBytes = si.bytesAvailable();
    }

    info.diskType = "Unknown";
#ifdef Q_OS_WIN
    {
        QProcess ps;
        ps.setProgram("powershell");
        ps.setArguments({"-NoProfile", "-Command",
            "Get-PhysicalDisk | Select MediaType,BusType | ConvertTo-Json"});
        ps.start();
        if (ps.waitForFinished(5000))
        {
            QByteArray out = ps.readAllStandardOutput();
            QJsonDocument doc = QJsonDocument::fromJson(out);

            std::function<QString(const QJsonObject &)> classify = [](const QJsonObject &aObj) -> QString
            {
                QString bus = aObj.value("BusType").toString();
                QString media = aObj.value("MediaType").toString();
                if (bus.contains("NVMe", Qt::CaseInsensitive))
                {
                    return "NVMe";
                }
                if (media.contains("SSD", Qt::CaseInsensitive))
                {
                    return "SSD";
                }
                if (media.contains("HDD", Qt::CaseInsensitive))
                {
                    return "HDD";
                }
                return "Unknown";
            };

            if (doc.isArray())
            {
                QJsonArray arr = doc.array();
                for (const QJsonValue &v : arr)
                {
                    QString t = classify(v.toObject());
                    if (t == "NVMe")
                    {
                        info.diskType = t;
                        break;
                    }
                    if (t == "SSD" && info.diskType != "NVMe")
                    {
                        info.diskType = t;
                    }
                    if (t == "HDD" && info.diskType == "Unknown")
                    {
                        info.diskType = t;
                    }
                }
            }
            else if (doc.isObject())
            {
                info.diskType = classify(doc.object());
            }
        }
    }
#elif defined(Q_OS_MACOS)
    {
        QProcess ps;
        ps.setProgram("diskutil");
        ps.setArguments({"info", "/"});
        ps.start();
        if (ps.waitForFinished(5000))
        {
            QString out = QString::fromUtf8(ps.readAllStandardOutput());
            if (out.contains("NVMe", Qt::CaseInsensitive))
            {
                info.diskType = "NVMe";
            }
            else if (out.contains("Solid State: Yes", Qt::CaseInsensitive) ||
                     out.contains("SSD", Qt::CaseInsensitive))
            {
                info.diskType = "SSD";
            }
            else if (out.contains("Solid State: No", Qt::CaseInsensitive))
            {
                info.diskType = "HDD";
            }
        }
    }
#elif defined(Q_OS_LINUX)
    {
        QStorageInfo sti(diskPath);
        QString dev = QString::fromUtf8(sti.device());
        QString devName = dev.section('/', -1);
        QString baseDev = devName;
        if (baseDev.startsWith("nvme"))
        {
            int pIdx = baseDev.lastIndexOf('p');
            if (pIdx > 4)
            {
                baseDev = baseDev.left(pIdx);
            }
        }
        else
        {
            while (!baseDev.isEmpty() && baseDev.back().isDigit())
            {
                baseDev.chop(1);
            }
        }
        if (baseDev.startsWith("nvme"))
        {
            info.diskType = "NVMe";
        }
        else
        {
            QFile rotFile(QString("/sys/block/%1/queue/rotational").arg(baseDev));
            if (rotFile.open(QIODevice::ReadOnly))
            {
                QString val = QString::fromUtf8(rotFile.readAll()).trimmed();
                info.diskType = (val == "0") ? "SSD" : "HDD";
            }
        }
    }
#endif

    return info;
}

void SystemProfiler::RunSpeedTest()
{
    mAborted = false;
    mResult = SpeedTestResult{};

    if (!mNam)
    {
        mNam = new QNetworkAccessManager(this);
    }

    emit SpeedTestProgress("Starting download test...", 0);
    StartDownloadTest();
}

void SystemProfiler::AbortSpeedTest()
{
    mAborted = true;
    if (mActiveReply)
    {
        mActiveReply->abort();
        mActiveReply = nullptr;
    }
}

void SystemProfiler::StartDownloadTest()
{
    if (mAborted)
    {
        return;
    }

    QUrl url("https://speed.cloudflare.com/__down?bytes=10485760");
    QNetworkRequest req(url);
    req.setTransferTimeout(30000);

    QElapsedTimer *timer = new QElapsedTimer;
    timer->start();

    mActiveReply = mNam->get(req);
    qint64 *received = new qint64(0);

    connect(mActiveReply, &QNetworkReply::readyRead, this, [this, received]()
    {
        *received += mActiveReply->readAll().size();
        int pct = static_cast<int>(std::min(100LL, *received * 100 / 10485760));
        emit SpeedTestProgress(QString("Downloading... %1%").arg(pct), pct / 2);
    });

    connect(mActiveReply, &QNetworkReply::finished, this, [this, timer, received]()
    {
        mActiveReply->deleteLater();
        mActiveReply = nullptr;

        if (mAborted)
        {
            delete timer;
            delete received;
            return;
        }

        qint64 elapsed = timer->elapsed();
        delete timer;

        if (*received > 0 && elapsed > 0)
        {
            double seconds = elapsed / 1000.0;
            double bits = static_cast<double>(*received) * 8.0;
            mResult.downloadMbps = bits / seconds / 1000000.0;
        }
        else
        {
            mResult.errors++;
            mResult.errorMessage += "Download test failed. ";
        }
        delete received;

        emit SpeedTestProgress("Starting upload test...", 50);
        StartUploadTest();
    });
}

void SystemProfiler::StartUploadTest()
{
    if (mAborted)
    {
        return;
    }

    QByteArray payload(5 * 1024 * 1024, '\0');
    QRandomGenerator *rng = QRandomGenerator::global();
    for (qsizetype i = 0; i < payload.size(); i += 4)
    {
        quint32 val = rng->generate();
        qsizetype remaining = payload.size() - i;
        qsizetype copyLen = remaining < 4 ? remaining : 4;
        memcpy(payload.data() + i, &val, static_cast<size_t>(copyLen));
    }

    QUrl url("https://speed.cloudflare.com/__up");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setTransferTimeout(30000);

    QElapsedTimer *timer = new QElapsedTimer;
    timer->start();
    qint64 totalBytes = payload.size();

    mActiveReply = mNam->post(req, payload);

    connect(mActiveReply, &QNetworkReply::uploadProgress, this, [this, totalBytes](qint64 aSent, qint64)
    {
        if (totalBytes > 0)
        {
            int pct = static_cast<int>(aSent * 100 / totalBytes);
            emit SpeedTestProgress(QString("Uploading... %1%").arg(pct), 50 + pct / 2);
        }
    });

    connect(mActiveReply, &QNetworkReply::finished, this, [this, timer, totalBytes]()
    {
        mActiveReply->deleteLater();
        mActiveReply = nullptr;

        if (mAborted)
        {
            delete timer;
            return;
        }

        qint64 elapsed = timer->elapsed();
        delete timer;

        if (elapsed > 0)
        {
            double seconds = elapsed / 1000.0;
            double bits = static_cast<double>(totalBytes) * 8.0;
            mResult.uploadMbps = bits / seconds / 1000000.0;
        }
        else
        {
            mResult.errors++;
            mResult.errorMessage += "Upload test failed. ";
        }

        mResult.success = (mResult.downloadMbps > 0 || mResult.uploadMbps > 0);
        emit SpeedTestProgress("Done", 100);
        emit SpeedTestFinished(mResult);
    });
}

RecommendedSettings SystemProfiler::Calculate(const SystemInfo &aSys, const SpeedTestResult &aSpeed)
{
    RecommendedSettings r;
    double dlMbps = aSpeed.downloadMbps;

    double networkFactor;
    if (dlMbps >= 500)
    {
        networkFactor = 2.0;
    }
    else if (dlMbps >= 200)
    {
        networkFactor = 1.5;
    }
    else if (dlMbps >= 100)
    {
        networkFactor = 1.2;
    }
    else if (dlMbps >= 50)
    {
        networkFactor = 1.0;
    }
    else if (dlMbps >= 25)
    {
        networkFactor = 0.7;
    }
    else
    {
        networkFactor = 0.5;
    }

    double rawConcurrency = (aSys.cpuCores / 4.0) * networkFactor;
    int concurrency = std::max(1, static_cast<int>(std::round(rawConcurrency)));

    if (aSys.availRamBytes > 0)
    {
        int ramCap = static_cast<int>(aSys.availRamBytes / (100LL * 1024 * 1024));
        concurrency = std::min(concurrency, std::max(1, ramCap));
    }
    r.concurrency = std::clamp(concurrency, 1, 1000);

    r.batchSize = std::clamp(r.concurrency * 2, 1, 1000);

    if (dlMbps >= 500)
    {
        r.aria2cConnections = 16;
    }
    else if (dlMbps >= 200)
    {
        r.aria2cConnections = 14;
    }
    else if (dlMbps >= 100)
    {
        r.aria2cConnections = 12;
    }
    else if (dlMbps >= 50)
    {
        r.aria2cConnections = 8;
    }
    else if (dlMbps >= 25)
    {
        r.aria2cConnections = 6;
    }
    else
    {
        r.aria2cConnections = 4;
    }
    r.aria2cConnections = std::clamp(r.aria2cConnections, 1, 16);

    return r;
}
