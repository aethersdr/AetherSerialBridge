#pragma once

// The Windows DLL and plugin policy of both executables (WS-7; master §4.11, D3, B10; INV-67).
//
//  - Linker /DEPENDENTLOADFLAG:0x800 (cmake asb_apply_dll_policy): the loader resolves the exe's static imports
//    of non-KnownDLLs from System32 only (Windows 10 1607+). Applied when Qt is linked statically (the shipped
//    form, D3); a dynamic-Qt development build imports Qt6*.dll from the build tree and could not start with it.
//  - applyProcessPolicy(), the FIRST statement of main: SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32),
//    so a later LoadLibrary with a bare name searches System32 only; and, where no plugin is needed (the headless
//    exe, and every static build), QT_PLUGIN_PATH / QT_QPA_PLATFORM_PLUGIN_PATH are removed from the environment.
//  - applyQtLibraryPolicy(), right after the Q*Application exists: QCoreApplication::setLibraryPaths({}) (the
//    same condition), so no plugin directory — and nothing qt.conf names — is searched.
// Stated limit: explicit loads made by static initialisers before main are not covered by
// SetDefaultDllDirectories; the runtime module audit (INV-88, WS-13) is the gate that would catch one.
//
// On other platforms both calls do nothing.

namespace asb::win {

enum class PluginPolicy { NoPlugins, KeepPlugins };

// Whether this build may clear the plugin paths: always for the headless exe (it loads no plugin); for the GUI
// only when Qt is static (its platform plugin is linked in).
PluginPolicy pluginPolicyFor(bool guiExecutable);

// false when SetDefaultDllDirectories failed (the caller ends the process: a policy that could not be applied is
// not silently skipped).
bool applyProcessPolicy(PluginPolicy);
void applyQtLibraryPolicy(PluginPolicy);

}  // namespace asb::win
