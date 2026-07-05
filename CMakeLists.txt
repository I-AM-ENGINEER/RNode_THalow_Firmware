# Top-level project CMakeLists for the RNode-HaLow ESP-IDF firmware.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "src" "libs")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(rnode-halow)
