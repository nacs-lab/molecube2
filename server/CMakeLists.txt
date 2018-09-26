#

set(SERVER_SOURCES
  main.cpp)
add_executable(molecube ${SERVER_SOURCES})

target_link_libraries(molecube libmolecube)
set_target_properties(molecube PROPERTIES OUTPUT_NAME molecube2)

install(TARGETS molecube DESTINATION "${CMAKE_INSTALL_BINDIR}")
