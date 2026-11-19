cmake_minimum_required(VERSION 3.16)
project(smb2_pc_port LANGUAGES C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Game integration: link the decompiled SMB2 code (i686 GCC, or x86_64 clang
# for the __ptr32 fields: cmake/llvm-mingw64.cmake, docs/x64.md)
option(SMB2_GAME "Link the decompiled game code" OFF)
# Default: the decomp checkout next to this one (../decomp)
get_filename_component(_smb2_default_decomp "${CMAKE_CURRENT_SOURCE_DIR}/../decomp" ABSOLUTE)
set(SMB2_DECOMP_DIR "${_smb2_default_decomp}" CACHE PATH "Path to the SMB2 decompilation (smb2-decomp checkout)")
if(SMB2_GAME AND NOT EXISTS "${SMB2_DECOMP_DIR}/src/main_loop")
    message(FATAL_ERROR "SMB2_DECOMP_DIR=${SMB2_DECOMP_DIR} is not a checkout of the SMB2 "
        "decompilation. Clone it next to this repository (../decomp) or pass "
        "-DSMB2_DECOMP_DIR=<path>.")
endif()

# Cross-compilation detection (pick deps matching the target architecture)
if(CMAKE_C_COMPILER MATCHES "i686")
    set(MINGW_DEPS_DIR ${CMAKE_SOURCE_DIR}/mingw_deps/i686-w64-mingw32)
else()
    set(MINGW_DEPS_DIR ${CMAKE_SOURCE_DIR}/mingw_deps/x86_64-w64-mingw32)
endif()
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(EXISTS ${MINGW_DEPS_DIR})
        message(STATUS "Using MinGW dependencies from: ${MINGW_DEPS_DIR}")
        set(CMAKE_FIND_ROOT_PATH ${MINGW_DEPS_DIR} ${CMAKE_FIND_ROOT_PATH})
        set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
        set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    endif()
endif()

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Wno-unused-parameter")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O0")
        if(NOT CMAKE_CROSSCOMPILING)
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
        endif()
    elseif(SMB2_GAME)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -flto")
    endif()
    # 32-bit x86 defaults to x87 math, which keeps intermediates in 80-bit
    # registers.  The GameCube rounds every op to double (fmul) or single
    # (fmuls), and game code depends on it: e.g. boat's race start tests
    # `frames == 0.1 * 60.0f`, which is exactly 6.0 in double but not in
    # x87 extended precision.  SSE math rounds each op to its C type, like
    # the PPC.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i[3-6]86")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse2 -mfpmath=sse")
    endif()
    # Bit-identical float results on every build (docs/detmath.md): never
    # fuse a*b+c into an FMA (GCC's default is -ffp-contract=fast, clang's
    # "on"; x86 baseline has no FMA, but ARM64 and -march=native builds do).
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffp-contract=off")
endif()

if(MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4 /WX-")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /Zi /Od /MDd")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /O2 /MD")
    endif()
endif()

# Find dependencies - try find_package first, fall back to sdl2-config
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_program(SDL2_CONFIG sdl2-config)
    if(SDL2_CONFIG)
        execute_process(COMMAND ${SDL2_CONFIG} --cflags OUTPUT_VARIABLE SDL2_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${SDL2_CONFIG} --libs OUTPUT_VARIABLE SDL2_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(SDL2_INCLUDE_DIRS ${SDL2_CFLAGS})
        set(SDL2_LIBRARIES ${SDL2_LIBS})
        set(SDL2_FOUND TRUE)
    endif()
endif()

# For cross-compilation, also check MinGW deps
if(CMAKE_CROSSCOMPILING AND NOT SDL2_FOUND AND EXISTS ${MINGW_DEPS_DIR})
    find_path(SDL2_INCLUDE_DIR SDL.h PATHS ${MINGW_DEPS_DIR}/include NO_DEFAULT_PATH)
    find_library(SDL2_LIBRARY SDL2 PATHS ${MINGW_DEPS_DIR}/lib NO_DEFAULT_PATH)
    find_library(SDL2MAIN_LIBRARY SDL2main PATHS ${MINGW_DEPS_DIR}/lib NO_DEFAULT_PATH)
    if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY AND SDL2MAIN_LIBRARY)
        set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
        set(SDL2_LIBRARIES ${SDL2_LIBRARY} ${SDL2MAIN_LIBRARY})
        set(SDL2_FOUND TRUE)
        message(STATUS "Found SDL2 in MinGW deps")
    endif()
endif()

if(NOT SDL2_FOUND)
    message(FATAL_ERROR "SDL2 not found. Please install libsdl2-dev")
endif()

# Find OpenGL
find_package(OpenGL REQUIRED)

# Find miniaudio (header-only, but we can check for it)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h
    PATHS ${CMAKE_SOURCE_DIR}/third_party/miniaudio
    NO_DEFAULT_PATH
)

