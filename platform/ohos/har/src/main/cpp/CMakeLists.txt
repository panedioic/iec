# CMake for the smoke-test HAR: builds libiec_server.so
cmake_minimum_required(VERSION 3.5.0)
project(iec)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(iec_server SHARED napi_init.cpp)

target_link_libraries(iec_server PUBLIC libace_napi.z.so libhilog_ndk.z.so)
