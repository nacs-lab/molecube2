#

add_definitions(-UNDEBUG)

add_executable(test_pulser test_pulser.cpp)
target_link_libraries(test_pulser libmolecube)

add_executable(test_controller test_controller.cpp)
target_link_libraries(test_controller libmolecube)
