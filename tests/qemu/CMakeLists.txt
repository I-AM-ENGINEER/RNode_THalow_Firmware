# Level-2 test project: firmware modules under QEMU (ESP32-S3).
# Builds the real production sources against the real ESP-IDF stack
# (xtensa codegen, real FreeRTOS scheduler) and runs Unity tests on boot.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(qemu_tests)
