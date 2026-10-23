# asb_core: model, trust logic, Applier, firewall text, status, side-channel worker (WS-3, WS-5).
# QtCore and QtNetwork only. INV-37: no QtGui types, so nothing here may link Qt6::Gui.
add_library(asb_core STATIC
    BuildInfo.cpp BuildInfo.h
    BuildStamp.cpp BuildStamp.h   # v1.1 D61: the build stamp (wording only, INV-120)
    # WS-3: config model, validation, AtomicJson, status types, engine seam
    AllowedClients.cpp AllowedClients.h
    AppLocations.cpp AppLocations.h
    AtomicJson.cpp AtomicJson.h
    BridgeConfig.cpp BridgeConfig.h
    ConfigFiles.cpp ConfigFiles.h
    ConfigJson.cpp ConfigJson.h
    EditTracker.cpp EditTracker.h
    Exit.h
    FileIo.h
    FirewallKeywords.h
    HelpText.h
    IBridgeEngine.h
    RunningTime.cpp RunningTime.h
    StatusPrinter.cpp StatusPrinter.h
    StatusStore.cpp StatusStore.h
    # WS-5: trust logic, Applier transaction, runner (platform-neutral)
    AceJudge.cpp AceJudge.h
    Applier.cpp Applier.h
    Barrier.cpp Barrier.h
    BridgeRunner.cpp BridgeRunner.h
    DataDirectoryTrust.cpp DataDirectoryTrust.h
    EngineClaim.cpp EngineClaim.h
    FilesystemPolicy.cpp FilesystemPolicy.h
    FirewallPlan.cpp FirewallPlan.h
    GatedSeams.cpp GatedSeams.h
    InstallationEvidence.cpp InstallationEvidence.h
    Interfaces.cpp Interfaces.h
    IrreversibleGate.cpp IrreversibleGate.h
    LockIdentity.cpp LockIdentity.h
    OperationHost.cpp OperationHost.h
    PhaseClock.cpp PhaseClock.h
    ServicePlan.cpp ServicePlan.h
    StartToken.cpp StartToken.h
    TrustedConfig.cpp TrustedConfig.h
    UninstallPreview.cpp UninstallPreview.h   # WS-16
    # WS-4: exit primitives, deadline watchdog, side-channel worker and log sinks
    DeadlineWatchdog.cpp DeadlineWatchdog.h DeadlineWatchdog_p.h
    LogSinks.cpp LogSinks.h
    SideChannel.cpp SideChannel.h
)
if(WIN32)
    target_sources(asb_core PRIVATE Exit_win.cpp DeadlineWatchdog_win.cpp)
else()
    target_sources(asb_core PRIVATE Exit_posix.cpp DeadlineWatchdog_posix.cpp)
endif()
find_package(Threads REQUIRED)
target_include_directories(asb_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_definitions(asb_core PRIVATE ASB_VERSION="${PROJECT_VERSION}")
target_link_libraries(asb_core PUBLIC Qt6::Core Qt6::Network Threads::Threads PRIVATE asb_warnings)
if(WIN32)
    # AppLocations: SHGetKnownFolderPath (shell32) + CoTaskMemFree (ole32).
    target_link_libraries(asb_core PRIVATE shell32 ole32)
endif()
