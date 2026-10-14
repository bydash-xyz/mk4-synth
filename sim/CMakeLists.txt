cmake_minimum_required(VERSION 3.16)
# Delegate to root CMakeLists.txt if invoked directly in sim/
get_filename_component(PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_subdirectory(${PARENT_DIR} ${CMAKE_CURRENT_BINARY_DIR}/root_build)
