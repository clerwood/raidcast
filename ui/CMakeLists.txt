add_library(raidcast_ui STATIC ui_shell.cpp updater.cpp settings.cpp)
target_include_directories(raidcast_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(raidcast_ui PUBLIC raidcast_common imgui d3d11 dxgi winhttp shell32
                                  PRIVATE nlohmann_json::nlohmann_json)
raidcast_enable_cppwinrt(raidcast_ui)

# The repository the update check queries. Override at configure time for forks.
set(RAIDCAST_REPO "clerwood/raidcast" CACHE STRING "GitHub owner/repo for update checks")
target_compile_definitions(raidcast_ui PUBLIC RAIDCAST_REPO="${RAIDCAST_REPO}")
