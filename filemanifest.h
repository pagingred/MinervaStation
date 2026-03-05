#ifndef FILEMANIFEST_H
#define FILEMANIFEST_H

#include <QMap>
#include <QString>

#include "filestate.h"
#include "fileentry.h"

class FileManifest {
public:
    explicit FileManifest(const QString &aDirectory = "");

    void SetDirectory(const QString &aDirectory);

    void Load();
    void Save() const;

    QMap<int, FileEntry> Entries() const;
    bool Contains(int aFileId) const;
    FileEntry Entry(int aFileId) const;

    void SetEntry(int aFileId, const FileEntry &aEntry);
    void SetEntry(int aFileId, FileState aState);
    void RemoveEntry(int aFileId);

private:
    QString ManifestPath() const;

    QString mDirectory;
    QMap<int, FileEntry> mEntries;
};

#endif // FILEMANIFEST_H
