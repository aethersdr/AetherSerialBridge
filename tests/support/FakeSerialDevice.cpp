#include "support/FakeSerialDevice.h"

#include <QTimer>

#include <algorithm>

namespace asb::test {

FakeSerialDevice::FakeSerialDevice(std::shared_ptr<FakeSerialWorld> world, int id_) : id(id_), m_world(std::move(world)) {}

FakeSerialDevice::~FakeSerialDevice() = default;

bool FakeSerialDevice::open(const asb::PortMapping&, QString* why)
{
    ++m_world->openCalls;
    ++openCalls;
    m_world->openCallMs.append(m_world->clock.elapsed());
    if (failOpens > 0 || m_world->failNextOpens > 0) {
        if (failOpens > 0) {
            --failOpens;
        } else {
            --m_world->failNextOpens;
        }
        m_world->events.append(QStringLiteral("open-failed %1").arg(id));
        if (why) *why = m_world->failMessage;
        return false;
    }
    ++m_world->opens;
    isOpen = true;
    m_rx.append(rxOnOpen);
    m_world->events.append(QStringLiteral("open %1").arg(id));
    return true;
}

void FakeSerialDevice::close()
{
    ++closeCalls;
    if (!isOpen) return;
    isOpen = false;
    m_world->lostOnClose[id] += m_tx.size() + m_native.size();
    m_tx.clear();
    m_native.clear();
    m_world->events.append(QStringLiteral("close %1").arg(id));
}

qint64 FakeSerialDevice::read(char* data, qint64 max)
{
    if (!isOpen || readFails) return -1;
    const qint64 n = std::min<qint64>(max, m_rx.size());
    std::copy_n(m_rx.constData(), n, data);
    m_rx.remove(0, n);
    return n;
}

qint64 FakeSerialDevice::write(const char* data, qint64 len)
{
    ++writeCalls;
    if (!isOpen || writeFails) return -1;
    const qint64 k = maxWritePerCall > 0 ? std::min(len, maxWritePerCall) : len;
    m_world->events.append(QStringLiteral("write %1 %2").arg(id).arg(k));
    m_tx.append(data, k);
    switch (drainMode) {
    case Drain::Auto:
        QTimer::singleShot(0, this, [this] { drain(); });
        break;
    case Drain::Manual:
        break;
    case Drain::InsideWrite:
        inWrite = true;
        drain();
        if (afterEmitInsideWrite) afterEmitInsideWrite();
        inWrite = false;
        break;
    }
    return k;
}

void FakeSerialDevice::feed(const QByteArray& bytes, bool silently)
{
    m_rx.append(bytes);
    if (!silently) emit readyRead();
}

void FakeSerialDevice::drain(qint64 n)
{
    if (n < 0 || n > m_tx.size()) n = m_tx.size();
    if (n == 0) return;
    if (nativeStage) {
        m_native.append(m_tx.left(n));
    } else {
        wire.append(m_tx.left(n));
        m_world->wires[id].append(m_tx.left(n));
    }
    m_tx.remove(0, n);
    emit bytesWritten(n);
}

void FakeSerialDevice::transmit(qint64 n)
{
    if (n < 0 || n > m_native.size()) n = m_native.size();
    if (n == 0) return;
    wire.append(m_native.left(n));
    m_world->wires[id].append(m_native.left(n));
    m_native.remove(0, n);
}

std::optional<qint64> FakeSerialDevice::nativeOutputPending() const
{
    if (!isOpen || nativeUnknown) return std::nullopt;
    if (nativeMisreport > 0) return nativeMisreport;
    return m_native.size();
}

engine::SerialDeviceFactory fakeSerialFactory(const std::shared_ptr<FakeSerialWorld>& world)
{
    return [world]() -> std::unique_ptr<engine::ISerialDevice> {
        auto d = std::make_unique<FakeSerialDevice>(world, ++world->created);
        if (world->configure) world->configure(*d);
        world->devices.append(d.get());
        return d;
    };
}

}  // namespace asb::test
