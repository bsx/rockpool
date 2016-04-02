#ifndef DATALOGGINGENDPOINT_H
#define DATALOGGINGENDPOINT_H

#include <QObject>

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
    Pebble *m_pebble;
    WatchConnection *m_connection;
    void sendACK(quint8 sessionId);
    void sendNACK(quint8 sessionId);
};

#endif // DATALOGGINGENDPOINT_H
