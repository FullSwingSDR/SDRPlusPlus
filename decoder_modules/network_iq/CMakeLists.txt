cmake_minimum_required(VERSION 3.13)
project(network_iq)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(network_iq PRIVATE "src/")
