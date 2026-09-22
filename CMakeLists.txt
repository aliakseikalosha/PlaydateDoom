cmake_minimum_required(VERSION 3.14)
set(CMAKE_C_STANDARD 11)

set(ENVSDK $ENV{PLAYDATE_SDK_PATH})

if (NOT ${ENVSDK} STREQUAL "")
	file(TO_CMAKE_PATH ${ENVSDK} SDK)
else()
	execute_process(
			COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config"
			COMMAND head -n 1
			COMMAND cut -c9-
			OUTPUT_VARIABLE SDK
			OUTPUT_STRIP_TRAILING_WHITESPACE
	)
endif()

if (NOT EXISTS ${SDK})
	message(FATAL_ERROR "SDK Path not found; set ENV value PLAYDATE_SDK_PATH")
	return()
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
set(CMAKE_XCODE_GENERATE_SCHEME TRUE)

set(PLAYDATE_GAME_NAME doom)
set(PLAYDATE_GAME_DEVICE doom_DEVICE)

project(${PLAYDATE_GAME_NAME} C ASM)

set(DOOM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/doomgeneric/doomgeneric)

# Doom engine + the Playdate platform layer (all built with pd_compat.h forced in).
set(DOOM_SOURCES
	${DOOM_DIR}/dummy.c
	${DOOM_DIR}/am_map.c
	${DOOM_DIR}/doomdef.c
	${DOOM_DIR}/doomstat.c
	${DOOM_DIR}/dstrings.c
	${DOOM_DIR}/d_event.c
	${DOOM_DIR}/d_items.c
	${DOOM_DIR}/d_iwad.c
	${DOOM_DIR}/d_loop.c
	${DOOM_DIR}/d_main.c
	${DOOM_DIR}/d_mode.c
	${DOOM_DIR}/d_net.c
	${DOOM_DIR}/f_finale.c
	${DOOM_DIR}/f_wipe.c
	${DOOM_DIR}/g_game.c
	${DOOM_DIR}/hu_lib.c
	${DOOM_DIR}/hu_stuff.c
	${DOOM_DIR}/info.c
	${DOOM_DIR}/i_cdmus.c
	${DOOM_DIR}/i_endoom.c
	${DOOM_DIR}/i_joystick.c
	${DOOM_DIR}/i_scale.c
	${DOOM_DIR}/i_sound.c
	${DOOM_DIR}/i_system.c
	${DOOM_DIR}/i_timer.c
	${DOOM_DIR}/memio.c
	${DOOM_DIR}/m_argv.c
	${DOOM_DIR}/m_bbox.c
	${DOOM_DIR}/m_cheat.c
	${DOOM_DIR}/m_config.c
	${DOOM_DIR}/m_controls.c
	${DOOM_DIR}/m_fixed.c
	${DOOM_DIR}/m_menu.c
	${DOOM_DIR}/m_misc.c
	${DOOM_DIR}/m_random.c
	${DOOM_DIR}/p_ceilng.c
	${DOOM_DIR}/p_doors.c
	${DOOM_DIR}/p_enemy.c
	${DOOM_DIR}/p_floor.c
	${DOOM_DIR}/p_inter.c
	${DOOM_DIR}/p_lights.c
	${DOOM_DIR}/p_map.c
	${DOOM_DIR}/p_maputl.c
	${DOOM_DIR}/p_mobj.c
	${DOOM_DIR}/p_plats.c
	${DOOM_DIR}/p_pspr.c
	${DOOM_DIR}/p_saveg.c
	${DOOM_DIR}/p_setup.c
	${DOOM_DIR}/p_sight.c
	${DOOM_DIR}/p_spec.c
	${DOOM_DIR}/p_switch.c
	${DOOM_DIR}/p_telept.c
	${DOOM_DIR}/p_tick.c
	${DOOM_DIR}/p_user.c
	${DOOM_DIR}/r_bsp.c
	${DOOM_DIR}/r_data.c
	${DOOM_DIR}/r_draw.c
	${DOOM_DIR}/r_main.c
	${DOOM_DIR}/r_plane.c
	${DOOM_DIR}/r_segs.c
	${DOOM_DIR}/r_sky.c
	${DOOM_DIR}/r_things.c
	${DOOM_DIR}/sha1.c
	${DOOM_DIR}/sounds.c
	${DOOM_DIR}/statdump.c
	${DOOM_DIR}/st_lib.c
	${DOOM_DIR}/st_stuff.c
	${DOOM_DIR}/s_sound.c
	${DOOM_DIR}/tables.c
	${DOOM_DIR}/v_video.c
	${DOOM_DIR}/wi_stuff.c
	${DOOM_DIR}/w_checksum.c
	${DOOM_DIR}/w_file.c
	${DOOM_DIR}/w_main.c
	${DOOM_DIR}/w_wad.c
	${DOOM_DIR}/z_zone.c
	${DOOM_DIR}/w_file_stdc.c
	${DOOM_DIR}/i_input.c
	${DOOM_DIR}/i_video.c
	${DOOM_DIR}/doomgeneric.c
	${DOOM_DIR}/mus2mid.c
	src/doomgeneric_playdate.c
)

set(PD_SOURCES
	src/main.c
	src/i_playdate_sound.c
	src/pd_compat.c
)

if (TOOLCHAIN STREQUAL "armgcc")
	add_executable(${PLAYDATE_GAME_DEVICE} ${DOOM_SOURCES} ${PD_SOURCES})
	set(GAME_TARGET ${PLAYDATE_GAME_DEVICE})
else()
	add_library(${PLAYDATE_GAME_NAME} SHARED ${DOOM_SOURCES} ${PD_SOURCES})
	set(GAME_TARGET ${PLAYDATE_GAME_NAME})
endif()

target_include_directories(${GAME_TARGET} PRIVATE ${DOOM_DIR} src)

# Doom renders 320x240 palettised (full Playdate screen height, letterboxed
# left/right only); the platform layer dithers it to 1-bit.
# 4 MiB zone heap (the device heap is 8 MiB); low detail is much faster.
target_compile_definitions(${GAME_TARGET} PRIVATE
	CMAP256
	FEATURE_SOUND
	SCREENHEIGHT=240
	DOOMGENERIC_RESX=320
	DOOMGENERIC_RESY=240
	DEFAULT_RAM=4
	MIN_RAM=2
	DEFAULT_DETAIL=1
)

set_source_files_properties(${DOOM_SOURCES} PROPERTIES COMPILE_OPTIONS
	"-include;${CMAKE_CURRENT_SOURCE_DIR}/src/pd_compat.h;-w;-fcommon;-std=gnu99")

include(${SDK}/C_API/buildsupport/playdate_game.cmake)
