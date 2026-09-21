add_executable(capture-probe main.cpp)
target_link_libraries(capture-probe PRIVATE raidcast_capture raidcast_encode)
raidcast_enable_cppwinrt(capture-probe)
raidcast_stage_runtime(capture-probe)

# A diagnostic, not part of the product — deliberately not install()ed.
