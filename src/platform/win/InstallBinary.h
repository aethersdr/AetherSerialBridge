#pragma once

// The install copy of the headless executable (WS-7; INV-41). Portable over FileIo: on Windows `io` is the lock
// directory's WinFileIo (EngineLock::io(), %ProgramFiles%\AetherSerialBridge opened by handle and judged), whose
// Config policy creates the file with owner Administrators and the protected SYSTEM + Administrators DACL.
//
//  1. an exclusive temp <rand>.asbtmp in the destination directory (CREATE_NEW, owner in the create call);
//  2. write the image, FlushFileBuffers;
//  3. read the copy back THROUGH THE TEMP'S OWN HANDLE and hash it: it must equal the expected SHA-256 (the GUI:
//     HeadlessDigest.h compiled in; the CLI self-install: the hash of its own running image) — else nothing is
//     published and the temp is deleted;
//  4. rename over the target with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH semantics, then the
//     directory flush (TempFile::commitAs).

#include "core/FileIo.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace asb::win::InstallBinary {

inline constexpr char kInstalledName[] = "aether-serial-bridge.exe";
inline constexpr qint64 kMaxImageBytes = 256LL * 1024 * 1024;

struct Result {
    bool ok = false;
    bool replaced = false;   // the target now holds the verified copy
    bool durable = false;    // ... and the directory flush after the rename succeeded
    QString error;
};

// `afterWrite` is a test seam (called after the flush, before the read-back; tst_Install corrupts the copy there).
// `beforeCommit` is called after the copy was verified and before the rename publishes it; it throws to stop a
// publication whose write or read-back returned after the caller's deadline (the temp is then deleted).
Result install(FileIo& io, const DirHandle& dir, const QString& name, const QByteArray& image,
               const QByteArray& expectedSha256Hex, const std::function<void()>& afterWrite = {},
               const std::function<void()>& beforeCommit = {});

// Lower-case hex SHA-256.
QByteArray sha256Hex(const QByteArray&);

}  // namespace asb::win::InstallBinary
