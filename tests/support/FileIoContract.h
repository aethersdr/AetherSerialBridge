#pragma once

// The FileIo contract (INV-15) as reusable checks, so every implementation runs the same tests:
// the in-memory fake here (WS-3), WinFileIo on the admin runner (WS-6) and PosixFileIo (WS-8). The
// native runs are the deferred gates of WS-3's INV-15 file-I/O mutant ("drop O_EXCL / CREATE_NEW").
//
// Each check records QTest failures in the calling test function; call it, then
// `if (QTest::currentTestFailed()) return;` if later steps depend on it.

#include "core/FileIo.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace asb::test {

class FileIoHarness {
public:
    virtual ~FileIoHarness() = default;
    virtual FileIo& io() = 0;
    virtual const DirHandle& dir() = 0;
    // Plant entries in the directory behind dir(), as an attacker would, before the code under test runs.
    virtual void plantFile(const QString& name, const QByteArray& bytes) = 0;
    virtual void plantSymlink(const QString& name, const QString& targetName) = 0;   // to another entry of dir()
    virtual void plantDirectory(const QString& name) = 0;
    // A regular file's bytes; nullopt when missing or not a regular file (links are not followed).
    virtual std::optional<QByteArray> read(const QString& name) = 0;
    virtual QStringList names() = 0;
};

namespace FileIoContract {

// A file planted at exactly the generated temp name is neither reused, followed, overwritten nor
// removed, and the write fails without touching the target.
void plantedGeneratedNameNotReused(FileIoHarness&);
// The same for a symlink planted at the generated name: the link's target is untouched.
void plantedSymlinkNotFollowed(FileIoHarness&);
// When the rename fails (a directory sits at the target) the temp is removed and nothing else remains.
void tempRemovedOnFailure(FileIoHarness&);
// Two saves leave exactly the target, holding the second content.
void repeatedSavesLeaveOnlyTheTarget(FileIoHarness&);
// readBack returns the written bytes through the same handle and refuses more than `cap`.
void readBackThroughTheSameHandle(FileIoHarness&);
// openExisting: a missing name reports fileNotFoundError(); a symlink is refused, not followed.
void openExistingMissingAndLinks(FileIoHarness&);

}  // namespace FileIoContract
}  // namespace asb::test
