#

set(libmolecube_SRCS
  config.cpp
  controller.cpp
  ctrl_iface.cpp
  dummy_pulser.cpp
  namesconfig.cpp
  pulser.cpp
  server.cpp)

add_library(libmolecube SHARED ${libmolecube_SRCS})

target_link_libraries(libmolecube ${MCDEP_LIBRARIES} ${YAML_CPP_LIBRARIES} rt)

set_target_properties(libmolecube PROPERTIES
  VERSION 0.1
  SOVERSION 0
  COMPILE_FLAGS "-fvisibility=hidden ${MCDEP_LDFLAGS_OTHER}"
  OUTPUT_NAME molecube)
install(TARGETS libmolecube LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
