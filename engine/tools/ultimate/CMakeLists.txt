# Ultimate SH, native: one executable holding several recompiled titles and the game-select screen
# (BACKLOG 0.7.5). Each title's generated code is compiled again with its fixed names renamed per title
# (gen_entry.py), partially linked into one object that exports only ID_entry, and linked with the
# engine once (genrecomp_multi: the title's tables bound when it starts).
#   cmake -S engine/tools/ultimate -B build/ultimate -G Ninja -DULTIMATE_TITLES="sh1;sh2"
cmake_minimum_required(VERSION 3.16)
project(ultimate_sh C)
set(PS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(ULTIMATE_GAMES_DIR "${PS_ROOT}/games" CACHE PATH "where the titles are (tests/: two stand-ins, CI)")
set(ULTIMATE_TITLES "sh1;shk1;sh2;shk2;sh3;shk;sh3k;shcd" CACHE STRING "games/ titles to hold")
add_subdirectory(${PS_ROOT}/engine genrecomp_build)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(RENAMES main ps_m68k_cycles ps_m68k_hook_pc ps_m68k_hook_fn ps_m68k_nhooks ps_z80_game_driver
    ps_z80_game_image ps_z80_game_image_len ps_cheat_names ps_cheat_count)
set(TITLE_OBJECTS "")
set(TITLE_LIST "")
foreach(T ${ULTIMATE_TITLES})
    set(G ${ULTIMATE_GAMES_DIR}/${T})
    file(GLOB RSRC CONFIGURE_DEPENDS ${G}/src/recomp/*.c)
    if(NOT RSRC)
        message(WARNING "${T}: src/recomp is empty (./play.sh ${T} generates it): left out")
        continue()
    endif()
    # a title whose generator did not emit the interpreter's cost table (SHCD) gets the documented
    # 68000 timings, as games/shcd/CMakeLists.txt
    set(EXTRA "")
    if(NOT EXISTS ${G}/src/recomp/recomp_cycles.c)
        set(EXTRA ${CMAKE_CURRENT_BINARY_DIR}/${T}_cycles.c)
        set(CYC_H ${PS_ROOT}/engine/ext/Genesis-Plus-GX/core/m68k/m68ki_cycles.h)
        if(EXISTS ${CYC_H})
            file(READ ${CYC_H} CYC)
            string(REGEX REPLACE "^[^{]*{" "" CYC "${CYC}")
            string(REGEX REPLACE "}[^}]*$" "" CYC "${CYC}")
        else()
            file(READ ${PS_ROOT}/engine/tools/recompiler/m68k_cycles.txt CYC)
            string(STRIP "${CYC}" CYC)
            string(REGEX REPLACE "[ \n]+" "," CYC "${CYC}")
        endif()
        file(CONFIGURE OUTPUT ${EXTRA} CONTENT "#include <stdint.h>\nconst uint16_t ps_m68k_cycles[0x10000] = {${CYC}};\n")
    endif()
    add_library(${T}_code OBJECT ${G}/src/main.c ${RSRC} ${EXTRA})
    target_include_directories(${T}_code PRIVATE ${G}/src ${PS_ROOT}/engine/include)
    target_link_libraries(${T}_code PRIVATE genrecomp_multi)   # include paths only
    file(GLOB OV ${G}/src/recomp/ov_*.c)
    file(GLOB ZD ${G}/src/recomp/z80_*.c)
    if(OV)
        target_compile_definitions(${T}_code PRIVATE PS_OVERLAYS)
    endif()
    if(ZD AND NOT EXISTS ${G}/src/recomp/z80_driver.c)    # SHCD's captured drivers (z80_drivers.c)
        target_compile_definitions(${T}_code PRIVATE PS_Z80_DRIVERS)
    endif()
    set(D "")
    foreach(N ${RENAMES})
        list(APPEND D "${N}=${T}__${N}")
    endforeach()
    target_compile_definitions(${T}_code PRIVATE ${D})
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_entry.py ${T} ${G}/src/recomp
                    ${CMAKE_CURRENT_BINARY_DIR}/${T}_entry.c ${EXTRA} COMMAND_ERROR_IS_FATAL ANY)
    add_library(${T}_entry OBJECT ${CMAKE_CURRENT_BINARY_DIR}/${T}_entry.c)
    target_include_directories(${T}_entry PRIVATE ${PS_ROOT}/engine/include)
    # one object, only ID_entry global: the title's names cannot meet another title's
    set(O ${CMAKE_CURRENT_BINARY_DIR}/${T}_all.o)
    if(APPLE)
        set(LINK_CMD ld -r -o ${O} -exported_symbol _${T}_entry $<TARGET_OBJECTS:${T}_code> $<TARGET_OBJECTS:${T}_entry>)
        set(HIDE_CMD true)
    else()
        set(LINK_CMD ${CMAKE_LINKER} -r -o ${O} $<TARGET_OBJECTS:${T}_code> $<TARGET_OBJECTS:${T}_entry>)
        set(HIDE_CMD ${CMAKE_OBJCOPY} --keep-global-symbol=${T}_entry ${O})
    endif()
    add_custom_command(OUTPUT ${O} COMMAND ${LINK_CMD} COMMAND ${HIDE_CMD}
                       DEPENDS ${T}_code ${T}_entry $<TARGET_OBJECTS:${T}_code> $<TARGET_OBJECTS:${T}_entry>
                       COMMAND_EXPAND_LISTS VERBATIM COMMENT "${T}: one object")
    set_source_files_properties(${O} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND TITLE_OBJECTS ${O})
    string(APPEND TITLE_LIST "PS_TITLE(${T})\n")
endforeach()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/titles.h CONTENT "${TITLE_LIST}")

# the game-select screen: the shelf launcher (its disc look), its main renamed
add_library(ultimate_screen OBJECT ${PS_ROOT}/engine/tools/launcher/launcher.c)
target_compile_definitions(ultimate_screen PRIVATE main=launcher_main SDL_MAIN_HANDLED)
target_link_libraries(ultimate_screen PRIVATE genrecomp_multi)   # SDL2 through it
add_executable(ultimate_sh main.c $<TARGET_OBJECTS:ultimate_screen> ${TITLE_OBJECTS})
configure_file(${PS_ROOT}/launchers/ultimate.txt ${CMAKE_CURRENT_BINARY_DIR}/ultimate.txt COPYONLY)
target_include_directories(ultimate_sh PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${PS_ROOT}/engine/include)
target_link_libraries(ultimate_sh PRIVATE genrecomp_multi)
