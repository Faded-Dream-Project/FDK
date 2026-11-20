cmake_minimum_required(VERSION 3.16)
project(fdk_cmake_consumer C)

# The consumer-build test: find_package(fdk CONFIG) against an
# INSTALLED FDK (never the source tree) — the config package's
# integration test (fdkConfig.cmake). The prefix arrives via
# CMAKE_PREFIX_PATH from the Makefile driver (or the caller).

find_package(fdk REQUIRED CONFIG)

add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE fdk::fdk)
