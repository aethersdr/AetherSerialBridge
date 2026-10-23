#pragma once

// Under AddressSanitizer / UBSan (the CI sanitizers leg), a sanitizer error ends the process from
// inside the sanitizer runtime, which QtTest never sees: the log then shows the sanitizer report but no
// "FAIL!" line naming the test that was running. Including this header in a test executable registers
// a sanitizer death callback that prints that line first (on stdout, QtTest's text format), so the run
// log attributes the error to the test function that was running. Without a
// sanitizer it does nothing.

#include <QTest>

#include <cstdio>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
#define ASB_HAVE_SANITIZER_DEATH_CALLBACK 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define ASB_HAVE_SANITIZER_DEATH_CALLBACK 1
#endif
#endif

#if defined(ASB_HAVE_SANITIZER_DEATH_CALLBACK)
#include <sanitizer/common_interface_defs.h>

namespace asb::test::detail {

inline void sanitizerDeathFailLine()
{
    // QtTest names the class; every test executable here is named after its test class.
    // currentAppName() is argv[0]: keep its file name only.
    const char* app = QTest::currentAppName();
    if (app) {
        for (const char* p = app; *p; ++p)
            if (*p == '/' || *p == '\\') app = p + 1;
    }
    const char* fn = QTest::currentTestFunction();
    std::fprintf(stdout, "FAIL!  : %s::%s() a sanitizer reported an error (report on stderr)\n",
                 app ? app : "?", fn ? fn : "?");
    std::fflush(stdout);
}

inline const bool sanitizerDeathCallbackRegistered = [] {
    __sanitizer_set_death_callback(&sanitizerDeathFailLine);
    return true;
}();

}  // namespace asb::test::detail
#endif
