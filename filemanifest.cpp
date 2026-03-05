#include "filemanifest.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

static QString stateToString(FileState s)
{
    switch (s)
    {
    case FileState::Downloading:
        return "downloading";
    case FileState::Downloaded:
        return "downloaded";
    case FileState::Uploading:
        return "uploading";
    case FileState::UploadRetrying:
        return "upload_retrying";
    case FileState::Uploaded:
        return "uploaded";
    case FileState::Duplicate:
        return "duplicate";
    }
    return "unknown";
}

static FileState stateFromString(const QString &s)
{
    if (s == "downloading")
    {
        return FileState::Downloading;
    }
    else if (s == "downloaded")
    {
        return FileState::Downloaded;
    }
    else if (s == "uploading")
    {
        return FileState::Uploading;
    }
    else if (s == "upload_retrying")
    {
        return FileState::UploadRetrying;
    }
    else if (s == "uploaded")
    {
        return FileState::Uploaded;
    }
    else if (s == "duplicate")
    {
        return FileState::Duplicate;
    }
    return FileState::Downloading;
}

FileManifest::FileManifest(const QString &aDirectory)
    : mDirectory(aDirectory)
{

}

void FileManifest::SetDirectory(const QString &aDirectory)
{
    mDirectory = aDirectory;
}

QString FileManifest::ManifestPath() const
{
    return mDirectory + "/manifest.json";
}

void FileManifest::Load()
{
    mEntries.clear();
    QFile f(ManifestPath());
    if (!f.open(QIODevice::ReadOnly))
        return;

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isArray())
    {
        for (const QJsonValue &val : doc.array())
        {
            QJsonObject obj = val.toObject();
            FileEntry e;
            e.fileId = obj.value("fileId").toInt();
            e.url = obj.value("url").toString();
            e.destPath = obj.value("destPath").toString();
            e.localPath = obj.value("localPath").toString();
            e.size = obj.value("size").toInteger(0);
            e.state = stateFromString(obj.value("state").toString());
            e.uploadRetryCount = obj.value("uploadRetryCount").toInt(0);
            e.timestamp = QDateTime::fromString(obj.value("timestamp").toString(), Qt::ISODate);
            if (e.fileId > 0)
            {
                mEntries.insert(e.fileId, e);
            }
        }
    }
}

void FileManifest::Save() const
{
    if (mDirectory.isEmpty())
    {
        qDebug() << "ERROR: Input directory is empty!";
        return;
    }
    QDir().mkpath(mDirectory);

    QJsonArray arr;
    for (QMap<int, FileEntry>::const_iterator it = mEntries.constBegin(); it != mEntries.constEnd(); ++it)
    {
        const FileEntry &e = it.value();
        QJsonObject obj;
        obj["fileId"] = e.fileId;
        obj["url"] = e.url;
        obj["destPath"] = e.destPath;
        obj["localPath"] = e.localPath;
        obj["size"] = e.size;
        obj["state"] = stateToString(e.state);
        obj["uploadRetryCount"] = e.uploadRetryCount;
        obj["timestamp"] = e.timestamp.toString(Qt::ISODate);
        arr.append(obj);
    }

    QString path = ManifestPath();
    QString tmpPath = path + ".tmp";
    QFile file(tmpPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        qDebug() << "ERROR: " << file.errorString();
        return;
    }

    file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    file.close();

    QFile::remove(path);
    QFile::rename(tmpPath, path);
}

QMap<int, FileEntry> FileManifest::Entries() const
{
    return mEntries;
}

bool FileManifest::Contains(int aFileId) const
{
    return mEntries.contains(aFileId);
}

FileEntry FileManifest::Entry(int aFileId) const
{
    return mEntries.value(aFileId);
}

void FileManifest::SetEntry(int aFileId, const FileEntry &aEntry)
{
    mEntries.insert(aFileId, aEntry);
    Save();
}

void FileManifest::SetEntry(int aFileId, FileState aState)
{
    if (mEntries.contains(aFileId))
    {
        mEntries[aFileId].state = aState;
        mEntries[aFileId].timestamp = QDateTime::currentDateTime();
        Save();
    }
}

void FileManifest::RemoveEntry(int aFileId)
{
    mEntries.remove(aFileId);
    Save();
}
