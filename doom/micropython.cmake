add_library(usermod_doom INTERFACE)

set(DOOM_DIR ${CMAKE_CURRENT_LIST_DIR})
set(DG_DIR ${DOOM_DIR}/doomgeneric)

target_sources(usermod_doom INTERFACE
    # Our platform glue
    ${DOOM_DIR}/doom_module.c
    ${DOOM_DIR}/doom_core.c
    ${DOOM_DIR}/doom_cache.c
    ${DOOM_DIR}/w_file_xip.c

    # doomgeneric core
    ${DG_DIR}/am_map.c
    ${DG_DIR}/d_event.c
    ${DG_DIR}/d_items.c
    ${DG_DIR}/d_iwad.c
    ${DG_DIR}/d_loop.c
    ${DG_DIR}/d_main.c
    ${DG_DIR}/d_mode.c
    ${DG_DIR}/d_net.c
    ${DG_DIR}/doomdef.c
    ${DG_DIR}/doomgeneric.c
    ${DG_DIR}/doomstat.c
    ${DG_DIR}/dstrings.c
    ${DG_DIR}/dummy.c
    ${DG_DIR}/f_finale.c
    ${DG_DIR}/f_wipe.c
    ${DG_DIR}/g_game.c
    ${DG_DIR}/gusconf.c
    ${DG_DIR}/hu_lib.c
    ${DG_DIR}/hu_stuff.c
    ${DG_DIR}/i_endoom.c
    ${DG_DIR}/i_input.c
    ${DG_DIR}/i_joystick.c
    ${DG_DIR}/i_scale.c
    ${DG_DIR}/i_sound.c
    ${DG_DIR}/i_system.c
    ${DG_DIR}/i_timer.c
    ${DG_DIR}/i_video.c
    ${DG_DIR}/icon.c
    ${DG_DIR}/info.c
    ${DG_DIR}/m_argv.c
    ${DG_DIR}/m_bbox.c
    ${DG_DIR}/m_cheat.c
    ${DG_DIR}/m_config.c
    ${DG_DIR}/m_controls.c
    ${DG_DIR}/m_fixed.c
    ${DG_DIR}/m_menu.c
    ${DG_DIR}/m_misc.c
    ${DG_DIR}/m_random.c
    ${DG_DIR}/memio.c
    ${DG_DIR}/mus2mid.c
    ${DG_DIR}/p_ceilng.c
    ${DG_DIR}/p_doors.c
    ${DG_DIR}/p_enemy.c
    ${DG_DIR}/p_floor.c
    ${DG_DIR}/p_inter.c
    ${DG_DIR}/p_lights.c
    ${DG_DIR}/p_map.c
    ${DG_DIR}/p_maputl.c
    ${DG_DIR}/p_mobj.c
    ${DG_DIR}/p_plats.c
    ${DG_DIR}/p_pspr.c
    ${DG_DIR}/p_saveg.c
    ${DG_DIR}/p_setup.c
    ${DG_DIR}/p_sight.c
    ${DG_DIR}/p_spec.c
    ${DG_DIR}/p_switch.c
    ${DG_DIR}/p_telept.c
    ${DG_DIR}/p_tick.c
    ${DG_DIR}/p_user.c
    ${DG_DIR}/r_bsp.c
    ${DG_DIR}/r_data.c
    ${DG_DIR}/r_draw.c
    ${DG_DIR}/r_main.c
    ${DG_DIR}/r_plane.c
    ${DG_DIR}/r_segs.c
    ${DG_DIR}/r_sky.c
    ${DG_DIR}/r_things.c
    ${DG_DIR}/s_sound.c
    ${DG_DIR}/sha1.c
    ${DG_DIR}/sounds.c
    ${DG_DIR}/st_lib.c
    ${DG_DIR}/st_stuff.c
    ${DG_DIR}/statdump.c
    ${DG_DIR}/tables.c
    ${DG_DIR}/v_video.c
    ${DG_DIR}/w_checksum.c
    ${DG_DIR}/w_file.c
    ${DG_DIR}/w_file_stdc.c
    ${DG_DIR}/w_main.c
    ${DG_DIR}/w_wad.c
    ${DG_DIR}/wi_stuff.c
    ${DG_DIR}/z_zone.c
)

target_include_directories(usermod_doom INTERFACE
    ${DOOM_DIR}
    ${DG_DIR}
    ${DOOM_DIR}/../src/io
    ${DOOM_DIR}/../src
)

target_compile_options(usermod_doom INTERFACE
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough
    -Wno-double-promotion
    -Wno-type-limits
    -Wno-unused-parameter
    -Wno-unused-but-set-parameter
    -Wno-unused-result
    -Wno-missing-braces
    -Wno-char-subscripts
    -Wno-format
    -Wno-format-truncation
    $<$<COMPILE_LANGUAGE:C>:-Wno-enum-conversion>
    $<$<COMPILE_LANGUAGE:C>:-Wno-pointer-sign>
)

target_compile_definitions(usermod_doom INTERFACE
    DOOM_THUMBY=1
    DOOMGENERIC_RESX=128
    DOOMGENERIC_RESY=128
)

target_link_libraries(usermod INTERFACE usermod_doom)
