project(molecube2)

## Setup
cmake_minimum_required(VERSION 3.0)
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CMakeCMacros)
include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MCDEP REQUIRED nacs-utils>=2.1 libzmq>=4.0)

# Remove rdynamic
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS)
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS)

add_definitions(-Wall -Wextra -g -pthread -fvisibility=hidden
  -fno-math-errno -Wconversion -Wno-sign-conversion ${MCDEP_CFLAGS})
include_directories(${MCDEP_INCLUDE_DIRS})

set(LINKER_FLAGS -Wl,--as-needed -Wl,--no-undefined -Wl,--gc-sections
  -pthread -fno-math-errno)

cmake_c_add_flags(CMAKE_C_FLAGS -std=gnu11)
cmake_c_add_flags(CMAKE_CXX_FLAGS -std=gnu++14)
cmake_c_add_flags(CMAKE_EXE_LINKER_FLAGS ${LINKER_FLAGS})
cmake_c_add_flags(CMAKE_STATIC_LINKER_FLAGS ${LINKER_FLAGS})
cmake_c_add_flags(CMAKE_SHARED_LINKER_FLAGS ${LINKER_FLAGS})
cmake_c_add_flags(CMAKE_MODULE_LINKER_FLAGS ${LINKER_FLAGS})

add_subdirectory(lib)