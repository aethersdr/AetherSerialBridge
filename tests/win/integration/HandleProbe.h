#pragma once

// v1.1 WS-17 (INV-128), test only: the file handles another process holds, by path. tst_GuiNative probes the real
// window between its operations and fails on any handle under the program folder (%ProgramFiles%\AetherSerialBridge)
// or the data folder (%ProgramData%\AetherSerialBridge).
//
// NtQuerySystemInformation(SystemExtendedHandleInformation) lists every handle on the machine; the target's are
// duplicated into this process (PROCESS_DUP_HANDLE), kept when their object type is "File" and GetFileType says disk
// (pipes and devices are skipped: a name query on a synchronous pipe can block), and named on a helper thread with a
// per-handle timeout (GetFinalPathNameByHandleW, DOS form; the NT form through NtQueryObject when that fails). A name
// query that does not return in time fails the probe with that reason; the helper thread is left behind.

#include <QList>
#include <QString>
#include <QStringList>

namespace asb::test::handles {

struct Held {
    quint64 handle = 0;     // the value in the target process
    quint32 access = 0;     // its granted access
    QString path;           // "C:\..." (DOS) or "\Device\HarddiskVolumeN\..." (NT) when the DOS form was not available
};

struct Probe {
    bool ok = false;        // the listing and every name query completed
    QString error;          // why not
    QList<Held> files;      // every disk-file handle of the process, named
    QList<Held> under;      // those under one of the roots (a root itself included)
    QString describe() const;   // one line per handle in `under` (or "none")
    QString describeAll() const;   // one line per handle in `files`
};

// The disk-file handles of `pid`; `roots` are DOS paths (either separator), compared case-insensitively in DOS form
// and in their NT device form.
Probe probe(quint32 pid, const QStringList& roots, int perHandleMs = 3000);

}  // namespace asb::test::handles
