add_library(usermod_gbemu INTERFACE)

target_sources(usermod_gbemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/gb_emu_module.c
    ${CMAKE_CURRENT_LIST_DIR}/gb_emu_core.c
    ${CMAKE_CURRENT_LIST_DIR}/minigb_apu.c
)

target_include_directories(usermod_gbemu INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(usermod_gbemu INTERFACE
    AUDIO_SAMPLE_RATE=22050
    MINIGB_APU_AUDIO_FORMAT_S16SYS
)

target_link_libraries(usermod INTERFACE usermod_gbemu)
