cmake_minimum_required(VERSION 3.16)
project(shcd C)

# SHCD (Mega CD): the disc's programs load at run time (IPX, the per-stage Word RAM images); what is
# not compiled runs on the interpreter. shcd_native: PixelSorcery's own Mega CD (genrecomp_native, the
# shipped build); shcd: the same code on the Genesis Plus GX-backed runtime (reference, only when the
# core is present)
set(GENRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine" CACHE PATH "PixelSorcery engine")
add_subdirectory(${GENRECOMP_DIR} genrecomp_build)

# the interpreter's per-opcode cost table (the generator emits it with a game's compiled code):
# the documented 68000 timings, as generate.py's load_cycle_table - from the oracle core's table, or
# without the core (the apps' tree) from m68k_cycles.txt, the same numbers (make_dmg.sh)
set(CYC_H ${GENRECOMP_DIR}/ext/Genesis-Plus-GX/core/m68k/m68ki_cycles.h)
if(EXISTS ${CYC_H})
    file(READ ${CYC_H} CYC)
    string(REGEX REPLACE "^[^{]*{" "" CYC "${CYC}")
    string(REGEX REPLACE "}[^}]*$" "" CYC "${CYC}")
else()
    set(CYC_H ${GENRECOMP_DIR}/tools/recompiler/m68k_cycles.txt)
    file(READ ${CYC_H} CYC)
    string(STRIP "${CYC}" CYC)
    string(REGEX REPLACE "[ \n]+" "," CYC "${CYC}")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/cycles.c
    "#include <stdint.h>\nconst uint16_t ps_m68k_cycles[0x10000] = {${CYC}};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CYC_H})

# compiled stage programs, when generated from the player's disc (engine/tools/megacd/overlay_wrap.py):
# each is an overlay, used only while that program is the one in Word RAM
file(GLOB OVERLAYS CONFIGURE_DEPENDS src/recomp/ov_*.c)
if(TARGET genrecomp)   # absent when the engine is built without Genesis Plus GX
    add_executable(shcd src/main.c ${CMAKE_CURRENT_BINARY_DIR}/cycles.c ${OVERLAYS})
    target_include_directories(shcd PRIVATE ${GENRECOMP_DIR}/include)
    if(OVERLAYS)
        target_compile_definitions(shcd PRIVATE PS_OVERLAYS)
    endif()
    target_link_libraries(shcd PRIVATE genrecomp)
endif()

# the same disc on the native Mega CD (engine/src/mcd: no emulator core), with the same compiled programs
# the BIOS's and the disc's Z80 sound drivers, recompiled when captured (tools/z80_drivers.sh)
file(GLOB Z80_DRIVERS CONFIGURE_DEPENDS src/recomp/z80_*.c)
add_executable(shcd_native src/main.c ${CMAKE_CURRENT_BINARY_DIR}/cycles.c ${Z80_DRIVERS} ${OVERLAYS})
target_include_directories(shcd_native PRIVATE ${GENRECOMP_DIR}/include)
if(Z80_DRIVERS)
    target_compile_definitions(shcd_native PRIVATE PS_Z80_DRIVERS)
endif()
if(OVERLAYS)
    target_compile_definitions(shcd_native PRIVATE PS_OVERLAYS)
endif()
target_link_libraries(shcd_native PRIVATE genrecomp_native)
