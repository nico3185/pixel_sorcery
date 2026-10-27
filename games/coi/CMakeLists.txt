cmake_minimum_required(VERSION 3.16)
project(coi C)

# genrecomp: the Genesis hardware runtime and recompiler, beside this repo
set(GENRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine" CACHE PATH "PixelSorcery engine")
if(NOT EXISTS "${GENRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "genrecomp not found at ${GENRECOMP_DIR}. Run Setup.cmd, or clone it there.")
endif()
add_subdirectory(${GENRECOMP_DIR} genrecomp_build)

# src/recomp is generated from your own ROM and never committed (README,
# Getting Started): genrecomp's tools/recompiler/generate.py with recomp.json
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS src/recomp/*.c)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR "src/recomp/ is empty. Generate it from your ROM first:\n"
        "  python ../genrecomp/tools/recompiler/generate.py \"path/to/General Chaos.gen\" -o src/recomp -c recomp.json\n"
        "or run Setup.cmd, which does every step.")
endif()

# Recompiled game code, compiled once and linked into both runtimes
add_library(coi_code OBJECT src/main.c ${RECOMP_SOURCES})
target_include_directories(coi_code PRIVATE src/ ${GENRECOMP_DIR}/include)
target_link_libraries(coi_code PRIVATE genrecomp_native)   # include paths only

# coi: the native port (no emulator code linked)
add_executable(coi $<TARGET_OBJECTS:coi_code>)
target_link_libraries(coi PRIVATE genrecomp_native)

# coi_oracle: same game code on the Genesis Plus GX-backed runtime, for the in-process
# compare modes (PS_RENDERER=compare, PS_AUDIO=compare) used by recomp.sh
add_executable(coi_oracle $<TARGET_OBJECTS:coi_code>)
target_link_libraries(coi_oracle PRIVATE genrecomp)
