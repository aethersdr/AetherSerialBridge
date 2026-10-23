#pragma once

// bridge.log in the data folder, created with its trusted owner (WS-6 for WS-7; INV-75, INV-96; the WS-4
// hand-off "secure creation of the Windows log file through LogFileOps"). The method set is WS-4's
// LogFileOps (size / rollover / append); WS-7 wraps this in a three-line LogFileOps adapter when it builds the
// service's FileLogSink (WS-4 is not in this branch).
//
//  append    bridge.log is opened RELATIVE to the trusted folder handle with FILE_APPEND_DATA, FILE_OPEN_IF,
//            FILE_OPEN_REPARSE_POINT, and — if it is created — owner SYSTEM (Runtime policy) in the create call;
//            an existing entry that is a link or not a regular file is refused. Share read + delete, so a viewer
//            can read it and the rollover can rename it.
//  rollover  bridge.log renamed by its handle over bridge.log.1 (replace; POSIX semantics).
//  size      -1 when the log does not exist.

#include "core/FileIo.h"
#include "platform/win/NtSeams.h"
#include "platform/win/WinFileIo.h"

namespace asb::win {

class WinLogFile {
public:
    // `dir` must be the trusted data folder's handle (WindowsDataFolder::handle() after a Service inspection).
    WinLogFile(NtSeams& seams, const DirHandle& dir, QString name = QStringLiteral("bridge.log"),
               QByteArray createSd = CreateSecurity::dataFolder().runtime);

    qint64 size();
    bool rollover(QString* err);
    bool append(const QByteArray& bytes, QString* err);

private:
    NtSeams& m_s;
    const DirHandle& m_dir;
    QString m_name;
    QByteArray m_sd;
};

}  // namespace asb::win
