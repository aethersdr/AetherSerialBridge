// INV-31 control object. It DELIBERATELY references writers the check lists: a ThemeManager
// mutator (setColor), C stdio opens (fopen, freopen), std::filesystem mutators (remove, rename)
// and, on Windows, _wfopen and CreateFileA. The writer-symbol check that passes the theme
// follower's objects must reject it, and must see every one of them (the EXPECT list in
// tests/CMakeLists.txt), so each of these list entries is proven to match this platform's symbol
// format. It also makes the two references the check requires, so it is rejected for the writers
// and nothing else. Compiled only, never linked or shipped.

#ifndef _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_WARNINGS  // fopen/_wfopen are the point here
#endif

#include <cstdio>
#include <filesystem>

#include <QColor>
#include <QString>

#include <sqlite3.h>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "core/ThemeManager.h"

void asbThemeWriterControlBad() {
    auto& tm = AetherSDR::ThemeManager::instance();
    tm.activateValidatedTheme(QStringLiteral("Default Dark"), QByteArray("{}"));
    tm.setColor(QStringLiteral("color.accent"), QColor(1, 2, 3));
    sqlite3* db = nullptr;
    sqlite3_open_v2("file:x?mode=ro", &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);

    std::FILE* f = std::fopen("x.json", "w");
    f = std::freopen("y.json", "w", f);
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path("x.json"), ec);
    std::filesystem::rename(std::filesystem::path("x.json"), std::filesystem::path("y.json"), ec);
#ifdef _WIN32
    std::FILE* w = _wfopen(L"x.json", L"w");
    (void)w;
    HANDLE h = CreateFileA("x.json", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    (void)h;
#endif
    (void)f;
}
