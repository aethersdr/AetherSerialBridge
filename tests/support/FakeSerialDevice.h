#pragma once

// A scriptable ISerialDevice for the engine tests (WS-4), above QtSerialDevice. Every device a factory
// makes shares one FakeSerialWorld, which keeps the cross-device event log (INV-2: "close 1" before
// "open 2"), the open counters and the open failure script.

#include "engine/ISerialDevice.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

namespace asb::test {

class FakeSerialDevice;

struct FakeSerialWorld {
    QStringList events;                 // "open 1", "open-failed 1", "close 1", "write 1 3", ...
    int created = 0;                    // devices made by the factory
    int openCalls = 0;                  // every open() call, failed or not
    int opens = 0;                      // successful opens
    int failNextOpens = 0;              // that many next open() calls fail
    QString failMessage = QStringLiteral("Access to the port is denied.");
    QList<qint64> openCallMs;           // QElapsedTimer ms of every open() call
    QElapsedTimer clock;
    std::function<void(FakeSerialDevice&)> configure;   // applied to each new device
    QList<QPointer<FakeSerialDevice>> devices;
    QHash<int, QByteArray> wires;       // device id → its wire; outlives the device (deleted with its session)
    QHash<int, qint64> lostOnClose;     // device id → bytes its close() discarded (Qt buffer + native queue)

    FakeSerialWorld() { clock.start(); }
    FakeSerialDevice* last() const { return devices.isEmpty() ? nullptr : devices.last().data(); }
};

class FakeSerialDevice final : public engine::ISerialDevice {
    Q_OBJECT

public:
    // How bytes accepted by write() reach "the wire" (and bytesWritten is emitted).
    enum class Drain {
        Auto,          // all of it, from the event loop after write() returned (like QSerialPort)
        Manual,        // only when the test calls drain(n)
        InsideWrite,   // at once, emitting bytesWritten INSIDE write() (INV-80)
    };

    FakeSerialDevice(std::shared_ptr<FakeSerialWorld> world, int id);
    ~FakeSerialDevice() override;

    bool open(const asb::PortMapping&, QString* why) override;
    void close() override;
    qint64 read(char*, qint64) override;
    qint64 write(const char*, qint64) override;
    qint64 bytesAvailable() const override { return m_rx.size(); }
    qint64 bytesToWrite() const override { return m_tx.size(); }
    // The driver's queue (nativeStage), 0 without the stage; nullopt when closed or nativeUnknown.
    std::optional<qint64> nativeOutputPending() const override;
    void setReadBufferSize(qint64 s) override { readBufferSize = s; }
    QString openNotice() const override { return openNoticeText; }

    // The device produces bytes: appended to its input; readyRead is emitted (directly, as QSerialPort
    // does from its notifier) unless `silently`.
    void feed(const QByteArray& bytes, bool silently = false);
    // Moves n bytes (all if n < 0) of the unwritten buffer to the wire (with nativeStage: to the
    // driver's queue) and emits bytesWritten(n): the driver accepted them.
    void drain(qint64 n = -1);
    // nativeStage: moves n bytes (all if n < 0) of the driver's queue to the wire (transmitted).
    void transmit(qint64 n = -1);
    void emitError(engine::SerialError e, const QString& message) { emit errorOccurred(e, message); }

    const int id;
    bool isOpen = false;
    int failOpens = 0;             // this device's next open() calls that fail (besides the world's)
    int openCalls = 0;
    int closeCalls = 0;            // every close() call, also on a closed device
    QByteArray rxOnOpen;           // input already there when an open succeeds (no readyRead)
    QString openNoticeText;        // what openNotice() reports
    Drain drainMode = Drain::Auto;
    qint64 maxWritePerCall = -1;   // > 0: short writes
    bool writeFails = false;       // write() returns -1
    bool readFails = false;        // read() returns -1
    qint64 readBufferSize = 0;
    // A driver below the Qt buffer: bytes drain() hands over wait in a native queue until
    // transmit(); close() discards that queue, as adapters that cancel pending output do, and counts
    // the loss in FakeSerialWorld::lostOnClose.
    bool nativeStage = false;
    bool nativeUnknown = false;    // nativeOutputPending() cannot tell (nullopt)
    qint64 nativeMisreport = 0;    // > 0: nativeOutputPending() reports this many bytes forever (a lying driver)
    QByteArray wire;               // everything transmitted, in order
    int writeCalls = 0;
    // Drain::InsideWrite: called right after bytesWritten was emitted, still inside write().
    std::function<void()> afterEmitInsideWrite;
    bool inWrite = false;

private:
    std::shared_ptr<FakeSerialWorld> m_world;
    QByteArray m_rx, m_tx, m_native;
};

// A factory whose devices share `world`.
engine::SerialDeviceFactory fakeSerialFactory(const std::shared_ptr<FakeSerialWorld>& world);

}  // namespace asb::test
