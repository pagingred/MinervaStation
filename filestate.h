#ifndef FILESTATE_H
#define FILESTATE_H

enum class FileState
{
    Downloading,
    Downloaded,
    Uploading,
    UploadRetrying,
    Uploaded,
    Duplicate
};

#endif // FILESTATE_H
