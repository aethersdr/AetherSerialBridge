# The names the Windows native tests (and the CI steps around them) give to what they create on the machine, and
# the product names they act on. ONE source:
#   - the windows job's FAT32 step in .github/workflows/ci.yml reads it;
#   - tests/win/integration/CMakeLists.txt turns the test-side entries into WinTestNames.h, which the native tests
#     and asb_win_qual include;
#   - tools/check_win_test_names.py (static-checks) fails when a product constant, the station script or any of the
#     files above drifts from it, or when a test-side name is written out as a literal anywhere but here.
# Keep every value a single-quoted string (or an array of them): the C++ and Python readers parse exactly that.
@{
    # ---- product names (the product does not read this file; the check compares them with the source) ----------
    ServiceName           = 'AetherSerialBridge'          # src/core/ServicePlan.h kServiceName
    InstallFolderLeaf     = 'AetherSerialBridge'          # %ProgramFiles% and %ProgramData% (src/core/AppLocations.cpp, ProcessGuard.cpp)
    FirewallGroup         = 'Aether Serial Bridge'        # src/core/FirewallPlan.h kGroup
    UninstallKeyLeaf      = 'AetherSerialBridge'          # HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\<leaf>, 64-bit view
                                                          # (src/platform/win/UninstallEntry.h kKey; WS-16 Apps & features entry)

    # ---- test-side names ----------------------------------------------------------------------------------------
    AccountPrefix         = 'asbtest-'                    # + 8 lowercase hex: local test accounts (and their profiles)
    ScratchPrefix         = 'asb-ws6-'                    # scratch folders under the temp folders
    ReportLog             = 'asb-ws7-reports.log'         # tst_ServiceNative's report log in %TEMP%
    KeywordMatrixCiGroup  = 'Aether Serial Bridge keyword matrix (CI)'     # tst_CimNative fw_keyword_matrix
    KeywordMatrixTestGroup = 'Aether Serial Bridge keyword matrix (test)'  # asb_win_qual keywords
    FatLabel              = 'ASBFAT'                      # the FAT32 test volume's label (ci.yml)
    FatImagePrefix        = 'asb-fat32'                   # its .vhdx file name prefix (ci.yml)

    # ---- the station script's names (qualification/ws7-station.ps1 runs alone on the station PC, so it keeps them
    #      as literals; the check compares them) ---------------------------------------------------------------
    AsideInfix            = '.ws7-aside-'                 # <install folder>.ws7-aside-<stamp>
    StationEvidenceLeaf   = 'asb-ws7-station'             # %SystemDrive%\asb-ws7-station
}
