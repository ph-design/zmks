cmake_minimum_required(VERSION 3.13.1)

set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../../module")

find_package(Zephyr REQUIRED HINTS ../../../zephyr)
project(esb_test)

target_sources(app PRIVATE src/main.c)
