#include "core/BuildInfo.h"

#include "core/BuildStamp.h"

// The build stamp record (v1.1 D61, core/BuildStamp.h). It lives here, beside version(), because every executable
// links this object (each one prints its version); an archive member nothing references would never be linked. Kept
// against section garbage collection (ELF: used + retain; PE: /INCLUDE of the symbol), and checked on the shipped
// files by `release_audit.py stamp`, so a dropped section fails the release, not a user.
#define ASB_STAMP_JSON "{\"schema\":1,\"product\":\"aether-serial-bridge\",\"version\":\"" ASB_VERSION "\"}"
static_assert(sizeof(asb::core::kBuildStampMagic) == 16, "the magic is 16 bytes with its terminating zero");
static_assert(16 + sizeof(ASB_STAMP_JSON) <= asb::core::kBuildStampSize, "the stamp JSON does not fit the record");

#if defined(_MSC_VER)
#pragma section(".asbstmp", read)
extern "C" __declspec(allocate(".asbstmp")) const char asb_build_stamp[asb::core::kBuildStampSize] =
    "ASB-BUILD-STAMP\0" ASB_STAMP_JSON;
#if defined(_M_IX86)
#pragma comment(linker, "/INCLUDE:_asb_build_stamp")
#else
#pragma comment(linker, "/INCLUDE:asb_build_stamp")
#endif
#else
#if defined(__has_attribute)
#if __has_attribute(retain)
#define ASB_STAMP_RETAIN __attribute__((retain))
#endif
#endif
#ifndef ASB_STAMP_RETAIN
#define ASB_STAMP_RETAIN
#endif
extern "C" {
extern const char asb_build_stamp[asb::core::kBuildStampSize];
__attribute__((section(".asb_stamp"), used)) ASB_STAMP_RETAIN const char asb_build_stamp[asb::core::kBuildStampSize] =
    "ASB-BUILD-STAMP\0" ASB_STAMP_JSON;
}
#endif

namespace asb::core {

std::string_view version() { return ASB_VERSION; }

QByteArray ownBuildStampRecord()
{
    return QByteArray(asb_build_stamp, qsizetype(kBuildStampSize));
}

}  // namespace asb::core
