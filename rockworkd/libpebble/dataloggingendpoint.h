#ifndef DATALOGGINGENDPOINT_H
#define DATALOGGINGENDPOINT_H

#include <QObject>
#include <QMap>
#include <QString>
#include <QUuid>
#include <QDateTime>

class Pebble;
class WatchConnection;

class DataLoggingEndpoint : public QObject
{
    Q_OBJECT
public:
    enum DataLoggingCommand {
        DataLoggingDespoolOpenSession = 0x01,
        DataLoggingDespoolSendData = 0x02,
        DataLoggingCloseSession = 0x03,
        DataLoggingReportOpenSessions = 0x84,
        DataLoggingACK = 0x85,
        DataLoggingNACK = 0x86,
        DataLoggingTimeout = 0x07,
        DataLoggingEmptySession = 0x88,
        DataLoggingGetSendEnableRequest = 0x89,
        DataLoggingGetSendEnableResponse = 0x0A,
        DataLoggingSetSendEnable = 0x8B
    };

    enum DataLoggingItemType {
        ByteArray = 0x00,
        UnsignedInt = 0x02,
        SignedInt = 0x03
    };

    explicit DataLoggingEndpoint(Pebble *pebble, WatchConnection *connection);

signals:

private slots:
    void handleMessage(const QByteArray &data);

private:
    struct DataLoggingSession {
        QUuid appUuid;
        QDateTime timestamp;
        quint32 logtag;
        DataLoggingItemType itemType;
        quint16 itemSize;
    };

    Pebble *m_pebble;
    WatchConnection *m_connection;
    QMap<quint8, DataLoggingSession> m_sessions;
    void sendACK(quint8 sessionId);
    void sendNACK(quint8 sessionId);
    void requestSessionList();
};

#endif // DATALOGGINGENDPOINT_H
