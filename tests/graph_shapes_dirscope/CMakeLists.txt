# Graph-shape fixture: a dependency that arrives through the directory-scope link_libraries() call.
# The command seeds LINK_LIBRARIES of every target created afterwards in this directory, so the
# executable below carries Qt6::Gui although its own target_link_libraries() never names it.
link_libraries(Qt6::Gui)
add_executable(asb_gs_dirscope EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
