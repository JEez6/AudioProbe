cmake_minimum_required(VERSION 3.22.1)
project(audioprobe)

add_library(audioprobe SHARED native-lib.cpp)

find_library(log-lib log)
find_library(aaudio-lib aaudio)
find_library(opensles-lib OpenSLES)

target_link_libraries(audioprobe
        ${log-lib}
        ${aaudio-lib}
        ${opensles-lib})
