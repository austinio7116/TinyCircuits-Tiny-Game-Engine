# This top-level micropython.cmake is responsible for listing
# the individual modules we want to include.
# Paths are absolute, and ${CMAKE_CURRENT_LIST_DIR} can be
# used to prefix subdirectories.

include(${CMAKE_CURRENT_LIST_DIR}/src/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/gbemu/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/chess/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/doom/micropython.cmake)