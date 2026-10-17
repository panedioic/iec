# CMake for the OHOS HAR: builds libiec_server.so and wraps the capability
# probes from the repository root.
cmake_minimum_required(VERSION 3.5.0)
project(iec_har)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reuse the root project instead of copying exec/caps.cpp into the HAR. Only
# the iec_caps object is linked; EXCLUDE_FROM_ALL keeps the reference server
# and the preload shim out of the HAP.
set(IEC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..")
add_subdirectory("${IEC_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/iec_root" EXCLUDE_FROM_ALL)

add_library(iec_har SHARED napi_init.cpp)
set_target_properties(iec_har PROPERTIES OUTPUT_NAME iec_server)

target_link_libraries(iec_har PUBLIC libace_napi.z.so libhilog_ndk.z.so iec_caps)
