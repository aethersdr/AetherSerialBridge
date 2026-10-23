#pragma once

// The build stamp (v1.1 D61): a version record every v1.1+ executable carries in a section of its own, so a reader
// can tell which release a program file is without running it. ELF: section ".asb_stamp"; PE: section ".asbstmp"
// (eight characters, the PE limit). The record is kBuildStampSize bytes: the 16-byte magic "ASB-BUILD-STAMP\0", then
// compact JSON {"schema":1,"product":"aether-serial-bridge","version":"<version>"}, zero padded. It is defined
// beside core::version() (BuildInfo.cpp), which every executable links.
//
// The stamp is for WORDING only ("an earlier build (before 1.1)", "version 1.1.0", "a newer build (1.2.0)"). The
// program's identity is its SHA-256 (D61); the stamp is never an input to the installation classifier (INV-120).
//
// The reader parses the file's own section table (the ELF section headers, the PE section table): no scan of the
// file's content, every read bounded by the file's size, and an offset or size that points outside the file is
// refused. A file without the section (every v1.0 program) has no stamp.

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <functional>
#include <optional>

namespace asb::core {

inline constexpr std::size_t kBuildStampSize = 256;
inline constexpr char kBuildStampMagic[] = "ASB-BUILD-STAMP";   // 15 characters + the terminating zero = 16 bytes
inline constexpr char kBuildStampElfSection[] = ".asb_stamp";
inline constexpr char kBuildStampPeSection[] = ".asbstmp";
inline constexpr char kBuildStampProduct[] = "aether-serial-bridge";

struct BuildStamp {
    QString version;   // as the build was configured (CMake project version)
};

// Reads `length` bytes at `offset` of the file; returns fewer bytes (or none) when it cannot.
using ReadAt = std::function<QByteArray(qint64 offset, qint64 length)>;

// The stamp of a file of `size` bytes read through `readAt`; nullopt when the file is not an ELF or PE image, has
// no stamp section, or the section is damaged (too small, outside the file, wrong magic, not schema 1 JSON).
std::optional<BuildStamp> parseBuildStamp(qint64 size, const ReadAt& readAt);
// The same, from a file (opened read-only, bounded reads) or from bytes already read.
std::optional<BuildStamp> readBuildStamp(const QString& path);
std::optional<BuildStamp> buildStampOf(const QByteArray& image);

// This build's record (kBuildStampSize bytes), as linked into this executable.
QByteArray ownBuildStampRecord();

// How a question names the installed program's build, from its stamp: nullopt (not read) -> empty; an empty
// version (read, no stamp) -> "an earlier build, before 1.1"; newer than `thisVersion` -> "a newer build, 1.2.0";
// otherwise "version 1.1.0".
QString describeInstalledVersion(const std::optional<QString>& installedVersion, const QString& thisVersion);

}  // namespace asb::core