if(NOT MINIAUDIO_INCLUDE_DIR)
    message(STATUS "miniaudio not found, will use embedded version")
    set(MINIAUDIO_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${SDL2_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
    ${MINIAUDIO_INCLUDE_DIR}
)

# Source files
set(SOURCES
    src/platform/platform_sdl2.c
    src/platform/pc_settings.c
    src/platform/pc_text.c
    src/platform/save.c
    src/input/input_sdl2.c
    src/input/gc_stick.c
    src/input/pad_config.c
    src/audio/audio_miniaudio.c
    src/render/render_opengl.c
    src/render/gx_core.c
    src/render/gx_hfr.c
    src/render/render_d3d12.c
    src/assets/assets.c
    src/assets/iso.c
    src/assets/gamedata.c
    src/assets/disc_icon.c
    src/platform/install_mode.c
    src/platform/pc_update.c
    src/platform/pc_file_dialog.c
    src/main/main.c
)

# Version shown in the exe's VERSIONINFO (installer/build.sh passes its --version)
set(SMB2_VERSION "0.0.0-dev" CACHE STRING "Version string for the Windows VERSIONINFO resource")
# In-game updater (docs/updater.md): GitHub repo "owner/name" whose Releases
# are checked; empty = compiled in but disabled
set(SMB2_UPDATE_REPO "" CACHE STRING "GitHub owner/name checked for updates (empty = updater disabled)")
if(NOT SMB2_UPDATE_REPO STREQUAL "" AND NOT SMB2_UPDATE_REPO MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "SMB2_UPDATE_REPO must be owner/name, got '${SMB2_UPDATE_REPO}'")
endif()
if(NOT SMB2_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-+][0-9A-Za-z.+-]*)?$")
    message(FATAL_ERROR "SMB2_VERSION must look like 1.2.3 or 1.2.3-rc1, got '${SMB2_VERSION}'")
endif()
# only pc_update.c sees them, so a new version rebuilds one file
set_source_files_properties(src/platform/pc_update.c PROPERTIES COMPILE_DEFINITIONS
    "SMB2_VERSION_STR=\"${SMB2_VERSION}\";SMB2_UPDATE_REPO_STR=\"${SMB2_UPDATE_REPO}\"")

# Windows: VERSIONINFO (FileDescription/ProductName "SMB2") + the generic
# Explorer icon.  Both the i686 (windres) and x86_64 (llvm-mingw windres)
# toolchains set CMAKE_RC_COMPILER.
if(WIN32)
    enable_language(RC)
    string(REGEX MATCHALL "[0-9]+" _smb2_ver_nums "${SMB2_VERSION}")
    list(APPEND _smb2_ver_nums 0 0 0 0)
    list(SUBLIST _smb2_ver_nums 0 4 _smb2_ver_nums)
    string(REPLACE ";" "," SMB2_VER_COMMA "${_smb2_ver_nums}")
    set(SMB2_RC_ICON "${CMAKE_SOURCE_DIR}/installer/smb2.ico")
    configure_file(${CMAKE_SOURCE_DIR}/src/res/smb2_pc_port.rc.in
                   ${CMAKE_BINARY_DIR}/smb2_pc_port.rc @ONLY)
    list(APPEND SOURCES ${CMAKE_BINARY_DIR}/smb2_pc_port.rc)
    set_property(SOURCE ${CMAKE_BINARY_DIR}/smb2_pc_port.rc APPEND PROPERTY
                 OBJECT_DEPENDS ${SMB2_RC_ICON})
endif()

# Create executable
add_executable(smb2_pc_port ${SOURCES})

