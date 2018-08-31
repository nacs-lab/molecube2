#

add_definitions(-UNDEBUG)

add_executable(test_pulser test_pulser.cpp)
target_link_libraries(test_pulser libmolecube)
