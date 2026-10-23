# Graph-shape fixture: an imported target found in this directory is visible here and below, not
# from the root where the graphs are written. The walk cannot resolve it, so it must keep the
# namespaced name and match it, not drop it. Qt6::Svg is found nowhere else in the project.
find_package(Qt6 QUIET COMPONENTS Svg)
if(TARGET Qt6::Svg)
    add_executable(asb_gs_unresolved_namespaced EXCLUDE_FROM_ALL ${ASB_GS_SOURCE})
    target_link_libraries(asb_gs_unresolved_namespaced PRIVATE Qt6::Svg)
    set(ASB_GS_HAVE_UNRESOLVED ON PARENT_SCOPE)
endif()
