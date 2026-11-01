cmake_minimum_required(VERSION 3.16)
project(genrecomp C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Plain globals as real definitions, not common symbols: ld gives a large common symbol (work_ram,
# g_ps_vdp) a size-derived 32 KB alignment, then warns that it reduces it.
if(NOT MSVC)
    add_compile_options(-fno-common)
endif()

# Options
option(GENRECOMP_BUILD_EXAMPLES "Build example programs" ON)

# SDL2
find_package(SDL2 REQUIRED)
find_package(ZLIB REQUIRED)
if(APPLE)
    enable_language(OBJC)
endif()

# --- genrecomp_native: the shipped runtime. Same sources, built with PS_NATIVE: the
# machine is PixelSorcery's own (ps_hw.c, ps_vdp/ps_render, ps_synth, recompiled Z80).
# No Genesis Plus GX header, source or library is involved.
add_library(genrecomp_native STATIC
    src/genrecomp.c
    src/m68k.c
    src/bus.c
    src/func_table.c
    src/m68k_interp.c
    src/io.c
    src/platform_sdl.c
    src/input.c
    src/ps_vdp.c
    src/ps_render.c
    src/ps_png.c
    src/movie.c
    src/ps_audio.c
    src/ps_synth.c
    src/ps_poll.c
    src/ps_state.c
    src/ps_hw.c
    src/ps_wide.c
    src/ps_menu.c
)
target_include_directories(genrecomp_native
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(genrecomp_native PRIVATE PS_NATIVE)
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE PS_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PS_GIT_VERSION AND DEFINED ENV{PS_VERSION})   # no git (the app's tree): the app's version
    set(PS_GIT_VERSION "$ENV{PS_VERSION}")
endif()
set_source_files_properties(src/ps_menu.c PROPERTIES COMPILE_DEFINITIONS "PS_VERSION=\"${PS_GIT_VERSION}\"")
target_link_libraries(genrecomp_native PUBLIC SDL2::SDL2 ZLIB::ZLIB)
# libm (sin, pow, ...); MSVC's CRT has it built in
if(NOT WIN32)
    target_link_libraries(genrecomp_native PUBLIC m)
endif()
if(APPLE)
    target_sources(genrecomp_native PRIVATE src/present_metal.m)
    target_link_libraries(genrecomp_native PUBLIC "-framework Metal" "-framework QuartzCore" "-framework AppKit")
    target_link_libraries(genrecomp_native PUBLIC objc)
endif()

# --- Launcher: a shelf of cartridges for a family of titles (tools/make_launcher.sh) ---
add_executable(ps_launcher tools/launcher/launcher.c)
if(TARGET SDL2::SDL2main)   # Windows: SDL.h renames main to SDL_main; SDL2main (static, before SDL2) has the entry point
    target_link_libraries(ps_launcher PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(ps_launcher PRIVATE SDL2::SDL2)
if(NOT WIN32)
    target_link_libraries(ps_launcher PRIVATE m)
endif()

# --- Instruction edge cases (ctest), header only: also in native-only builds (Windows CI) ---
enable_testing()
add_executable(genrecomp_test_m68k_div tests/test_m68k_div.c)
target_include_directories(genrecomp_test_m68k_div PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME m68k_div COMMAND genrecomp_test_m68k_div)

# --- Genesis Plus GX hardware backend (static lib) ---
set(GPGX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ext/Genesis-Plus-GX/core)
# Without its sources (the app's bundled tree, a plain download) only the native runtime and the
# launcher are built: no *_oracle builds, self-check, reference runner, tracer or explorer
if(NOT EXISTS ${GPGX_DIR}/system.c)
    message(STATUS "Genesis Plus GX not present: native runtime only")
    return()
endif()
# PixelSorcery hooks (Z80 run/reset, Z80 trace; cpu_hook declared extern) on top of the pinned submodule
set(GPGX_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/patches/genesis-plus-gx.patch)
# games configured in parallel on a fresh checkout would all try to apply it: one at a time
file(LOCK ${CMAKE_CURRENT_SOURCE_DIR}/ext/.gpgx-patch.lock GUARD FILE)
execute_process(COMMAND git apply --reverse --check ${GPGX_PATCH}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ext/Genesis-Plus-GX
    RESULT_VARIABLE GPGX_PATCHED OUTPUT_QUIET ERROR_QUIET)
if(NOT GPGX_PATCHED EQUAL 0)
    execute_process(COMMAND git apply ${GPGX_PATCH}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ext/Genesis-Plus-GX
        RESULT_VARIABLE GPGX_APPLY)
    if(NOT GPGX_APPLY EQUAL 0)
        message(FATAL_ERROR "could not apply ${GPGX_PATCH}")
    endif()
endif()

add_library(genplusgx_hw STATIC
    # Core
    ${GPGX_DIR}/genesis.c
    ${GPGX_DIR}/system.c
    ${GPGX_DIR}/io_ctrl.c
    ${GPGX_DIR}/loadrom.c
    ${GPGX_DIR}/mem68k.c
    ${GPGX_DIR}/membnk.c
    ${GPGX_DIR}/memz80.c
    ${GPGX_DIR}/state.c
    ${GPGX_DIR}/vdp_ctrl.c
    ${GPGX_DIR}/vdp_render.c

    # M68K CPU (Musashi — needed for memory map infrastructure)
    ${GPGX_DIR}/m68k/m68kcpu.c
    ${GPGX_DIR}/m68k/s68kcpu.c

    # Z80 CPU
    ${GPGX_DIR}/z80/z80.c

    # Sound
    ${GPGX_DIR}/sound/blip_buf.c
    ${GPGX_DIR}/sound/eq.c
    ${GPGX_DIR}/sound/psg.c
    ${GPGX_DIR}/sound/sound.c
    ${GPGX_DIR}/sound/ym2612.c
    ${GPGX_DIR}/sound/ym2413.c
    ${GPGX_DIR}/sound/ym3438.c
    ${GPGX_DIR}/sound/opll.c

    # Cartridge hardware
    ${GPGX_DIR}/cart_hw/md_cart.c
    ${GPGX_DIR}/cart_hw/sms_cart.c
    ${GPGX_DIR}/cart_hw/sram.c
    ${GPGX_DIR}/cart_hw/eeprom_93c.c
    ${GPGX_DIR}/cart_hw/eeprom_i2c.c
    ${GPGX_DIR}/cart_hw/eeprom_spi.c
    ${GPGX_DIR}/cart_hw/flash_cfi.c
    ${GPGX_DIR}/cart_hw/ggenie.c
    ${GPGX_DIR}/cart_hw/areplay.c
    ${GPGX_DIR}/cart_hw/megasd.c
    ${GPGX_DIR}/cart_hw/yx5200.c
    ${GPGX_DIR}/cart_hw/svp/svp.c
    ${GPGX_DIR}/cart_hw/svp/ssp16.c

    # Input hardware
    ${GPGX_DIR}/input_hw/input.c
    ${GPGX_DIR}/input_hw/gamepad.c
    ${GPGX_DIR}/input_hw/activator.c
    ${GPGX_DIR}/input_hw/graphic_board.c
    ${GPGX_DIR}/input_hw/lightgun.c
    ${GPGX_DIR}/input_hw/mouse.c
    ${GPGX_DIR}/input_hw/paddle.c
    ${GPGX_DIR}/input_hw/smash.c
    ${GPGX_DIR}/input_hw/sportspad.c
    ${GPGX_DIR}/input_hw/teamplayer.c
    ${GPGX_DIR}/input_hw/terebi_oekaki.c
    ${GPGX_DIR}/input_hw/xe_1ap.c

    # Mega CD stubs (required by genesis.h include chain)
    ${GPGX_DIR}/cd_hw/cdc.c
    ${GPGX_DIR}/cd_hw/cdd.c
    ${GPGX_DIR}/cd_hw/gfx.c
    ${GPGX_DIR}/cd_hw/pcm.c
    ${GPGX_DIR}/cd_hw/scd.c
    ${GPGX_DIR}/cd_hw/cd_cart.c

    # Our OSD implementation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/osd.c
)

target_include_directories(genplusgx_hw
    PUBLIC  ${GPGX_DIR}
    PUBLIC  ${GPGX_DIR}/m68k
    PUBLIC  ${GPGX_DIR}/z80
    PUBLIC  ${GPGX_DIR}/sound
    PUBLIC  ${GPGX_DIR}/cart_hw
    PUBLIC  ${GPGX_DIR}/cart_hw/svp
    PUBLIC  ${GPGX_DIR}/cd_hw
    PUBLIC  ${GPGX_DIR}/input_hw
    PUBLIC  ${GPGX_DIR}/ntsc
    PUBLIC  ${GPGX_DIR}/sound/minimp3
    # Our osd.h lives in src/
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src
    # genrecomp public headers (for osd.c -> input.h)
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(genplusgx_hw PRIVATE
    USE_32BPP_RENDERING
    LSB_FIRST
    HAVE_NO_SPRITE_LIMIT
)

# Seed bootstrap: GPGX's per-instruction CPU hook, used by genrecomp_trace to
# record every executed entry point. Off by default (recomp builds never run
# the interpreter, but keep the core untouched unless asked).
option(GENRECOMP_TRACE "Build genrecomp_trace (GPGX HOOK_CPU entry-point tracer)" OFF)
if(GENRECOMP_TRACE)
    target_sources(genplusgx_hw PRIVATE ${GPGX_DIR}/debug/cpuhook.c)
    target_compile_definitions(genplusgx_hw PUBLIC HOOK_CPU)
    target_include_directories(genplusgx_hw PUBLIC ${GPGX_DIR}/debug)
endif()

# GPGX uses libm (sin, pow, ...); MSVC's CRT has it built in
if(NOT WIN32)
    target_link_libraries(genplusgx_hw PUBLIC m)
endif()

# MSVC doesn't support __inline__ (GCC keyword) — remap to __inline
if(MSVC)
    target_compile_definitions(genplusgx_hw PRIVATE __inline__=__inline)
endif()

# --- genrecomp adapter library (static lib) ---
add_library(genrecomp STATIC
    src/genrecomp.c
    src/m68k.c
    src/bus.c
    src/func_table.c
    src/m68k_interp.c
    src/io.c
    src/platform_sdl.c
    src/input.c
    src/ps_vdp.c
    src/ps_render.c
    src/ps_png.c
    src/movie.c
    src/ps_audio.c
    src/ps_synth.c
    src/ps_poll.c
    src/ps_state.c
    src/ps_wide.c
    src/ps_menu.c
    src/ps_megacd.c
)
target_include_directories(genrecomp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${GPGX_DIR}
    PRIVATE ${GPGX_DIR}/m68k
    PRIVATE ${GPGX_DIR}/z80
    PRIVATE ${GPGX_DIR}/sound
    PRIVATE ${GPGX_DIR}/cart_hw
    PRIVATE ${GPGX_DIR}/cart_hw/svp
    PRIVATE ${GPGX_DIR}/cd_hw
    PRIVATE ${GPGX_DIR}/input_hw
    PRIVATE ${GPGX_DIR}/ntsc
)
target_compile_definitions(genrecomp PRIVATE
    USE_32BPP_RENDERING
    LSB_FIRST
    HAVE_NO_SPRITE_LIMIT
)
find_package(ZLIB REQUIRED)
target_link_libraries(genrecomp
    PUBLIC  genplusgx_hw
    PUBLIC  SDL2::SDL2
    PUBLIC  ZLIB::ZLIB
)
if(APPLE)
    enable_language(OBJC)
    target_sources(genrecomp PRIVATE src/present_metal.m)      # PS_PRESENTER=metal
    target_link_libraries(genrecomp PUBLIC "-framework Metal" "-framework QuartzCore" "-framework AppKit")
    target_link_libraries(genrecomp PUBLIC objc)   # CAMetalLayer drawable count (platform_sdl.c)
endif()

# --- Example: minimal recomp ---
if(GENRECOMP_BUILD_EXAMPLES)
    add_executable(genrecomp_minimal examples/minimal/main.c)
    target_link_libraries(genrecomp_minimal PRIVATE genrecomp SDL2::SDL2main)
endif()

# --- Runtime self-check (ctest) ---
enable_testing()
add_executable(genrecomp_test_runtime tests/test_runtime.c)
target_link_libraries(genrecomp_test_runtime PRIVATE genrecomp)
target_include_directories(genrecomp_test_runtime PRIVATE ${GPGX_DIR} ${GPGX_DIR}/m68k ${GPGX_DIR}/z80
    ${GPGX_DIR}/sound ${GPGX_DIR}/cart_hw ${GPGX_DIR}/cart_hw/svp ${GPGX_DIR}/cd_hw
    ${GPGX_DIR}/input_hw ${GPGX_DIR}/ntsc ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(genrecomp_test_runtime PRIVATE USE_32BPP_RENDERING LSB_FIRST HAVE_NO_SPRITE_LIMIT)
add_test(NAME runtime COMMAND genrecomp_test_runtime)

# --- Reference runner (GPGX interpreter, ground truth for diffing) ---
add_executable(genrecomp_ref tools/ref_runner/main.c)
target_link_libraries(genrecomp_ref PRIVATE genrecomp SDL2::SDL2main)
target_include_directories(genrecomp_ref PRIVATE ${GPGX_DIR} ${GPGX_DIR}/m68k ${GPGX_DIR}/z80
    ${GPGX_DIR}/sound ${GPGX_DIR}/cart_hw ${GPGX_DIR}/cart_hw/svp ${GPGX_DIR}/cd_hw
    ${GPGX_DIR}/input_hw ${GPGX_DIR}/ntsc ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(genrecomp_ref PRIVATE USE_32BPP_RENDERING LSB_FIRST HAVE_NO_SPRITE_LIMIT)

# --- Guided exploration (Go-Explore on the reference interpreter, hints when stuck) ---
add_executable(genrecomp_explore tools/explore/explore.c tools/explore/zstate.c)
find_package(ZLIB REQUIRED)
target_link_libraries(genrecomp_explore PRIVATE genrecomp SDL2::SDL2main ZLIB::ZLIB)
get_target_property(_ref_inc genrecomp_ref INCLUDE_DIRECTORIES)
target_include_directories(genrecomp_explore PRIVATE ${_ref_inc})
target_compile_definitions(genrecomp_explore PRIVATE USE_32BPP_RENDERING LSB_FIRST HAVE_NO_SPRITE_LIMIT)

# --- The original as a library for learning agents (tools/explore/ps_gym.py, Gymnasium) ---
add_library(ps_env SHARED tools/explore/ps_env.c)
# the static libraries go into a shared one: position-independent code (the default on macOS, not Linux)
set_target_properties(genrecomp genplusgx_hw PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ps_env PRIVATE genrecomp SDL2::SDL2)
target_include_directories(ps_env PRIVATE ${_ref_inc})
target_compile_definitions(ps_env PRIVATE USE_32BPP_RENDERING LSB_FIRST HAVE_NO_SPRITE_LIMIT)

# --- Entry-point tracer (seed bootstrap): -DGENRECOMP_TRACE=ON ---
if(GENRECOMP_TRACE)
    add_executable(genrecomp_trace tools/trace_runner/main.c)
    target_link_libraries(genrecomp_trace PRIVATE genrecomp SDL2::SDL2main)
    target_include_directories(genrecomp_trace PRIVATE ${GPGX_DIR} ${GPGX_DIR}/m68k ${GPGX_DIR}/z80
        ${GPGX_DIR}/sound ${GPGX_DIR}/cart_hw ${GPGX_DIR}/cart_hw/svp ${GPGX_DIR}/cd_hw
        ${GPGX_DIR}/input_hw ${GPGX_DIR}/ntsc ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(genrecomp_trace PRIVATE USE_32BPP_RENDERING LSB_FIRST HAVE_NO_SPRITE_LIMIT)
endif()
