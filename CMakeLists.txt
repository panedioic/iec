# CMakeLists.txt — build the two artifacts needed to exercise the services.
#
# This is the test build, not a replacement for the Makefile: it produces
#   dist/iec_server          the tty + exec servers
#   dist/libiec_preload.so   the client-side shim in LD_PRELOAD form
# both under the CMake binary directory, so they can be copied to a test host
# or device together. `libiec_preload.so` is deliberately named differently
# from the Makefile's `libiec.so`: a stale artifact from the other build system
# cannot be picked up by accident.
#
#   cmake -S . -B build-cmake
#   cmake --build build-cmake -j
#
# The fake root and the qemu objects are data, not build outputs. Their paths
# are compiled into the server; override them for an installed tree with
#   -DIEC_FAKE_SYSROOT=... -DIEC_QEMU_DIR=...
cmake_minimum_required(VERSION 3.16)
project(iec VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep the two test artifacts together, mirroring the Makefile's dist/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist")

set(IEC_FAKE_SYSROOT "${CMAKE_CURRENT_SOURCE_DIR}/dist/fake_sysroot"
    CACHE PATH "Where absolute paths are rewritten when the PATH capability is 0")
set(IEC_QEMU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/qemu"
    CACHE PATH "Directory holding libqemu-<guest>.so")

set(IEC_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/common")

# ── server ────────────────────────────────────────────────────────────────
add_executable(iec_server
    main.cpp
    tty/tty_server.cpp
    exec/exec_server.cpp
    exec/caps.cpp
    exec/exec_so.cpp
    exec/exec_qemu.cpp
    exec/exec_shebang.cpp
    exec/elf.cpp
    common/sock_server.cpp)

target_include_directories(iec_server PRIVATE ${IEC_INCLUDES})
target_compile_definitions(iec_server PRIVATE
    _GNU_SOURCE
    "FAKE_SYSROOT=\"${IEC_FAKE_SYSROOT}\""
    "PATH_QEMU_X86_64_SO=\"${IEC_QEMU_DIR}/libqemu-x86_64.so\""
    "PATH_QEMU_AARCH64_SO=\"${IEC_QEMU_DIR}/libqemu-aarch64.so\"")
target_compile_options(iec_server PRIVATE -Wall)
target_link_libraries(iec_server PRIVATE dl pthread)

# ── preload shim ──────────────────────────────────────────────────────────
# tty/shim.c and exec/shim.c are the two halves of the client side; rpc.c and
# sock_client.c are shared by both, and must be compiled in: an LD_PRELOAD
# object cannot expect the host to provide them.
add_library(iec_preload SHARED
    tty/shim.c
    exec/shim.c
    common/rpc.c
    common/sock_client.c)

target_include_directories(iec_preload PRIVATE ${IEC_INCLUDES})
target_compile_definitions(iec_preload PRIVATE INT_PRELOAD _GNU_SOURCE)
target_compile_options(iec_preload PRIVATE -Wall)
target_link_libraries(iec_preload PRIVATE dl)

set_target_properties(iec_preload PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "iec_preload")
