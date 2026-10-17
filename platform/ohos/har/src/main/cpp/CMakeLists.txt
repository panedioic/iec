# CMake for the OHOS HAR: builds libiec_server.so from the capability probes
# and the tty/exec server sources in the repository root.
cmake_minimum_required(VERSION 3.5.0)
project(iec_har LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reuse the root project's modules instead of copying them into the HAR. Only
# iec_caps is linked from there; EXCLUDE_FROM_ALL keeps the standalone server
# and the preload shim out of the HAP. The service sources are listed below
# because the executable target beside them owns main() and cannot be linked.
set(IEC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..")
add_subdirectory("${IEC_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/iec_root" EXCLUDE_FROM_ALL)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(IEC_GUEST_SWITCH "${IEC_ROOT}/loader/guest_switch_x86_64.S")
else()
    set(IEC_GUEST_SWITCH "${IEC_ROOT}/loader/guest_switch_aarch64.S")
endif()

add_library(iec_har SHARED
    napi_init.cpp
    "${IEC_ROOT}/tty/tty_server.cpp"
    "${IEC_ROOT}/exec/exec_server.cpp"
    "${IEC_ROOT}/exec/path_resolver.cpp"
    "${IEC_ROOT}/exec/exec_loader.cpp"
    "${IEC_ROOT}/exec/exec_so.cpp"
    "${IEC_ROOT}/exec/exec_qemu.cpp"
    "${IEC_ROOT}/exec/exec_shebang.cpp"
    "${IEC_ROOT}/exec/elf.cpp"
    "${IEC_ROOT}/fs/resolver.c"
    "${IEC_ROOT}/loader/loader.cpp"
    "${IEC_GUEST_SWITCH}"
    "${IEC_ROOT}/common/sock_server.cpp")

target_include_directories(iec_har PRIVATE
    "${IEC_ROOT}"
    "${IEC_ROOT}/common"
    "${IEC_ROOT}/loader")
# common/constant.h supplies the sandbox paths under __OHOS__, so no host paths
# are defined here.
target_compile_definitions(iec_har PRIVATE _GNU_SOURCE)

set_target_properties(iec_har PROPERTIES OUTPUT_NAME iec_server)

target_link_libraries(iec_har PUBLIC
    libace_napi.z.so libhilog_ndk.z.so
    iec_caps dl pthread)
