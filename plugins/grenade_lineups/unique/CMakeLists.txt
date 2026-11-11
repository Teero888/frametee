# The library for Unique Race, which the editor opens while a project runs on it
# (libgrenade_lineups.unique beside the DDNet one).
if(NOT TARGET unique_physics)
    return()
endif()

gl_add_plugin(grenade_lineups_unique_plugin unique_physics)

set(runtime_dir "${CMAKE_BINARY_DIR}/plugins/grenade_lineups")
set_target_properties(grenade_lineups_unique_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${runtime_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${runtime_dir}"
    OUTPUT_NAME "grenade_lineups.unique"
    BUILD_RPATH_USE_ORIGIN ON
)
if(WIN32)
    set_target_properties(grenade_lineups_unique_plugin PROPERTIES PREFIX "")
endif()
if(NOT WIN32 AND NOT APPLE)
    set_target_properties(grenade_lineups_unique_plugin PROPERTIES
        INSTALL_RPATH "$ORIGIN;$ORIGIN/../../libs/unique_physics;$ORIGIN/../../libs/ddnet_physics/libs/ddnet_map_loader"
    )
endif()
install(TARGETS grenade_lineups_unique_plugin
    RUNTIME DESTINATION "plugins/grenade_lineups" COMPONENT Runtime
    LIBRARY DESTINATION "plugins/grenade_lineups" COMPONENT Runtime
)
