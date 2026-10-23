#pragma once

// What --list-ports prints on Linux (INV-32). It reads
// directory entries and symbolic links only (opendir/readdir, readlink, lstat) and NEVER opens a port:
// opening one can toggle DTR/RTS on the device (HW 5). The directories are injectable for tests.

#include <QList>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <optional>

namespace asb::posix::SerialPortList {

inline constexpr char kByIdDirectory[] = "/dev/serial/by-id";
inline constexpr char kDevDirectory[] = "/dev";

struct Entry {
    QString path;                    // /dev/serial/by-id/<name>
    std::optional<QString> target;   // the final link target (named even when it does not exist); nullopt when
                                     // the entry is not a link or the chain loops (printed "(dangling)")
    bool operator==(const Entry&) const = default;
};

// The stable names udev makes, sorted ordinally, each with its final link target.
QList<Entry> byId(const QString& dir = QString::fromLatin1(kByIdDirectory));
// USB serial adapters by kernel name (ttyUSB*, ttyACM*): ttyACM before ttyUSB (ordinal), then by number.
QStringList kernelNames(const QString& devDir = QString::fromLatin1(kDevDirectory));
// The listing, line for line.
void print(QTextStream& out, const QString& byIdDir = QString::fromLatin1(kByIdDirectory),
           const QString& devDir = QString::fromLatin1(kDevDirectory));

}  // namespace asb::posix::SerialPortList
