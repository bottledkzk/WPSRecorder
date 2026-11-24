# Compatibility entry point for the assignment's docs/CMakeLists.txt layout.
cmake_minimum_required(VERSION 3.16)
project(wpsrecorder_documented_build LANGUAGES CXX)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_BINARY_DIR}/application")
