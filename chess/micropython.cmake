add_library(usermod_chess INTERFACE)

target_sources(usermod_chess INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/chess_module.c
    ${CMAKE_CURRENT_LIST_DIR}/chess_game.c
    ${CMAKE_CURRENT_LIST_DIR}/mcu-max.c
)

target_include_directories(usermod_chess INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../src
)

target_compile_definitions(usermod_chess INTERFACE
    MCUMAX_HASHING_ENABLED
    MCUMAX_HASH_TABLE_SIZE=4096
)

target_link_libraries(usermod INTERFACE usermod_chess)
