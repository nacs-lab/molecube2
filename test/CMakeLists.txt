#

add_definitions(-UNDEBUG)

add_executable(test_pulser test_pulser.cpp)
target_link_libraries(test_pulser libmolecube)

add_executable(test_benchmark_pulse test_benchmark_pulse.cpp)
target_link_libraries(test_benchmark_pulse libmolecube)

add_executable(test_set_ttl test_set_ttl.cpp)
target_link_libraries(test_set_ttl libmolecube)

add_executable(test_trigger test_trigger.cpp)
target_link_libraries(test_trigger libmolecube)

add_executable(test_set_clock test_set_clock.cpp)
target_link_libraries(test_set_clock libmolecube)

add_executable(test_set_dds test_set_dds.cpp)
target_link_libraries(test_set_dds libmolecube)

add_executable(test_init_dds test_init_dds.cpp)
target_link_libraries(test_init_dds libmolecube)

add_executable(test_dump_all_dds test_dump_all_dds.cpp)
target_link_libraries(test_dump_all_dds libmolecube)

add_executable(test_dds_actions test_dds_actions.cpp)
target_link_libraries(test_dds_actions libmolecube)

add_executable(test_controller test_controller.cpp)
target_link_libraries(test_controller libmolecube)

add_executable(test_DDS test_DDS.cpp)
target_link_libraries(test_DDS libmolecube)

add_executable(test_DDS_ramp test_DDS_ramp.cpp)
target_link_libraries(test_DDS_ramp libmolecube)

add_executable(test_DDS_full_ramp test_DDS_full_ramp.cpp)
target_link_libraries(test_DDS_full_ramp libmolecube)

add_executable(test_sequence test_sequence.cpp)
target_link_libraries(test_sequence libmolecube)
