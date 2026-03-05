#ifndef LBENTRY_H
#define LBENTRY_H

#include <QString>

struct LbEntry
{
    int rank = 0;
    QString username;
    qint64 bytes = 0;
    int files = 0;
    QString avatarUrl;
};

#endif // LBENTRY_H
