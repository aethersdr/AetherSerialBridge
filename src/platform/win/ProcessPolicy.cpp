#include "platform/win/ProcessPolicy.h"

#include <QCoreApplication>
#include <QStringList>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdlib.h>
#endif

namespace asb::win {

PluginPolicy pluginPolicyFor(bool guiExecutable)
{
#ifdef QT_STATIC
    (void)guiExecutable;
    return PluginPolicy::NoPlugins;
#else
    return guiExecutable ? PluginPolicy::KeepPlugins : PluginPolicy::NoPlugins;
#endif
}

bool applyProcessPolicy(PluginPolicy plugins)
{
#ifdef _WIN32
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) return false;
    if (plugins == PluginPolicy::NoPlugins) {
        for (const wchar_t* v : {L"QT_PLUGIN_PATH", L"QT_QPA_PLATFORM_PLUGIN_PATH"}) {
            SetEnvironmentVariableW(v, nullptr);   // the process environment
            _wputenv_s(v, L"");                    // and the CRT's copy, which qgetenv reads
        }
    }
    return true;
#else
    (void)plugins;
    return true;
#endif
}

void applyQtLibraryPolicy(PluginPolicy plugins)
{
#ifdef _WIN32
    if (plugins == PluginPolicy::NoPlugins) QCoreApplication::setLibraryPaths({});
#else
    (void)plugins;
#endif
}

}  // namespace asb::win
