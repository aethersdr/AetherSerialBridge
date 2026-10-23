#include "engine/Session.h"

#include <QHostAddress>

namespace asb::engine {

QString enumName(EndReason r)
{
    switch (r) {
    case EndReason::Replaced: return QStringLiteral("replaced");
    case EndReason::Shutdown: return QStringLiteral("shutdown");
    case EndReason::ClientGone: return QStringLiteral("client gone");
    case EndReason::OpenRefused: return QStringLiteral("open refused");
    case EndReason::ReadFailed: return QStringLiteral("read failed");
    case EndReason::WriteFailed: return QStringLiteral("write failed");
    case EndReason::DeviceError: return QStringLiteral("device error");
    case EndReason::Stalled: return QStringLiteral("stalled");
    }
    return QStringLiteral("?");
}

QString endpointText(const QTcpSocket& s)
{
    QHostAddress a = s.peerAddress();
    bool isV4 = false;
    const quint32 v4 = a.toIPv4Address(&isV4);   // an IPv4-mapped peer prints as IPv4
    if (isV4) a = QHostAddress(v4);
    if (a.isNull()) return QStringLiteral("?");
    const QString host = a.protocol() == QAbstractSocket::IPv6Protocol ? QStringLiteral("[%1]").arg(a.toString())
                                                                        : a.toString();
    return QStringLiteral("%1:%2").arg(host).arg(s.peerPort());
}

Session::Session(QTcpSocket* socket, std::unique_ptr<ISerialDevice> device, QObject* parent)
    : QObject(parent),
      endpoint(endpointText(*socket)),
      m_socket(socket),
      m_device(std::move(device)),
      m_socketEnd(socket),
      m_deviceEnd(m_device.get())
{
    m_socket->setParent(this);
}

Session::~Session() = default;

}  // namespace asb::engine
