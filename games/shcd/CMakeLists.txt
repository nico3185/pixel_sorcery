cmake_minimum_required(VERSION 3.16)
project(shcd C)

# SHCD (Mega CD): the disc's programs load at run time (IPX, the per-stage Word RAM images);
# what is not compiled runs on the interpreter of the
# Genesis Plus GX-backed runtime, which also runs the Mega CD (sub 68000, CDD, PCM, CD audio)
set(GENRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine" CACHE PATH "PixelSorcery engine")
add_subdirectory(${GENRECOMP_DIR} genrecomp_build)

# the interpreter's per-opcode cost table (the generator emits it with a game's compiled code):
# the documented 68000 timings, from the oracle core's table, as generate.py's load_cycle_table
file(READ ${GENRECOMP_DIR}/ext/Genesis-Plus-GX/core/m68k/m68ki_cycles.h CYC)
string(REGEX REPLACE "^[^{]*{" "" CYC "${CYC}")
string(REGEX REPLACE "}[^}]*$" "" CYC "${CYC}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cycles.c
    "#include <stdint.h>\nconst uint16_t ps_m68k_cycles[0x10000] = {${CYC}};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${GENRECOMP_DIR}/ext/Genesis-Plus-GX/core/m68k/m68ki_cycles.h)

# compiled stage programs, when generated from the player's disc (engine/tools/megacd/overlay_wrap.py):
# each is an overlay, used only while that program is the one in Word RAM
file(GLOB OVERLAYS CONFIGURE_DEPENDS src/recomp/ov_*.c)
add_executable(shcd src/main.c ${CMAKE_CURRENT_BINARY_DIR}/cycles.c ${OVERLAYS})
target_include_directories(shcd PRIVATE ${GENRECOMP_DIR}/include)
if(OVERLAYS)
    target_compile_definitions(shcd PRIVATE PS_OVERLAYS)
endif()
target_link_libraries(shcd PRIVATE genrecomp)
