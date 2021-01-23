#

add_definitions(-UNDEBUG)

add_executable(test_pulser test_pulser.cpp)
target_link_libraries(test_pulser libmolecube)

add_executable(test_set_ttl test_set_ttl.cpp)
target_link_libraries(test_set_ttl libmolecube)

add_executable(test_set_clock test_set_clock.cpp)
target_link_libraries(test_set_clock libmolecube)

add_executable(test_set_dds test_set_dds.cpp)
target_link_libraries(test_set_dds libmolecube)

add_executable(test_controller test_controller.cpp)
target_link_libraries(test_controller libmolecube)

add_executable(test_DDS test_DDS.cpp)
target_link_libraries(test_DDS libmolecube)

add_executable(test_DDS_ramp test_DDS_ramp.cpp)
target_link_libraries(test_DDS_ramp libmolecube)

add_executable(test_DDS_full_ramp test_DDS_full_ramp.cpp)
target_link_libraries(test_DDS_full_ramp libmolecube)
