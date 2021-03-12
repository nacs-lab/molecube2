project(molecube2)

## Setup
cmake_minimum_required(VERSION 3.0)
set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CMakeCMacros)
include(GNUInstallDirs)

set(SYSTEMD_SERVICE_DIR "${CMAKE_INSTALL_LIBDIR}/systemd/system"
  CACHE PATH "Systemd service file install dir")
set(SYSTEMD_SYSUSERS_DIR "${CMAKE_INSTALL_LIBDIR}/sysusers.d"
  CACHE PATH "Systemd sysusers file install dir")
set(SYSTEMD_TMPFILES_DIR "${CMAKE_INSTALL_LIBDIR}/tmpfiles.d"
  CACHE PATH "Systemd tmpfiles file install dir")

find_package(PkgConfig REQUIRED)
set(MCDEP_PKGS nacs-utils>=10.1 nacs-seq libzmq>=4.0)
pkg_check_modules(NACS_KERNEL nacs-kernel)
if(NACS_KERNEL_FOUND)
  set(MCDEP_PKGS ${MCDEP_PKGS} nacs-kernel)
endif()
pkg_check_modules(MCDEP REQUIRED ${MCDEP_PKGS})
find_package(yaml-cpp REQUIRED CONFIG NO_SYSTEM_ENVIRONMENT_PATH)

# Remove rdynamic
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS)
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS)

add_definitions(-Wall -Wextra -g -pthread -faligned-new -fvisibility=hidden
  -Wno-psabi -Wno-unknown-warning-option
  -fno-math-errno -Wconversion -Wno-sign-conversion ${MCDEP_CFLAGS})
include_directories(${MCDEP_INCLUDE_DIRS} ${YAML_CPP_INCLUDE_DIR})

set(LINKER_FLAGS -Wl,--as-needed -Wl,--no-undefined -Wl,--gc-sections
  -pthread -fno-math-errno -Wl,-Bsymbolic-functions)

cmake_c_add_flags(CMAKE_C_FLAGS -std=gnu11)
cmake_c_add_flags(CMAKE_CXX_FLAGS -std=gnu++17 -fno-rtti)
cmake_c_add_flags(CMAKE_EXE_LINKER_FLAGS ${LINKER_FLAGS})
cmake_c_add_flags(CMAKE_STATIC_LINKER_FLAGS ${LINKER_FLAGS})
cmake_c_add_flags(CMAKE_SHARED_LINKER_FLAGS ${LINKER_FLAGS})
cmake_c_add_flags(CMAKE_MODULE_LINKER_FLAGS ${LINKER_FLAGS})

add_subdirectory(lib)
add_subdirectory(server)
add_subdirectory(data)
add_subdirectory(test)
