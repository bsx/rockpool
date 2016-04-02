#include "dataloggingendpoint.h"

#include "pebble.h"
#include "watchconnection.h"
#include "watchdatareader.h"
#include "watchdatawriter.h"

#include <QUuid>
#include <QDateTime>

DataLoggingEndpoint::DataLoggingEndpoint(Pebble *pebble, WatchConnection *connection):
    QObject(pebble),
    m_pebble(pebble),
    m_connection(connection)
{
    m_connection->registerEndpointHandler(WatchConnection::EndpointDataLogging, this, "handleMessage");
}

void DataLoggingEndpoint::handleMessage(const QByteArray &data)
{
    qDebug() << "data logged" << data.toHex();
    WatchDataReader reader(data);
    DataLoggingCommand command = (DataLoggingCommand)reader.read<quint8>();
    switch (command) {
    case DataLoggingDespoolOpenSession: {
        quint8 sessionId = reader.read<quint8>();
        QUuid appUuid = reader.readUuid();
        QDateTime timestamp = reader.readTimestamp();
        quint32 logtag = reader.readLE<quint32>();
        DataLoggingItemType item_type = (DataLoggingItemType)reader.read<quint8>();
        quint16 item_size = reader.readLE<quint16>();
        qDebug() << "Opening datalogging session:" << sessionId << "App:" << appUuid << "Timestamp:" << timestamp
                 << "Logtag:" << logtag << "Item Type:" << item_type << "Item Size:" << item_size;

        sendACK(sessionId);
        return;
    }
    case DataLoggingDespoolSendData: {
        quint8 sessionId = reader.read<quint8>();
        quint32 itemsLeft = reader.readLE<quint32>();
        quint32 crc = reader.readLE<quint32>();
        qDebug() << "Despooling data: Session:" << sessionId << "Items left:" << itemsLeft << "CRC:" << crc;

        sendACK(sessionId);
        return;
    }
    case DataLoggingTimeout: {
        quint8 sessionId = reader.read<quint8>();
        qDebug() << "DataLogging reached timeout: Session:" << sessionId;
        return;
    }
    default:
        qDebug() << "Unhandled DataLogging message";
    }
}


void DataLoggingEndpoint::sendACK(quint8 sessionId)
{
    QByteArray reply;
    WatchDataWriter writer(&reply);
    writer.write<quint8>(DataLoggingACK);
    writer.write<quint8>(sessionId);
    m_connection->writeToPebble(WatchConnection::EndpointDataLogging, reply);
}

void DataLoggingEndpoint::sendNACK(quint8 sessionId)
{
    QByteArray reply;
    WatchDataWriter writer(&reply);
    writer.write<quint8>(DataLoggingNACK);
    writer.write<quint8>(sessionId);
    m_connection->writeToPebble(WatchConnection::EndpointDataLogging, reply);
}
