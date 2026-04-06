# Doom-only firmware build — excludes chess and gbemu to maximize RAM
include(${CMAKE_CURRENT_LIST_DIR}/src/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/doom/micropython.cmake)
