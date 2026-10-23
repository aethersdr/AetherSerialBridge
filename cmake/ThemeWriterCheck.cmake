# ThemeWriterCheck.cmake: INV-31, a tripwire on the theme follower's object files: they reference
# none of a list of known writer APIs.
#
# What this proves, and what it does not. It looks at what the compiled objects REFERENCE (undefined
# symbols), so a call to a listed writer cannot hide behind a branch the tests never take. It does
# NOT prove the follower cannot write: the calls the follower must make anyway can write when given
# other arguments (QFile::open(WriteOnly|Truncate), openat(O_CREAT|O_TRUNC), CreateFileW(GENERIC_WRITE),
# sqlite3_prepare_v2 with an UPDATE), and no list of names is complete. Read-only behaviour rests on
# the open flags, which the tests check on the real calls: tst_ThemeFollower::openFlagsReadOnly
# (SQLITE_OPEN_READONLY, via the open seam), ::themeFileOpenedReadOnly (a theme file the process may
# not write is still followed: the open asks for read access only) and ::mainDbAndWalBytesUnchanged.
# The listed writers:
#   - ThemeManager's mutators (setActiveTheme, setColor, setSizing, setGradient, setString,
#     setFontToken, removeOverride, save*/delete/rename/import/export), which persist to theme files;
#   - SQLite entry points that write or copy (sqlite3_exec, backup, checkpoint, blob, the non-v2
#     opens);
#   - common file-mutating C, Win32, C++ and Qt calls (write/rename/unlink/mkdir..., fopen/freopen,
#     WriteFile/MoveFileExW/CreateFileA..., std::filesystem::remove/rename/copy... (libstdc++/libc++
#     names and MSVC's __std_fs_* entry points), QIODevice::write, QFile::remove/rename/copy,
#     QDir::mkpath..., QSaveFile). Not listed: std::ofstream (a template whose names do not fit
#     this matcher), and anything not named here.
# AppSettings is not on the list: the choice lives in the bridge's own gui.ini (B5).
#
# Two references are REQUIRED (ThemeManager::activateValidatedTheme and sqlite3_open_v2): if the
# scanner stops seeing undefined symbols in these objects, the check fails instead of passing
# vacuously. A control object that calls listed writers must be rejected, and must be rejected for
# every writer named in its EXPECT list (tests/CMakeLists.txt), so each control entry is proven to
# be seen on this platform's symbol format.
#
#  Included as a module:   asb_add_theme_writer_check(<object-library> <test-name>
#                                                     [CONTROL EXPECT <writer>...])
#  Run as a script:        -DOBJECTS_FILE=<file with the object paths> -DLABEL=<name>
#                          -DNM=<nm> (ELF/Mach-O) or -DDUMPBIN=<dumpbin> (COFF)
#                          [-DEXPECT=<writer>,...]  (a control: each must be among the hits;
#                          "a|b" accepts either spelling, e.g. fopen|fopen64)
#  Prints "ThemeWriterCheck: VIOLATION: <label>: ..." and fails on a listed reference; a control
#  that misses an EXPECTed writer prints "ThemeWriterCheck: CONTROL MISSED: <writer>".
#  The object list is generated per configuration (<object-library>-<CONFIG>.objects), so
#  multi-config generators (Visual Studio, Ninja Multi-Config) get one list per configuration.

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    cmake_minimum_required(VERSION 3.22)
    if(NOT OBJECTS_FILE OR NOT EXISTS "${OBJECTS_FILE}")
        message(FATAL_ERROR "ThemeWriterCheck: OBJECTS_FILE missing: '${OBJECTS_FILE}'")
    endif()
    file(READ "${OBJECTS_FILE}" _objects)
    string(STRIP "${_objects}" _objects)
    if(NOT _objects)
        message(FATAL_ERROR "ThemeWriterCheck: no object files listed in ${OBJECTS_FILE}")
    endif()

    # Undefined symbols of every object, one per list entry.
    set(_symbols "")
    foreach(_obj IN LISTS _objects)
        if(NOT EXISTS "${_obj}")
            message(FATAL_ERROR "ThemeWriterCheck: object not found: ${_obj}")
        endif()
        if(DUMPBIN)
            execute_process(COMMAND "${DUMPBIN}" /NOLOGO /SYMBOLS "${_obj}"
                            OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR "ThemeWriterCheck: dumpbin failed on ${_obj} (${_rc}): ${_err}")
            endif()
            string(REPLACE ";" "," _out "${_out}")
            string(REPLACE "\n" ";" _lines "${_out}")
            foreach(_l IN LISTS _lines)
                if(_l MATCHES "UNDEF.*External[ \t]+\\|[ \t]+(.*)$")
                    list(APPEND _symbols "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        elseif(NM)
            execute_process(COMMAND "${NM}" -C -u "${_obj}"
                            OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR "ThemeWriterCheck: nm failed on ${_obj} (${_rc}): ${_err}")
            endif()
            string(REPLACE ";" "," _out "${_out}")
            string(REPLACE "\n" ";" _lines "${_out}")
            foreach(_l IN LISTS _lines)
                if(_l MATCHES "^[ \t]*U[ \t]+(.*)$")
                    list(APPEND _symbols "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        else()
            message(FATAL_ERROR "ThemeWriterCheck: neither NM nor DUMPBIN given")
        endif()
    endforeach()

    # C++ members: matched demangled (nm -C, and dumpbin's "(...)" text) or MSVC-decorated.
    set(_members
        "ThemeManager:setActiveTheme" "ThemeManager:setColor" "ThemeManager:setSizing"
        "ThemeManager:setGradient" "ThemeManager:setString" "ThemeManager:setFontToken"
        "ThemeManager:removeOverride" "ThemeManager:saveCurrentThemeAs" "ThemeManager:saveActiveTheme"
        "ThemeManager:deleteTheme" "ThemeManager:renameTheme" "ThemeManager:importThemeFromFile"
        "ThemeManager:exportThemeToFile" "ThemeManager:writeThemeFile"
        "QIODevice:write" "QFile:remove" "QFile:rename" "QFile:copy" "QFile:link" "QFile:resize"
        "QFile:setPermissions" "QFileDevice:resize" "QFileDevice:setPermissions"
        "QDir:mkpath" "QDir:mkdir" "QDir:rmdir" "QDir:rmpath" "QDir:remove" "QDir:rename"
        "QDir:removeRecursively" "QSaveFile:QSaveFile" "QSaveFile:commit"
        "filesystem:remove" "filesystem:remove_all" "filesystem:rename" "filesystem:copy"
        "filesystem:copy_file" "filesystem:create_directory" "filesystem:create_directories"
        "filesystem:resize_file" "filesystem:permissions")
    # C functions: the whole symbol name (an optional Windows import prefix or leading underscore).
    set(_functions
        sqlite3_exec sqlite3_backup_init sqlite3_backup_step sqlite3_wal_checkpoint
        sqlite3_wal_checkpoint_v2 sqlite3_blob_open sqlite3_open sqlite3_open16
        write pwrite pwrite64 writev pwritev ftruncate ftruncate64 truncate truncate64 unlink unlinkat
        rename renameat renameat2 mkdir mkdirat rmdir fwrite fputs fputc symlink symlinkat link linkat
        chmod fchmod fchmodat creat creat64 remove fopen fopen64 freopen freopen64 fdopen
        _write _wunlink _unlink _wrename _wmkdir _mkdir _wremove _wfopen _wfreopen _wfopen_s fopen_s
        CreateFileA WriteFile WriteFileEx DeleteFileW DeleteFileA MoveFileW MoveFileA MoveFileExW MoveFileExA
        CreateDirectoryW CreateDirectoryA RemoveDirectoryW SetEndOfFile SetFileInformationByHandle
        CopyFileW CopyFileExW ReplaceFileW CreateHardLinkW CreateSymbolicLinkW
        __std_fs_remove __std_fs_rename __std_fs_copy_file __std_fs_create_directory
        __std_fs_resize_file __std_fs_change_permissions)
    set(_required_members "ThemeManager:activateValidatedTheme")
    set(_required_functions sqlite3_open_v2)

    function(_twc_member_hit sym spec out)
        string(REPLACE ":" ";" _p "${spec}")
        list(GET _p 0 _cls)
        list(GET _p 1 _fn)
        if(sym MATCHES "(^|[^A-Za-z0-9_])${_cls}::${_fn}\\(" OR sym MATCHES "\\?${_fn}@${_cls}@")
            set(${out} TRUE PARENT_SCOPE)
        else()
            set(${out} FALSE PARENT_SCOPE)
        endif()
    endfunction()
    function(_twc_function_hit sym fn out)
        # nm prints the bare name; dumpbin "name" or "__imp_name", sometimes with " (...)" after.
        string(REGEX REPLACE "[ \t].*$" "" _name "${sym}")
        if(_name STREQUAL "${fn}" OR _name STREQUAL "__imp_${fn}" OR _name STREQUAL "_${fn}"
           OR _name STREQUAL "__imp__${fn}")
            set(${out} TRUE PARENT_SCOPE)
        else()
            set(${out} FALSE PARENT_SCOPE)
        endif()
    endfunction()

    set(_hits "")
    set(_seen_required "")
    foreach(_s IN LISTS _symbols)
        foreach(_m IN LISTS _members)
            _twc_member_hit("${_s}" "${_m}" _h)
            if(_h)
                list(APPEND _hits "${_m} (${_s})")
            endif()
        endforeach()
        foreach(_f IN LISTS _functions)
            _twc_function_hit("${_s}" "${_f}" _h)
            if(_h)
                list(APPEND _hits "${_f} (${_s})")
            endif()
        endforeach()
        foreach(_m IN LISTS _required_members)
            _twc_member_hit("${_s}" "${_m}" _h)
            if(_h)
                list(APPEND _seen_required "${_m}")
            endif()
        endforeach()
        foreach(_f IN LISTS _required_functions)
            _twc_function_hit("${_s}" "${_f}" _h)
            if(_h)
                list(APPEND _seen_required "${_f}")
            endif()
        endforeach()
    endforeach()

    list(LENGTH _symbols _nsym)
    foreach(_r IN LISTS _required_members _required_functions)
        if(NOT _r IN_LIST _seen_required)
            message(FATAL_ERROR "ThemeWriterCheck: ${LABEL}: the required reference ${_r} was not seen among "
                                "${_nsym} undefined symbols; the scan is not seeing these objects' references")
        endif()
    endforeach()
    # A control: every writer it was built to reference must be among the hits, so a list entry
    # that this platform's symbol format never matches cannot pass unnoticed.
    set(_missed "")
    string(REPLACE "," ";" _expect "${EXPECT}")
    foreach(_e IN LISTS _expect)
        set(_found FALSE)
        string(REPLACE "|" ";" _alts "${_e}")
        foreach(_alt IN LISTS _alts)
            foreach(_h IN LISTS _hits)
                string(FIND "${_h}" "${_alt} (" _at)
                if(_at EQUAL 0)
                    set(_found TRUE)
                endif()
            endforeach()
        endforeach()
        if(NOT _found)
            list(APPEND _missed "${_e}")
        endif()
    endforeach()
    foreach(_e IN LISTS _missed)
        message("ThemeWriterCheck: CONTROL MISSED: ${LABEL}: ${_e} was not detected")
    endforeach()
    if(_hits)
        list(REMOVE_DUPLICATES _hits)
        string(REPLACE ";" "\n  " _pretty "${_hits}")
        message("ThemeWriterCheck: VIOLATION: ${LABEL}: references a writer:\n  ${_pretty}")
        message(FATAL_ERROR "ThemeWriterCheck: ${LABEL} failed")
    endif()
    message(STATUS "ThemeWriterCheck: ${LABEL}: ${_nsym} undefined symbols, none of the listed writers referenced")
    return()
endif()

# ---------------------------------------------------------------------------------------------
# Module
# ---------------------------------------------------------------------------------------------
function(asb_add_theme_writer_check objlib test_name)
    cmake_parse_arguments(PARSE_ARGV 2 _a "CONTROL" "" "EXPECT")
    # One list per configuration: $<TARGET_OBJECTS> differs between configurations, and one file
    # cannot be generated with different contents (multi-config generators). ctest -C <config>
    # (or the single configuration) selects the matching list through $<CONFIG> in the command.
    set(_list "${CMAKE_BINARY_DIR}/theme-writer-check/${objlib}-$<CONFIG>.objects")
    file(GENERATE OUTPUT "${_list}" CONTENT "$<TARGET_OBJECTS:${objlib}>")
    if(MSVC)
        if(NOT ASB_DUMPBIN)
            message(FATAL_ERROR "ThemeWriterCheck: dumpbin not found (INV-31 writer-symbol test)")
        endif()
        set(_tool "-DDUMPBIN=${ASB_DUMPBIN}")
    else()
        if(NOT ASB_NM)
            message(FATAL_ERROR "ThemeWriterCheck: nm not found (INV-31 writer-symbol test)")
        endif()
        set(_tool "-DNM=${ASB_NM}")
    endif()
    set(_expect "")
    if(_a_CONTROL)
        if(NOT _a_EXPECT)
            message(FATAL_ERROR "ThemeWriterCheck: control ${objlib} needs an EXPECT list")
        endif()
        string(REPLACE ";" "," _joined "${_a_EXPECT}")
        set(_expect "-DEXPECT=${_joined}")
    endif()
    add_test(NAME ${test_name}
             COMMAND ${CMAKE_COMMAND} -DOBJECTS_FILE=${_list} -DLABEL=${objlib} ${_tool} ${_expect}
                     -P "${CMAKE_SOURCE_DIR}/cmake/ThemeWriterCheck.cmake")
    set_tests_properties(${test_name} PROPERTIES LABELS "build;gui")
    if(_a_CONTROL)
        # The control must be rejected for the reasons it was built for (every EXPECTed writer
        # seen), not for a crash.
        set_tests_properties(${test_name} PROPERTIES
            PASS_REGULAR_EXPRESSION "ThemeWriterCheck: VIOLATION: ${objlib}: references a writer:"
            FAIL_REGULAR_EXPRESSION "ThemeWriterCheck: CONTROL MISSED")
    endif()
endfunction()
