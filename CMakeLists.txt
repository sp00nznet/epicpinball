# Epic Pinball, statically recompiled.
#
# One executable, epicpinball.exe: PINBALL.EXE, the thirteen tables and the
# MASI sound drivers, each lifted to C by tools/lift.py into work/gen/<image>/,
# linked over one DOS machine (src/). The lifted C is generated from your own
# copy of the game and is never committed; see README.
#
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   py tools/discover.py        # lift, build, play, repeat until settled
cmake_minimum_required(VERSION 3.20)
project(epicpinball C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(EPIC_TRACE "record lifted calls and enable EPIC_WATCH (slow)" OFF)

# pcrecomp supplies the CPU model (runtime/recomp16/cpu.h) the lifted C uses.
set(PCRECOMP_HOME "$ENV{PCRECOMP_HOME}" CACHE PATH "pcrecomp checkout")
if(NOT PCRECOMP_HOME)
    foreach(cand ../pcrecomp ../pcrecomp-main)
        if(EXISTS "${CMAKE_SOURCE_DIR}/${cand}/runtime/recomp16/cpu.h")
            get_filename_component(PCRECOMP_HOME "${CMAKE_SOURCE_DIR}/${cand}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${PCRECOMP_HOME}/runtime/recomp16/cpu.h")
    message(FATAL_ERROR "pcrecomp not found: set PCRECOMP_HOME to a checkout of https://github.com/sp00nznet/pcrecomp")
endif()

if(MSVC)
    add_compile_options(/W1 /wd4996 /wd4244 /wd4146 /wd4102 /wd4101 /wd4267 /wd4018 /wd4305 /bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_link_options(/STACK:16777216)   # lifted call chains are deep, and EXEC nests a whole program
endif()

set(GEN "${CMAKE_SOURCE_DIR}/work/gen")
set(RUNTIME src/main.c src/machine.c src/dos.c src/images.c src/vga.c src/audio.c src/host.c src/present.c src/pad.c)

if(EXISTS "${GEN}/images_list.c")
    # Each image is its own library: its <image>.h defines SEG_ constants that
    # must not leak into another image's C. The globs are read at configure
    # time, so re-run `cmake -B build` after a lift (tools/discover.py does);
    # CONFIGURE_DEPENDS would, but races under parallel MSBuild.
    file(GLOB tables "${GEN}/*/*_table.c")
    set(image_libs)
    foreach(table ${tables})
        get_filename_component(dir "${table}" DIRECTORY)
        get_filename_component(img "${dir}" NAME)
        file(GLOB lifted "${dir}/*.c")
        add_library(img_${img} OBJECT ${lifted})
        target_include_directories(img_${img} PRIVATE src "${dir}" "${PCRECOMP_HOME}/runtime")
        list(APPEND image_libs $<TARGET_OBJECTS:img_${img}>)
        if(EPIC_TRACE)
            target_compile_definitions(img_${img} PRIVATE RECOMP_TRACE)
            # /Od: the optimizer spends minutes on the big lifted functions,
            # and a trace build is for reading stacks, not for speed
            target_compile_options(img_${img} PRIVATE /Zi /Od /Ob0)
        endif()
    endforeach()
    add_executable(epicpinball ${RUNTIME} "${GEN}/images_list.c" ${image_libs})
    target_include_directories(epicpinball PRIVATE src "${GEN}" "${PCRECOMP_HOME}/runtime")
    target_link_libraries(epicpinball PRIVATE user32 gdi32 winmm shell32 d3d11 dxgi dxguid)
    if(EPIC_TRACE)
        target_compile_definitions(epicpinball PRIVATE RECOMP_TRACE EPIC_DEBUG)
        target_compile_options(epicpinball PRIVATE /Zi /Oy-)
        target_link_options(epicpinball PRIVATE /DEBUG)
    endif()
else()
    message(STATUS "epicpinball: no lifted code in work/gen -- run tools/lift.py")
endif()

# The runtime's own pieces on their own, no game data needed: what CI runs.
add_executable(selftest tests/selftest.c src/vga.c src/audio.c src/images.c)
target_include_directories(selftest PRIVATE src "${PCRECOMP_HOME}/runtime")
