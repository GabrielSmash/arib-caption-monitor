#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QUdpSocket>

class UdpReceiver : public QObject {
    Q_OBJECT

public:
    explicit UdpReceiver(QObject* parent = nullptr);

    // multicastGroup may be empty for unicast.
    bool start(quint16 port, const QString& multicastGroup, QString* error);
    void stop();

    bool isActive() const { return socket_.state() == QAbstractSocket::BoundState; }
    quint16 port() const { return port_; }
    QString multicastGroup() const { return group_.isNull() ? QString() : group_.toString(); }
    quint64 datagrams() const { return datagrams_; }
    quint64 bytes() const { return bytes_; }
    quint64 rtpDatagrams() const { return rtpDatagrams_; }
    QString lastSender() const { return lastSender_; }
    // Milliseconds since the last datagram, or -1 if none was received yet.
    qint64 msSinceLastDatagram() const { return lastDatagram_.isValid() ? lastDatagram_.elapsed() : -1; }

signals:
    void tsData(const QByteArray& data);

private:
    void readPending();

    QUdpSocket socket_;
    QHostAddress group_;
    quint16 port_ = 0;
    quint64 datagrams_ = 0;
    quint64 bytes_ = 0;
    quint64 rtpDatagrams_ = 0;
    QString lastSender_;
    QElapsedTimer lastDatagram_;
};