# Deterministic libm/rand/qsort for game code (docs/detmath.md): fdlibm +
# the GC MSL's rand/qsort, so every compiler/CRT/CPU gets the same bits.
# include/pc_detmath.h maps the game's calls to it.
file(GLOB DETMATH_SOURCES ${CMAKE_SOURCE_DIR}/src/detmath/*.c
                          ${CMAKE_SOURCE_DIR}/src/detmath/fdlibm/*.c)
add_library(smb2_detmath STATIC ${DETMATH_SOURCES})
target_compile_options(smb2_detmath PRIVATE -fno-builtin -fno-math-errno)
target_link_libraries(smb2_pc_port smb2_detmath)
# gx_core.c builds the projection/light matrices the game reads back
set_source_files_properties(src/render/gx_core.c PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_SOURCE_DIR}/include/pc_detmath.h")

# ---------------------------------------------------------------------------
# Decompiled game + GC SDK shim layer
# ---------------------------------------------------------------------------
if(SMB2_GAME)
    file(GLOB GAME_SOURCES
        ${SMB2_DECOMP_DIR}/src/main_loop/*.c
        ${SMB2_DECOMP_DIR}/src/main_game/*.c
        ${SMB2_DECOMP_DIR}/src/sel_stage/*.c
        ${SMB2_DECOMP_DIR}/src/exoption/*.c
        ${SMB2_DECOMP_DIR}/src/race2/*.c
        ${SMB2_DECOMP_DIR}/src/pilot2/*.c
        ${SMB2_DECOMP_DIR}/src/rel_mini_bowling/*.c
        ${SMB2_DECOMP_DIR}/src/boat/*.c
        ${SMB2_DECOMP_DIR}/src/dogfight/*.c
        ${SMB2_DECOMP_DIR}/src/baseball/*.c
        ${SMB2_DECOMP_DIR}/src/golf2/*.c
        ${SMB2_DECOMP_DIR}/src/fight/*.c
        ${SMB2_DECOMP_DIR}/src/sel_ngc/*.c
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "The 64-bit game build needs clang (__ptr32 DPTR fields, "
            "docs/x64.md): use -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw64.cmake "
            "-DLLVM_MINGW_ROOT=<unpacked llvm-mingw>")
    endif()
    # Text of the decomp TUs (tools/pc_decomp_text.py, docs/language.md):
    #  - Shift-JIS: the GC build compiles the decomp through sjiswrap (UTF-8
    #    source -> code page 932), so its Japanese literals are SJIS bytes,
    #    the encoding the game's font code reads; GCC/Clang would keep UTF-8.
    #  - the US disc's missing German/French/Spanish/Italian table entries
    #    ("", "--", "NOCARD GER" ...) get the table's English text.
    # The TUs (and .inc files) this changes are compiled from rewritten copies
    # (same lines; SJIS bytes identical to the GC objects).
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    file(GLOB_RECURSE SMB2_DECOMP_INCS ${SMB2_DECOMP_DIR}/include/*.inc)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/pc_decomp_text.py --list
                ${GAME_SOURCES} ${SMB2_DECOMP_INCS}
        OUTPUT_VARIABLE SMB2_TEXT_FILES
        RESULT_VARIABLE SMB2_TEXT_RES
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT SMB2_TEXT_RES EQUAL 0)
        message(FATAL_ERROR "tools/pc_decomp_text.py --list failed")
    endif()
    string(REPLACE "\n" ";" SMB2_TEXT_FILES "${SMB2_TEXT_FILES}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${GAME_SOURCES} ${SMB2_DECOMP_INCS})
    set(SMB2_TEXT_DIR ${CMAKE_BINARY_DIR}/pctext)
    set(SMB2_TEXT_SOURCES "")
    set(SMB2_TEXT_INCS "")
    foreach(f ${SMB2_TEXT_FILES})
        file(RELATIVE_PATH rel ${SMB2_DECOMP_DIR} ${f})
        if(rel MATCHES "^include/")
            string(REGEX REPLACE "^include/" "" relinc ${rel})
            set(out ${SMB2_TEXT_DIR}/include/${relinc})
            list(APPEND SMB2_TEXT_INCS ${out})
        else()
            set(out ${SMB2_TEXT_DIR}/${rel})
            list(TRANSFORM GAME_SOURCES REPLACE "^${f}$" "${out}")
            list(APPEND SMB2_TEXT_SOURCES ${f})
        endif()
        get_filename_component(outdir ${out} DIRECTORY)
        add_custom_command(OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${outdir}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/pc_decomp_text.py ${f} ${out}
            DEPENDS ${f} ${CMAKE_SOURCE_DIR}/tools/pc_decomp_text.py
            COMMENT "PC text: ${rel}")
    endforeach()
    add_custom_target(smb2_text_incs DEPENDS ${SMB2_TEXT_INCS})
    add_library(smb2_game STATIC ${GAME_SOURCES})
    add_dependencies(smb2_game smb2_text_incs)
    # Pointer-size audit: instead of -w, report only pointer<->int
    # size-mismatch casts in the game TUs.  See docs/x64.md.
    option(SMB2_PTR_WARNINGS "Warn on pointer/int size-mismatch casts in game code" OFF)
    if(SMB2_PTR_WARNINGS)
        # (noisy: grep the log for the two warning tags)
        set(SMB2_GAME_WARN_FLAGS -Wpointer-to-int-cast -Wint-to-pointer-cast)
    else()
        set(SMB2_GAME_WARN_FLAGS -w)
    endif()
    target_include_directories(smb2_game BEFORE PRIVATE
        ${SMB2_TEXT_DIR}/include
        ${SMB2_DECOMP_DIR}/include
        ${SMB2_DECOMP_DIR}/build/GM2E8P/include
    )
    target_compile_options(smb2_game PRIVATE
        -std=gnu99 -fno-builtin -fcommon -fno-strict-aliasing -fwrapv
        -Wno-unknown-pragmas ${SMB2_GAME_WARN_FLAGS}
        # MWCC intrinsics (__fabs etc.) are used bare in the decomp; give
        # every TU real prototypes + inline impls.
        -include ${CMAKE_CURRENT_SOURCE_DIR}/src/game/shim/gc_intrinsics.h
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        # MWCC/GCC-style "inline" without extern definitions (mini_ranking.c)
        target_compile_options(smb2_game PRIVATE -fgnu89-inline)
    endif()

    # TUs built with "#pragma unsigned_char" on GC
    set_source_files_properties(
        ${SMB2_DECOMP_DIR}/src/main_loop/dialog.c
        ${SMB2_DECOMP_DIR}/src/main_loop/textdraw.c
        ${SMB2_DECOMP_DIR}/src/main_loop/textbox.c
        ${SMB2_DECOMP_DIR}/src/main_loop/sprite.c
        PROPERTIES COMPILE_OPTIONS "-funsigned-char"
    )
    # Each REL has its own _prolog/_epilog/_unresolved; rename all but main_loop's
    file(GLOB MAIN_GAME_SOURCES ${SMB2_DECOMP_DIR}/src/main_game/*.c)
    file(GLOB SEL_STAGE_SOURCES ${SMB2_DECOMP_DIR}/src/sel_stage/*.c)
    file(GLOB EXOPTION_SOURCES ${SMB2_DECOMP_DIR}/src/exoption/*.c)
    set_source_files_properties(${MAIN_GAME_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=mainGamePrologue;_epilog=mainGameEpilogue;_unresolved=mainGameUnresolved")
    set_source_files_properties(${SEL_STAGE_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=selStagePrologue;_epilog=selStageEpilogue;_unresolved=selStageUnresolved")
    set_source_files_properties(${EXOPTION_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=exOptionPrologue;_epilog=exOptionEpilogue;_unresolved=exOptionUnresolved")

    file(GLOB MINI_RACE2_SOURCES ${SMB2_DECOMP_DIR}/src/race2/*.c)
    set_source_files_properties(${MINI_RACE2_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=race2Prologue;_epilog=race2Epilogue;_unresolved=race2Unresolved")
    file(GLOB MINI_PILOT2_SOURCES ${SMB2_DECOMP_DIR}/src/pilot2/*.c)
    set_source_files_properties(${MINI_PILOT2_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=pilot2Prologue;_epilog=pilot2Epilogue;_unresolved=pilot2Unresolved")
    file(GLOB MINI_BOWLING_SOURCES ${SMB2_DECOMP_DIR}/src/rel_mini_bowling/*.c)
    set_source_files_properties(${MINI_BOWLING_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=bowlingPrologue;_epilog=bowlingEpilogue;_unresolved=bowlingUnresolved")
    file(GLOB MINI_BOAT_SOURCES ${SMB2_DECOMP_DIR}/src/boat/*.c)
    set_source_files_properties(${MINI_BOAT_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=boatPrologue;_epilog=boatEpilogue;_unresolved=boatUnresolved")
    file(GLOB MINI_DOGFIGHT_SOURCES ${SMB2_DECOMP_DIR}/src/dogfight/*.c)
    set_source_files_properties(${MINI_DOGFIGHT_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=dogfightPrologue;_epilog=dogfightEpilogue;_unresolved=dogfightUnresolved")
    file(GLOB MINI_BASEBALL_SOURCES ${SMB2_DECOMP_DIR}/src/baseball/*.c)
    set_source_files_properties(${MINI_BASEBALL_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=baseballPrologue;_epilog=baseballEpilogue;_unresolved=baseballUnresolved")
    file(GLOB MINI_GOLF_SOURCES ${SMB2_DECOMP_DIR}/src/golf2/*.c)
    set_source_files_properties(${MINI_GOLF_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=golfPrologue;_epilog=golfEpilogue;_unresolved=golfUnresolved")
    file(GLOB MINI_FIGHT_SOURCES ${SMB2_DECOMP_DIR}/src/fight/*.c)
    set_source_files_properties(${MINI_FIGHT_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=fightPrologue;_epilog=fightEpilogue;_unresolved=fightUnresolved")

    file(GLOB SEL_NGC_SOURCES ${SMB2_DECOMP_DIR}/src/sel_ngc/*.c)
    set_source_files_properties(${SEL_NGC_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_prolog=selNgcPrologue;_epilog=selNgcEpilogue;_unresolved=selNgcUnresolved")

    # rewritten copies: same per-file flags as the original, and its directory
    # for #include "..." of neighbouring files
    foreach(f ${SMB2_TEXT_SOURCES})
        file(RELATIVE_PATH rel ${SMB2_DECOMP_DIR} ${f})
        get_filename_component(srcdir ${f} DIRECTORY)
        get_source_file_property(defs ${f} COMPILE_DEFINITIONS)
        get_source_file_property(opts ${f} COMPILE_OPTIONS)
        if(NOT opts)
            set(opts "")
        endif()
        if(defs)
            set_source_files_properties(${SMB2_TEXT_DIR}/${rel} PROPERTIES COMPILE_DEFINITIONS "${defs}")
        endif()
        set_source_files_properties(${SMB2_TEXT_DIR}/${rel} PROPERTIES
            COMPILE_OPTIONS "${opts};-iquote;${srcdir}")
    endforeach()

    file(GLOB SHIM_SOURCES ${CMAKE_SOURCE_DIR}/src/game/shim/*.c)
    add_library(smb2_shim STATIC ${SHIM_SOURCES})
    target_include_directories(smb2_shim BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/src/game/shim
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${SMB2_DECOMP_DIR}/include
        ${SMB2_DECOMP_DIR}/build/GM2E8P/include
    )
    target_compile_options(smb2_shim PRIVATE -Wno-unused-parameter)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        # x64: DPTR (__ptr32) fields in disc-layout structs, docs/x64.md
        foreach(t smb2_game smb2_shim smb2_pc_port)
            target_compile_options(${t} PRIVATE -fms-extensions
                "SHELL:-include ${CMAKE_SOURCE_DIR}/include/pc_ptr32.h")
        endforeach()
    endif()

    # Test build: report out-of-bounds array indexing and accesses past the
    # end of a known object (the "GC globals are adjacent" overrun class:
    # GC code indexing past one array into the next symbol).  Recover mode, handlers in pc_ubsan_rt.c log
    # "[oob]" lines to stderr; no sanitizer runtime is linked.
    option(SMB2_SANITIZE_BOUNDS "Instrument game code with array-bounds/object-size checks (test builds)" OFF)
    if(SMB2_SANITIZE_BOUNDS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            set(SMB2_SAN_FLAGS -fsanitize=array-bounds,object-size)
        else()
            set(SMB2_SAN_FLAGS -fsanitize=bounds,object-size)
        endif()
        # only [] / [0] / [1] trailing members count as flexible arrays
        list(APPEND SMB2_SAN_FLAGS -fsanitize-recover=all -fstrict-flex-arrays=1)
        foreach(t smb2_game smb2_shim)
            target_compile_options(${t} PRIVATE ${SMB2_SAN_FLAGS})
            target_compile_definitions(${t} PRIVATE SMB2_SANITIZE_BOUNDS)
        endforeach()
        set_source_files_properties(${CMAKE_SOURCE_DIR}/src/game/shim/pc_ubsan_rt.c
            PROPERTIES COMPILE_OPTIONS "-fno-sanitize=all")
        if(CMAKE_CROSSCOMPILING AND CMAKE_C_COMPILER MATCHES "i686")
            # fixed load address so reported pointers match nm output
            target_link_options(smb2_pc_port PRIVATE -Wl,--disable-dynamicbase)
        endif()
    endif()

    # game <-> shim reference each other; CMake repeats cyclic static libs
    target_link_libraries(smb2_game smb2_shim)
    target_link_libraries(smb2_shim smb2_game)
    target_link_libraries(smb2_pc_port smb2_game)

    target_compile_definitions(smb2_pc_port PRIVATE SMB2_LINK_GAME)

    # Game code: libm/rand/srand/qsort -> deterministic pc_* versions
    # (docs/detmath.md)
    foreach(t smb2_game smb2_shim)
        target_compile_options(${t} PRIVATE
            "SHELL:-include ${CMAKE_SOURCE_DIR}/include/pc_detmath.h")
        target_link_libraries(${t} smb2_detmath)
    endforeach()

    if(CMAKE_CROSSCOMPILING AND CMAKE_C_COMPILER MATCHES "i686")
        # Map GC RAM at its real address 0x80000000 (needs LAA) + big stack
        target_link_options(smb2_pc_port PRIVATE
            -Wl,--large-address-aware -Wl,--stack,16777216)
    elseif(CMAKE_CROSSCOMPILING AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        # x64: keep the image (and so every game static) below 2 GiB at a
        # fixed address, with no high-entropy ASLR, so 32-bit pointer
        # values the game keeps in u32/s32 fields stay valid.  GC RAM is
        # mapped at 0x80000000 by gc_mem.c.  See docs/x64.md.
        target_link_options(smb2_pc_port PRIVATE
            -Wl,--image-base,0x10000000
            -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va
            -Wl,--stack,16777216)
    endif()
endif()

# Link libraries
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Link SDL2 statically so the exe runs without SDL2.dll beside it
    if(EXISTS ${MINGW_DEPS_DIR}/lib/libSDL2.a)
        set(SDL2_LIBRARIES
            mingw32
            ${MINGW_DEPS_DIR}/lib/libSDL2main.a
            ${MINGW_DEPS_DIR}/lib/libSDL2.a
        )
    endif()
    target_link_libraries(smb2_pc_port
        ${SDL2_LIBRARIES}
        opengl32
        m kernel32 user32 gdi32 winmm imm32 ole32 oleaut32
        version uuid advapi32 setupapi shell32 comdlg32 dinput8 winhttp
    )
else()
    target_link_libraries(smb2_pc_port
        ${SDL2_LIBRARIES}
        ${OPENGL_LIBRARIES}
        ${CMAKE_DL_LIBS}
        m
        pthread
        z
    )
endif()

# Target compile definitions
target_compile_definitions(smb2_pc_port PRIVATE
    _GNU_SOURCE
    SDL_MAIN_HANDLED
)
# Direct3D 12 backend (Windows): d3d12/dxgi/d3dcompiler are loaded at runtime
if(WIN32)
    target_compile_definitions(smb2_pc_port PRIVATE SMB2_HAVE_D3D12)
endif()

# Windows cross-compilation needs main=SDL_main
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(smb2_pc_port PRIVATE main=SDL_main)
endif()

# Copy assets directory on build
add_custom_command(TARGET smb2_pc_port POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:smb2_pc_port>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:smb2_pc_port>/assets
    COMMENT "Copying assets to build directory"
)

# Install target
install(TARGETS smb2_pc_port
    RUNTIME DESTINATION bin
)

# CPack for packaging
set(CPACK_PACKAGE_NAME "SMB2")
set(CPACK_PACKAGE_VERSION "${SMB2_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION "Unofficial PC port (needs your own GameCube disc image)")
set(CPACK_GENERATOR "ZIP")
include(CPack)

# Testing
enable_testing()
add_test(NAME smb2_pc_port_test COMMAND smb2_pc_port --help)