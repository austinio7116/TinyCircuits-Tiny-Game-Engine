add_library(usermod_gbemu INTERFACE)

target_sources(usermod_gbemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/gb_emu_module.c
    ${CMAKE_CURRENT_LIST_DIR}/gb_emu_core.c
)

target_include_directories(usermod_gbemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_gbemu)
