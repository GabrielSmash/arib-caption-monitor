#include "UdpReceiver.h"

#include <QNetworkDatagram>
#include <QVariant>

UdpReceiver::UdpReceiver(QObject* parent) : QObject(parent) {
    connect(&socket_, &QUdpSocket::readyRead, this, &UdpReceiver::readPending);
}

bool UdpReceiver::start(quint16 port, const QString& multicastGroup, QString* error) {
    stop();

    QHostAddress group;
    if (!multicastGroup.trimmed().isEmpty()) {
        group = QHostAddress(multicastGroup.trimmed());
        if (group.isNull() || !group.isMulticast()) {
            *error = QString("Endereço multicast inválido: %1").arg(multicastGroup);
            return false;
        }
    }

    // ShareAddress lets other tools (e.g. TSDuck) listen on the same port while the monitor runs.
    if (!socket_.bind(QHostAddress::AnyIPv4, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        *error = QString("Não foi possível abrir a porta UDP %1: %2").arg(port).arg(socket_.errorString());
        return false;
    }
    socket_.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, QVariant(4 * 1024 * 1024));

    if (!group.isNull() && !socket_.joinMulticastGroup(group)) {
        *error = QString("Falha ao entrar no grupo %1: %2").arg(group.toString(), socket_.errorString());
        socket_.close();
        return false;
    }

    port_ = port;
    group_ = group;
    datagrams_ = bytes_ = rtpDatagrams_ = 0;
    lastSender_.clear();
    lastDatagram_.invalidate();
    return true;
}

void UdpReceiver::stop() {
    if (!group_.isNull() && isActive())
        socket_.leaveMulticastGroup(group_);
    socket_.close();
    group_.clear();
}

void UdpReceiver::readPending() {
    while (socket_.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket_.receiveDatagram();
        QByteArray data = datagram.data();
        if (data.isEmpty())
            continue;

        ++datagrams_;
        bytes_ += data.size();
        lastDatagram_.start();
        lastSender_ = QString("%1:%2").arg(datagram.senderAddress().toString()).arg(datagram.senderPort());

        // RTP (version 2) wrapping: skip fixed header, CSRCs and extension.
        const auto* b = reinterpret_cast<const uint8_t*>(data.constData());
        if (b[0] != 0x47 && data.size() >= 12 && (b[0] >> 6) == 2) {
            int offset = 12 + 4 * (b[0] & 0x0F);
            if ((b[0] & 0x10) && offset + 4 <= data.size())
                offset += 4 + 4 * ((b[offset + 2] << 8) | b[offset + 3]);
            if (offset < data.size()) {
                ++rtpDatagrams_;
                data.remove(0, offset);
            }
        }

        emit tsData(data);
    }
}
