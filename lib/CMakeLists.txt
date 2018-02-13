#

set(libmolecube_SRCS
  server.cpp)

if(NOT DUMMY_BACKEND_ONLY)
  set(libmolecube_SRCS
    ${libmolecube_SRCS}
    controller.cpp)
endif()

add_library(libmolecube SHARED ${libmolecube_SRCS})

target_link_libraries(libmolecube ${MCDEP_LIBRARIES})

set_target_properties(libmolecube PROPERTIES
  VERSION 0.1
  SOVERSION 0
  COMPILE_FLAGS "-fvisibility=hidden ${MCDEP_LDFLAGS_OTHER}"
  OUTPUT_NAME molecube)
install(TARGETS libmolecube LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
