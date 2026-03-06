#ifndef HYPERSCRAPEPROTOCOL_H
#define HYPERSCRAPEPROTOCOL_H

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QMap>
#include <QString>
#include <QUrl>
#include <QVector>

class HyperscrapeProtocol
{
public:
    enum class MsgType : quint8
    {
        Register         = 0x00,
        UploadSubchunk   = 0x01,
        GetChunks        = 0x02,
        DetachChunk      = 0x03,
        RegisterResponse = 0x80,
        ChunkResponse    = 0x81,
        Error            = 0x82,
        Ok               = 0x83,
    };

    struct ChunkAssignment
    {
        QString chunkId;
        QString fileId;
        QString url;
        quint64 rangeStart = 0;
        quint64 rangeEnd = 0;
    };

    struct KvResponse
    {
        QMap<QString, QString> fields;

        QString value(const QString &aKey, const QString &aFallback = {}) const
        {
            return fields.value(aKey, aFallback);
        }
        bool hasError() const { return fields.contains(QStringLiteral("error")); }
        QString error() const { return fields.value(QStringLiteral("error")); }
        QString chunkId() const { return fields.value(QStringLiteral("chunk_id")); }
    };

    static constexpr int SubchunkSize = 996147;
    static constexpr int Version = 4;

    static QString DefaultUrl()
    {
        return QStringLiteral("wss://firehose.minerva-archive.org/worker");
    }

    static QByteArray BuildRegister(const QString &aToken, int aMaxConcurrent)
    {
        QByteArray msg;
        QDataStream ds(&msg, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds << static_cast<quint8>(MsgType::Register);
        ds << static_cast<quint32>(Version);
        ds << static_cast<quint32>(aMaxConcurrent);
        WriteString(ds, aToken);
        return msg;
    }

    static QByteArray BuildGetChunks(int aCount)
    {
        QByteArray msg;
        QDataStream ds(&msg, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds << static_cast<quint8>(MsgType::GetChunks);
        ds << static_cast<quint32>(aCount);
        return msg;
    }

    static QByteArray BuildUploadSubchunk(const QString &aChunkId,
                                          const QString &aFileId,
                                          const QByteArray &aData)
    {
        QByteArray msg;
        QDataStream ds(&msg, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds << static_cast<quint8>(MsgType::UploadSubchunk);
        WriteString(ds, aChunkId);
        WriteString(ds, aFileId);
        ds << static_cast<quint32>(aData.size());
        ds.writeRawData(aData.constData(), aData.size());
        return msg;
    }

    static QByteArray BuildDetachChunk(const QString &aChunkId)
    {
        QByteArray msg;
        QDataStream ds(&msg, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds << static_cast<quint8>(MsgType::DetachChunk);
        WriteString(ds, aChunkId);
        return msg;
    }

    static QVector<ChunkAssignment> ParseChunkResponse(const QByteArray &aMsg)
    {
        QVector<ChunkAssignment> result;
        QDataStream ds(aMsg);
        ds.setByteOrder(QDataStream::LittleEndian);
        quint8 type = 0;
        ds >> type;
        quint32 count = 0;
        ds >> count;
        for (quint32 i = 0; i < count && ds.status() == QDataStream::Ok; ++i)
        {
            ChunkAssignment ca;
            ca.chunkId = ReadString(ds);
            ca.fileId = ReadString(ds);
            ca.url = ReadString(ds);
            ds >> ca.rangeStart;
            ds >> ca.rangeEnd;
            result.append(ca);
        }
        return result;
    }

    static KvResponse ParseKvResponse(const QByteArray &aMsg)
    {
        KvResponse r;
        QDataStream ds(aMsg);
        ds.setByteOrder(QDataStream::LittleEndian);
        quint8 type = 0;
        ds >> type;
        quint32 count = 0;
        ds >> count;
        for (quint32 i = 0; i < count && ds.status() == QDataStream::Ok; ++i)
        {
            QString key = ReadString(ds);
            QString val = ReadString(ds);
            r.fields[key] = val;
        }
        return r;
    }

    static QString ApiBaseFromWsUrl(const QString &aWsUrl)
    {
        QUrl url(aWsUrl);
        url.setScheme(url.scheme() == QStringLiteral("wss")
                      ? QStringLiteral("https")
                      : QStringLiteral("http"));
        url.setPath({});
        QString result = url.toString();
        while (result.endsWith(QLatin1Char('/')))
        {
            result.chop(1);
        }
        return result;
    }

private:
    static void WriteString(QDataStream &aDs, const QString &aStr)
    {
        QByteArray utf8 = aStr.toUtf8();
        aDs << static_cast<quint32>(utf8.size());
        aDs.writeRawData(utf8.constData(), utf8.size());
    }

    static QString ReadString(QDataStream &aDs)
    {
        quint32 len = 0;
        aDs >> len;
        if (len == 0 || aDs.status() != QDataStream::Ok)
        {
            return {};
        }
        QByteArray buf(static_cast<int>(len), Qt::Uninitialized);
        aDs.readRawData(buf.data(), static_cast<int>(len));
        return QString::fromUtf8(buf);
    }
};

#endif // HYPERSCRAPEPROTOCOL_H
