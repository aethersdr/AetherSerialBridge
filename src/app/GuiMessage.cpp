#include "app/GuiMessage.h"

#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QFile>
#include <QString>
#endif

namespace asb::app {

#ifdef _WIN32
namespace {

bool usable(HANDLE h)
{
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
}

}  // namespace
#endif

void showGuiMessage(const std::string& text, MessageKind kind)
{
#ifdef _WIN32
#ifdef ASB_TEST_SEAMS
    const QString seam = qEnvironmentVariable("ASB_TEST_MESSAGEBOX");
    if (seam.startsWith(QLatin1String("record:"))) {
        QFile f(seam.mid(7));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write((QStringLiteral("box %1\n").arg(kind == MessageKind::Error ? QStringLiteral("error") : QStringLiteral("info")) +
                     QString::fromLocal8Bit(text.c_str()))
                        .toUtf8());
        return;
    }
#endif
    const DWORD which = kind == MessageKind::Error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
    FILE* stream = kind == MessageKind::Error ? stderr : stdout;
    if (usable(GetStdHandle(which))) {
        std::fputs(text.c_str(), stream);
        std::fflush(stream);
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        if (freopen_s(&f, "CONOUT$", "w", stream) == 0 && f) {
            std::fputs("\n", f);   // the prompt was already printed: start on a line of our own
            std::fputs(text.c_str(), f);
            std::fflush(f);
            return;
        }
    }
    const std::wstring wide = QString::fromLocal8Bit(text.c_str()).trimmed().toStdWString();
    MessageBoxW(nullptr, wide.c_str(), L"Aether Serial Bridge",
                MB_OK | (kind == MessageKind::Error ? MB_ICONERROR : MB_ICONINFORMATION) | MB_SETFOREGROUND);
#else
    std::FILE* stream = kind == MessageKind::Error ? stderr : stdout;
    std::fputs(text.c_str(), stream);
    std::fflush(stream);
#endif
}

}  // namespace asb::app
