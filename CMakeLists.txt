cmake_minimum_required(VERSION 3.15)
project(robox_recomp C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# Robox (WiiWare) recompilation. Derived from templates/CMakeLists.txt.tmpl.
#
# Build (Windows + MSYS2 mingw64):
#     set PATH=C:\msys64\mingw64\bin;%PATH%
#     cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
#     cmake --build build -j

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Nintendo 3DS target detection.
#
# The devkitPro 3DS.cmake toolchain sets CMAKE_SYSTEM_NAME=Nintendo3DS. When it
# is active we are cross-compiling for the ARM11 (ARMv6k, little-endian, VFPv2).
# The renderer/window/audio layer (video.c, gx_ogl.c, robox_splash.c,
# robox_menu.c) is unavailable there -- PICA200 has no OpenGL -- so those files
# body-guard themselves out on __3DS__ and platform_3ds.c + gx_c3d.c stand in.
# SDL2 (the devkitPro portlibs prebuilt, 2.28.5) is linked ONLY for os.c's
# OSThread->SDL threading; graphics go through citro3d and input through hid.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Nintendo3DS")
    set(ROBOX_3DS ON)
    message(STATUS "robox: targeting Nintendo 3DS (devkitARM/libctru/citro3d)")
endif()

# ---------------------------------------------------------------------------
# Platform ports.
#
# Android/iOS, web and 3DS each live in their own repository. What stayed here
# is the conditional code: the #if defined(__ANDROID__) / __EMSCRIPTEN__ /
# __3DS__ blocks are woven through video.c, gx_ogl.c, main.c and others, and
# they compile to nothing on targets that are not them.
#
# What moved out is each port's packaging and the source files that are ONLY
# that platform -- android_bootstrap.c, robox_web_debug.c, gx_c3d.c and
# platform_3ds.c. Every one of those is wholly wrapped in its own #if, so on
# desktop they were empty translation units and removing them changed nothing.
#
# Point this at a port repository's platform/ directory to compile them back
# in. Left empty (the default) this is a desktop build and none are needed.
#
#     cmake -S runtime -B build-3ds -DROBOX_PLATFORM_DIR=/path/to/port/platform
#
# The web target additionally needs its shell and mods file, which are real
# build inputs rather than packaging:
#
#     -DROBOX_WEB_SHELL=/path/to/web_shell.html
#     -DROBOX_WEB_MODS=/path/to/web_mods.cfg
# ---------------------------------------------------------------------------
set(ROBOX_PLATFORM_DIR "" CACHE PATH
    "platform/ directory of a port repository (mobile, web or 3ds)")
set(ROBOX_WEB_SHELL "" CACHE FILEPATH "web_shell.html, from the web port")
set(ROBOX_WEB_MODS  "" CACHE FILEPATH "web_mods.cfg, from the web port")

set(ROBOX_PORT_SOURCES "")
if(ROBOX_PLATFORM_DIR)
    file(GLOB ROBOX_PORT_SOURCES CONFIGURE_DEPENDS "${ROBOX_PLATFORM_DIR}/*.c")
    if(NOT ROBOX_PORT_SOURCES)
        message(FATAL_ERROR "ROBOX_PLATFORM_DIR has no .c files: ${ROBOX_PLATFORM_DIR}")
    endif()
    message(STATUS "robox: platform sources from ${ROBOX_PLATFORM_DIR}")
endif()

if(EMSCRIPTEN AND NOT ROBOX_WEB_SHELL)
    message(FATAL_ERROR
        "The web target needs -DROBOX_WEB_SHELL=<web_shell.html> and "
        "-DROBOX_WEB_MODS=<web_mods.cfg>, both from the web port repository.")
endif()

set(WARN_FLAGS
    -Wno-parentheses
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-label
    -Wno-unused-function
    -Wno-implicit-fallthrough
    -Wno-overflow
    -Wno-pointer-to-int-cast
    -Wno-int-to-pointer-cast
    -Wno-shift-count-overflow
    -Wno-sign-compare
    -Wno-missing-braces
    -Wno-missing-field-initializers
    -Wno-type-limits
    -Wno-error=implicit-function-declaration
    -Wno-error=incompatible-pointer-types
    -Wno-builtin-declaration-mismatch
)

# The overlay/menu font is extracted from the retail font archive in Assets/,
# so the generated headers are game data and stay out of version control like
# everything else under Assets/. Regenerate them on a fresh checkout. The build
# already cannot work without Assets/ (the DOL and the preload bundles live
# there), so this adds no new requirement.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/robox_font_a8.h")
    message(STATUS "robox: extracting menu font from Assets/fonts/title.brfna")
    execute_process(
        COMMAND python "${CMAKE_SOURCE_DIR}/tools/extract_brfna.py"
                       "${CMAKE_SOURCE_DIR}/Assets/fonts/title.brfna"
                       "${CMAKE_SOURCE_DIR}/src/robox_font"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE ROBOX_FONT_RC)
    if(NOT ROBOX_FONT_RC EQUAL 0)
        message(FATAL_ERROR
            "menu font extraction failed -- run tools/extract_brfna.py by hand")
    endif()
endif()

# The setup screen's music and sound effects, baked into the binary.
#
# Unlike the font above these are OUR OWN content (Setup/*.rbxs, authored for
# this project) so the sources are committed and this step always works -- it
# needs nothing from Assets/. They have to be inside the exe rather than loose
# files because the setup runs on a fresh copy where nothing else exists yet:
# if its own music were a file on disk, the very first thing a new user sees
# would also be the thing most likely to be missing.
set(ROBOX_SETUP_AUDIO
    "Robox Recomp Setup Music:setup_music_rbxs"
    "Robot Select:setup_select_rbxs"
    "Robox Back:setup_back_rbxs"
    "Move:setup_move_rbxs")
foreach(entry IN LISTS ROBOX_SETUP_AUDIO)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 stem)
    list(GET parts 1 sym)
    set(out "${CMAKE_SOURCE_DIR}/src/generated_setup/${sym}.h")
    if(NOT EXISTS "${out}")
        message(STATUS "robox: baking setup audio ${stem}.rbxs")
        execute_process(
            COMMAND python "${CMAKE_SOURCE_DIR}/tools/bin2c.py"
                           "${CMAKE_SOURCE_DIR}/Setup/${stem}.rbxs" "${out}" "${sym}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE ROBOX_BIN2C_RC)
        if(NOT ROBOX_BIN2C_RC EQUAL 0)
            message(FATAL_ERROR
                "baking Setup/${stem}.rbxs failed -- run tools/bin2c.py by hand")
        endif()
    endif()
endforeach()

# The splash stills and its two sound cues.
#
# These live in splash/ rather than Assets/splash/ so that the rule "nothing
# under Assets/ is ours" holds without exception -- Assets/ is gitignored as
# retail game data, and an exception carved out of it is how game data ends up
# committed by accident. This artwork is the port's own
# (build/Assets/generate_mp4.py), which is also why it has to be baked in: the
# WAD does not contain it, so a fresh install has a complete Assets/ tree and
# still no splash, and the credit would silently skip on the first launch.
set(ROBOX_SPLASH_ASSETS
    "logo.rspl:splash_logo_rspl"
    "by.rspl:splash_by_rspl"
    "name.rspl:splash_name_rspl"
    "SLap_quack.rbxs:splash_slap_rbxs"
    "BAss.rbxs:splash_bass_rbxs")
foreach(entry IN LISTS ROBOX_SPLASH_ASSETS)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 sym)
    set(out "${CMAKE_SOURCE_DIR}/src/generated_setup/${sym}.h")
    if(NOT EXISTS "${out}")
        message(STATUS "robox: baking splash asset ${name}")
        execute_process(
            COMMAND python "${CMAKE_SOURCE_DIR}/tools/bin2c.py"
                           "${CMAKE_SOURCE_DIR}/splash/${name}" "${out}" "${sym}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE ROBOX_BIN2C_RC)
        if(NOT ROBOX_BIN2C_RC EQUAL 0)
            message(FATAL_ERROR
                "baking splash/${name} failed -- run tools/bin2c.py by hand")
        endif()
    endif()
endforeach()

# The mod loader's default configs, baked the same way so a fresh install
# arrives with mods/ documented and editable instead of silently defaulted.
# Only the configs: mods/robox.dls is built from the game's own robox.wt and
# robox.pcm, and mods/mario.chr is SMB1 data -- neither can live in this repo.
# The default control map, baked for the same reason as the mod configs: it is
# a file the game looks for beside the executable, and a release that does not
# carry it falls back to built-in defaults with no way to see or edit them.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/generated_setup/controls_cfg.h")
    message(STATUS "robox: baking controls.cfg")
    execute_process(
        COMMAND python "${CMAKE_SOURCE_DIR}/tools/bin2c.py"
                       "${CMAKE_SOURCE_DIR}/controls.cfg"
                       "${CMAKE_SOURCE_DIR}/src/generated_setup/controls_cfg.h"
                       controls_cfg
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE ROBOX_BIN2C_RC)
    if(NOT ROBOX_BIN2C_RC EQUAL 0)
        message(FATAL_ERROR "baking controls.cfg failed -- run tools/bin2c.py by hand")
    endif()
endif()

set(ROBOX_MOD_CONFIGS "mods.cfg:mods_cfg" "wav_music.cfg:wav_music_cfg")
foreach(entry IN LISTS ROBOX_MOD_CONFIGS)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 sym)
    set(out "${CMAKE_SOURCE_DIR}/src/generated_setup/${sym}.h")
    if(NOT EXISTS "${out}")
        message(STATUS "robox: baking mod config ${name}")
        execute_process(
            COMMAND python "${CMAKE_SOURCE_DIR}/tools/bin2c.py"
                           "${CMAKE_SOURCE_DIR}/mods/${name}" "${out}" "${sym}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE ROBOX_BIN2C_RC)
        if(NOT ROBOX_BIN2C_RC EQUAL 0)
            message(FATAL_ERROR
                "baking mods/${name} failed -- run tools/bin2c.py by hand")
        endif()
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The recompiled game code.
#
# src/generated/ is a line-by-line translation of the retail executable, so it
# is not in this repository for the same reason Robox USA.dol is not: a
# translation of a program is a derivative of it. It is produced here at
# configure time from YOUR copy of the game -- the arrangement UnleashedRecomp
# uses, where UnleashedRecompLib/CMakeLists.txt drives XenonRecomp from the
# default.xex you supply.
#
# Two things are needed, both overridable with -D:
#
#   ROBOX_RECOMPILER   a checkout of the recompiler repository
#   ROBOX_DOL          your own Robox USA.dol
#
# Once generated the tree is left alone, so this costs nothing on rebuilds.
# Delete src/generated/ to force a regen after changing the recompiler.
# ---------------------------------------------------------------------------
set(ROBOX_RECOMPILER "${CMAKE_SOURCE_DIR}/../WiiRecomp" CACHE PATH
    "Checkout of the WiiRecomp repository")
set(ROBOX_DOL "${CMAKE_SOURCE_DIR}/private/Robox USA.dol" CACHE FILEPATH
    "Your own copy of the game executable")

set(ROBOX_GENERATED "${CMAKE_SOURCE_DIR}/src/generated")

if(NOT EXISTS "${ROBOX_GENERATED}/func_table.c")
    if(NOT EXISTS "${ROBOX_RECOMPILER}/recompile_wii.py")
        message(FATAL_ERROR
            "No recompiled sources, and no recompiler at:\n"
            "    ${ROBOX_RECOMPILER}\n"
            "Clone the recompiler repository beside this one, or pass "
            "-DROBOX_RECOMPILER=<path>. See README.md.")
    endif()
    if(NOT EXISTS "${ROBOX_DOL}")
        message(FATAL_ERROR
            "No recompiled sources, and no game executable at:\n"
            "    ${ROBOX_DOL}\n"
            "Put your own Robox USA.dol there, or pass -DROBOX_DOL=<path>. "
            "No game data ships with this project. See README.md.")
    endif()

    message(STATUS "robox: recompiling ${ROBOX_DOL} (this takes a few seconds)")
    execute_process(
        COMMAND python "${ROBOX_RECOMPILER}/recompile_wii.py" "${ROBOX_DOL}"
                       --no-scaffold
                       --config-dir "${ROBOX_RECOMPILER}/games/robox"
                       --out "${ROBOX_GENERATED}"
        WORKING_DIRECTORY "${ROBOX_RECOMPILER}"
        RESULT_VARIABLE ROBOX_RECOMP_RC)
    if(NOT ROBOX_RECOMP_RC EQUAL 0)
        message(FATAL_ERROR "recompilation failed -- run recompile_wii.py by hand")
    endif()

    # The music mod's hooks are QUIRK EDITs spliced into the generated bodies,
    # so they have to go back in after every regen.
    execute_process(
        COMMAND python "${ROBOX_RECOMPILER}/games/robox/apply_music_hooks.py" "${ROBOX_GENERATED}"
        WORKING_DIRECTORY "${ROBOX_RECOMPILER}"
        RESULT_VARIABLE ROBOX_HOOKS_RC)
    if(NOT ROBOX_HOOKS_RC EQUAL 0)
        message(FATAL_ERROR "applying music hooks failed")
    endif()
endif()

file(GLOB GENERATED_FUNC_SOURCES CONFIGURE_DEPENDS "src/generated/funcs_*.c")
file(GLOB SDK_SOURCES            CONFIGURE_DEPENDS "sdk/*.c")
file(GLOB QUIRKS_SOURCES         CONFIGURE_DEPENDS "quirks/*.c")

# fmidi (vendored, fmidi-master/): host-side MIDI sequencer for the music.
# Built with the describe API disabled so the fmt library isn't needed.
set(FMIDI_SOURCES
    fmidi-master/sources/fmidi/u_memstream.cc
    fmidi-master/sources/fmidi/u_stdio.cc
    fmidi-master/sources/fmidi/u_iterator.cc
    fmidi-master/sources/fmidi/file/read_smf.cc
    fmidi-master/sources/fmidi/file/write_smf.cc
    fmidi-master/sources/fmidi/file/read_xmi.cc
    fmidi-master/sources/fmidi/file/read_mus.cc
    fmidi-master/sources/fmidi/file/identify.cc
    fmidi-master/sources/fmidi/fmidi_internal.cc
    fmidi-master/sources/fmidi/fmidi_seq.cc
    fmidi-master/sources/fmidi/fmidi_util.cc
    fmidi-master/sources/fmidi/fmidi_player.cc
)

# Lua 5.4 (vendored, vendor/lua/): the VM behind the .lua mods in mods/lua/.
# Upstream sources, unmodified, minus lua.c and luac.c -- those are the
# standalone interpreter and compiler and both define main().
#
# Left in plain ISO C mode on purpose: no LUA_USE_POSIX, no LUA_USE_WINDOWS.
# That drops dlopen and popen, which this has no use for, and is the only
# configuration that is simultaneously correct for mingw, the NDK and
# Emscripten -- all three of which build this exact file list.
#
# Not built for the 3DS. That port already had to trim 26 MB of BSS to fit
# New 3DS FCRAM and has no overlay renderer for a mod to draw with; sdk/
# robox_lua*.c compile to no-ops there (see the __3DS__ guard in each).
if(NOT ROBOX_3DS)
    set(LUA_SOURCES
        vendor/lua/lapi.c    vendor/lua/lcode.c    vendor/lua/lctype.c
        vendor/lua/ldebug.c  vendor/lua/ldo.c      vendor/lua/ldump.c
        vendor/lua/lfunc.c   vendor/lua/lgc.c      vendor/lua/llex.c
        vendor/lua/lmem.c    vendor/lua/lobject.c  vendor/lua/lopcodes.c
        vendor/lua/lparser.c vendor/lua/lstate.c   vendor/lua/lstring.c
        vendor/lua/ltable.c  vendor/lua/ltm.c      vendor/lua/lundump.c
        vendor/lua/lvm.c     vendor/lua/lzio.c
        vendor/lua/lauxlib.c vendor/lua/lbaselib.c vendor/lua/lcorolib.c
        vendor/lua/ldblib.c  vendor/lua/liolib.c   vendor/lua/lmathlib.c
        vendor/lua/loadlib.c vendor/lua/loslib.c   vendor/lua/lstrlib.c
        vendor/lua/ltablib.c vendor/lua/lutf8lib.c vendor/lua/linit.c
    )
else()
    set(LUA_SOURCES)
endif()

# Application icon. Only Windows consumes a .rc, and the RC language has to be
# enabled explicitly -- project() above declares C and CXX only, so without this
# CMake treats robox.rc as an unknown extension and silently ignores it.
if(WIN32)
    enable_language(RC)
    set(ROBOX_PLATFORM_SOURCES robox.rc)
else()
    set(ROBOX_PLATFORM_SOURCES)
endif()

set(ROBOX_SOURCES
    ${ROBOX_PLATFORM_SOURCES}   # robox.rc on Windows
    ${ROBOX_PORT_SOURCES}       # a port repository's platform/*.c, if given
    src/main.c
    src/runtime.c
    src/generated/func_table.c
    ${GENERATED_FUNC_SOURCES}
    ${SDK_SOURCES}
    ${QUIRKS_SOURCES}
    ${FMIDI_SOURCES}
    ${LUA_SOURCES}
    sdk/robox_midi.cc
    sdk/robox_synth.cc
)

# Android has no executables in the normal sense: SDL's SDLActivity does the
# process setup on the Java side and then dlopen()s a shared library, which it
# expects to be called "libmain.so" unless the activity overrides getLibraries().
# Everywhere else this stays a plain executable.
if(ANDROID)
    add_library(robox SHARED ${ROBOX_SOURCES})
else()
    add_executable(robox ${ROBOX_SOURCES})
endif()

# No console window on Windows.
#
# Without this the exe is built for the console subsystem, so double-clicking
# it pops a cmd window that sits behind the game for the whole session. The
# game owns main() (SDL_MAIN_HANDLED above), and -mwindows only changes the PE
# subsystem flag -- mingw's startup still enters at main(), so nothing about
# the entry point changes.
#
# Safe now that a normal run is silent: stderr goes to the null device unless
# RECOMP_LOG asks for it, and both that and the crash dump write to files
# rather than a terminal. Run from a shell with RECOMP_LOG=1 and read
# logs/run.log; a GUI-subsystem process has nowhere to print to.
#
# A diagnostic build is the exception and keeps its console. The whole point of
# that build is to say why it failed, and a GUI-subsystem process has nowhere
# to say it -- which is precisely the hole being investigated. Run it from a
# terminal and the output is right there even if writing the log file fails.
if(WIN32)
    if(CMAKE_C_FLAGS MATCHES "ROBOX_DIAG")
        message(STATUS "ROBOX: diagnostic build -- keeping the console subsystem")
    else()
        set_target_properties(robox PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()

target_include_directories(robox PRIVATE
    src
    src/generated
    sdk
    quirks
    fmidi-master/sources
    vendor/lua
    vendor
)
set_source_files_properties(${FMIDI_SOURCES} sdk/robox_midi.cc PROPERTIES
    COMPILE_DEFINITIONS "FMIDI_STATIC=1;FMIDI_BUILD=1;FMIDI_DISABLE_DESCRIBE_API=1")

# -O2 is REQUIRED for correctness here, not just speed: mid-function labels
# discovered as standalone functions make guest loops bounce between C
# functions via emitted tail calls (`func_X(); return;`). At -O0 every
# bounce pushes a host frame — multi-million-iteration guest loops overflow
# the host stack (0xc00000fd during GXInit's memclear). GCC's sibling-call
# optimization (-O2) turns those into jumps. -fno-strict-aliasing guards the
# fpr union punning.
# -O3 + host tuning on top of the mandatory -O2/-fno-strict-aliasing. The
# recompiled game code is the frame-time bottleneck in gameplay (~14 ms of a
# 16.7 ms budget at -O2), and it is an enormous pile of small functions over
# one global context struct — exactly what better inlining and scheduling
# help.
#
# It defaults OFF, and must stay that way, because a binary built here gets
# handed to other people. -march=native means "assume the build machine's exact
# CPU", and on this one that is znver3: gcc then emits AVX2, BMI2 and FMA
# throughout, raising the floor to Intel Haswell (2013) or AMD Zen. Anything
# older -- and every Celeron/Pentium N-series ever made, which have no AVX at
# any generation -- dies on an illegal instruction in the first few hundred
# instructions of startup.
#
# That failure is completely silent: it happens long before main(), so there is
# no window, no message box, no log, and no crash dialog worth the name. It
# looks exactly like double-clicking the exe and nothing happening at all,
# which is precisely what it was reported as. Turning this on cost a release
# that ran on the machine that built it and nowhere else.
#
# ON is for a build you are going to run yourself and nobody else will.
option(ROBOX_NATIVE_ARCH "Optimize for this exact CPU (-march=native). Produces a binary that will NOT run on older CPUs -- never enable it for a build you intend to distribute." OFF)

# The baseline to build against when ROBOX_NATIVE_ARCH is OFF.
#
#   x86-64      SSE2 only. Every 64-bit CPU ever sold. The default, and what
#               the download most people take should be built with.
#   x86-64-v2   SSE4.2, POPCNT. Nehalem (2008) / Bulldozer (2011).
#   x86-64-v3   AVX2, BMI2, FMA. Haswell (2013) / Zen (2017). Most of what
#               -march=native was buying, minus the part where the answer
#               depends on whose machine ran the compiler.
#
# Prefer v3 over native for a second, faster download. native means znver3 on
# this build host, which pulls in SSE4a -- an AMD-only extension that no Intel
# CPU implements at any generation. gcc does not currently emit any, so such a
# binary runs on Intel by luck rather than by design, and nothing guarantees
# the next rebuild is as lucky. v3 is vendor-neutral and states its floor.
set(ROBOX_ARCH "x86-64" CACHE STRING "CPU baseline when ROBOX_NATIVE_ARCH is OFF: x86-64, x86-64-v2, or x86-64-v3")
# -O2 is the correctness floor everywhere (sibling-call optimisation, see above).
# Desktop/web push to -O3; the 3DS stays at -O2 to keep .text small (63 MB cap,
# and the whole executable image lives in the 3DS's limited FCRAM) and to keep
# the 50 MB of generated C compiling in reasonable time on the ARM cross-gcc.
if(ROBOX_3DS)
    set(_robox_opt -O2)
else()
    set(_robox_opt -O3)
endif()
target_compile_options(robox PRIVATE ${WARN_FLAGS} ${_robox_opt} -fno-strict-aliasing)
if(ROBOX_NATIVE_ARCH)
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-march=native HAVE_MARCH_NATIVE)
    if(HAVE_MARCH_NATIVE)
        message(WARNING
            "ROBOX_NATIVE_ARCH is ON: this binary is built for THIS CPU and will "
            "crash silently at startup on an older one. Do not distribute it.")
        target_compile_options(robox PRIVATE -march=native)
    endif()
elseif(NOT CMAKE_CROSSCOMPILING AND NOT ROBOX_3DS AND NOT EMSCRIPTEN)
    # Say the baseline out loud rather than inheriting whatever the toolchain
    # happens to default to -- the whole point here is that the answer is not
    # allowed to depend on the machine doing the build. -mtune=generic still
    # schedules for modern CPUs, so the default costs scheduling preference
    # rather than instructions.
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-march=${ROBOX_ARCH} -mtune=generic" HAVE_MARCH_BASELINE)
    if(HAVE_MARCH_BASELINE)
        message(STATUS "ROBOX: CPU baseline -march=${ROBOX_ARCH}")
        target_compile_options(robox PRIVATE -march=${ROBOX_ARCH} -mtune=generic)
    else()
        message(FATAL_ERROR
            "ROBOX_ARCH=${ROBOX_ARCH} is not accepted by this compiler. Silently "
            "carrying on would fall back to the toolchain default and produce a "
            "binary whose CPU requirements nobody knows.")
    endif()
endif()

# GLES 3.0 instead of desktop GL 3.3 core. Forced ON for Android; can be set on
# a desktop build (with ANGLE or a driver that offers ES contexts) to validate
# the ES renderer path without a device in the loop.
if(ANDROID OR EMSCRIPTEN)
    # WebGL2 is GLES 3.0, so the browser wants exactly the Android shader path.
    set(_robox_gles_default ON)
else()
    set(_robox_gles_default OFF)
endif()
option(ROBOX_GLES "Target OpenGL ES 3.0 instead of desktop GL 3.3 core" ${_robox_gles_default})
if(ROBOX_GLES)
    target_compile_definitions(robox PRIVATE ROBOX_GLES=1)
    message(STATUS "robox: renderer targeting OpenGL ES 3.0")
endif()
# Host MEM2 backing. On desktop/web this is 256 MB so the HLE heap can hand out
# guest addresses deep into MEM2 (see sdk/os.c) with room to spare. The 3DS does
# not have that room: New 3DS gives homebrew ~124 MB of FCRAM total, and code
# (~15 MB .text) + MEM1 (24 MB) already eat a third of it. Start at the
# guest-visible 64 MB and raise only if the HLE pool actually overflows -- that
# overflow, if it happens, is a real datum about the port, not a number to guess
# around. Override with -DROBOX_MEM2_SIZE=0x...
if(ROBOX_3DS)
    # Guest-visible MEM2 (64 MB, 0x90000000..0x94000000) PLUS the 8 MB HLE heap
    # that lives just above it (0x94000000..0x94800000, see sdk/os.c). Both must
    # be backed by the g_mem2 buffer, so 72 MB total. With MEM1 (24 MB) that is
    # 96 MB of guest arenas -- the ceiling that keeps the whole image inside New
    # 3DS FCRAM. Raising the HLE heap means raising this in lockstep.
    set(ROBOX_MEM2_SIZE "0x04800000" CACHE STRING "Host MEM2 backing size")
else()
    set(ROBOX_MEM2_SIZE "0x10000000" CACHE STRING "Host MEM2 backing size")
endif()
target_compile_definitions(robox PRIVATE
    _CRT_SECURE_NO_WARNINGS
    SDL_MAIN_HANDLED=1
    RECOMP_PROJECT_TITLE="ROBOX"
    RECOMP_DEFAULT_ELF="Robox USA.dol"
    PPC_MEM2_SIZE=${ROBOX_MEM2_SIZE}
)
if(ROBOX_3DS)
    target_compile_definitions(robox PRIVATE __3DS__=1 ROBOX_3DS=1)
endif()

if(ANDROID)
    set_target_properties(robox PROPERTIES OUTPUT_NAME "main")   # -> libmain.so
else()
    set_target_properties(robox PROPERTIES OUTPUT_NAME "ROBOX")
endif()

# Per-function guest tracer. It injects a ppc_trace() call at the top of EVERY
# recompiled function (millions of calls per second) and is only useful when
# chasing a crash — it costs a large fraction of the frame time, so it is OFF
# by default. Turn on with: cmake -S . -B build -DROBOX_TRACE=ON
# (It used to be forced on via a stale CMAKE_C_FLAGS cache entry, which is
# what made the game stutter.)
option(ROBOX_TRACE "Compile the per-function guest tracer (slow)" OFF)
if(ROBOX_TRACE)
    target_compile_definitions(robox PRIVATE PPC_TRACE_ALL=1)
    message(STATUS "robox: per-function tracer ENABLED (slow)")
endif()

# SDL2. On Android there is no system SDL2 to find -- it is built from source as
# a subproject and its Java half is packaged into the APK, so pkg-config /
# find_package would hard-error at configure time. Point ROBOX_SDL2_SOURCE_DIR
# at an SDL2 checkout (default: vendor-android/SDL2, which is gitignored).
if(EMSCRIPTEN)
    # Emscripten ships SDL2 as a port; -sUSE_SDL=2 is needed at BOTH compile and
    # link time (it supplies the headers as well as the implementation).
    target_compile_options(robox PRIVATE -sUSE_SDL=2)
    target_link_options(robox PRIVATE -sUSE_SDL=2)
elseif(ANDROID)
    set(ROBOX_SDL2_SOURCE_DIR "${CMAKE_SOURCE_DIR}/vendor-android/SDL2"
        CACHE PATH "SDL2 source tree used for the Android build")
    if(NOT EXISTS "${ROBOX_SDL2_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Android build needs SDL2 sources at ${ROBOX_SDL2_SOURCE_DIR}\n"
            "  git clone --branch SDL2 --depth 1 https://github.com/libsdl-org/SDL "
            "${ROBOX_SDL2_SOURCE_DIR}")
    endif()
    add_subdirectory("${ROBOX_SDL2_SOURCE_DIR}" SDL2-build)
    # SDL2main is not used: SDLActivity calls into SDL_main itself.
    target_link_libraries(robox PRIVATE SDL2::SDL2)
elseif(ROBOX_3DS)
    # devkitPro portlibs prebuilt SDL2 (2.28.5), matched to this libctru. Linked
    # ONLY for os.c's OSThread->SDL threading/sync -- SDL video is never
    # initialised (graphics go through citro3d, input through hid, audio through
    # ndsp). No SDL2main: we own main() (SDL_MAIN_HANDLED=1 above), and linking
    # SDL2main's own main() would collide with it. Headers are included as
    # <SDL2/SDL.h>, so the parent include dir goes on the path.
    if(NOT DEVKITPRO)
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    endif()
    target_include_directories(robox PRIVATE "${DEVKITPRO}/portlibs/3ds/include")
    target_link_libraries(robox PRIVATE "${DEVKITPRO}/portlibs/3ds/lib/libSDL2.a")
else()
    # pkg-config (MSYS2 mingw64) with find_package fallback.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    endif()
    if(TARGET PkgConfig::SDL2)
        target_link_libraries(robox PRIVATE PkgConfig::SDL2)
    else()
        find_package(SDL2 REQUIRED)
        target_link_libraries(robox PRIVATE SDL2::SDL2)
    endif()
endif()

if(EMSCRIPTEN)
    # ---- Web target -------------------------------------------------------
    # -mtail-call is what makes the generated PPC_TAIL (musttail) edges become
    # real wasm `return_call`s. Without it a guest loop back-edge grows the wasm
    # stack every iteration and a 5M-iteration loop dies -- measured.
    target_compile_options(robox PRIVATE -mtail-call -pthread)
    target_link_options(robox PRIVATE
        -mtail-call
        # The guest never returns to a host loop: it paces inside
        # hle_VIWaitForRetrace with a blocking frame limiter. PROXY_TO_PTHREAD
        # runs main() on a worker so that blocking design survives untouched
        # and the browser's main thread stays free to pump events and audio.
        # (The alternative, ASYNCIFY, cannot unwind the limiter's busy-spin.)
        -pthread
        -sPROXY_TO_PTHREAD=1
        -sPTHREAD_POOL_SIZE=4
        # The guest itself issues the GL calls (gx_ogl_draw runs deep inside
        # recompiled code), so GL has to work on the worker that PROXY_TO_PTHREAD
        # puts main() on. A WebGL context belongs to the main thread's canvas and
        # is undefined from a worker -- the first symptom is
        # "Cannot read properties of undefined (reading 'createShader')".
        # Transferring the canvas as an OffscreenCanvas gives the worker a real
        # context of its own.
        # NOTE: OFFSCREENCANVAS_SUPPORT + OFFSCREENCANVASES_TO_PTHREAD were tried
        # and removed. The canvas was never actually transferred to the worker,
        # so the context we created there rendered correctly into something the
        # page could not see -- a black page with a game running at 60 fps behind
        # it. GL is proxied to the main thread instead (see video.c).
        # Fallback path for GL on a worker when the browser/SDL cannot hand over
        # a real OffscreenCanvas: render into an offscreen framebuffer that is
        # blitted to the page canvas. Broader compatibility than OffscreenCanvas
        # alone, and it is what makes SDL_GL_CreateContext succeed off-main.
        -sOFFSCREEN_FRAMEBUFFER=1
        # Guest arenas are ~280 MB (MEM1 24 + MEM2 backing 256), so let the
        # heap grow rather than reserving it all up front.
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=2GB
        # Must exceed STACK_SIZE, and the guest arenas get allocated on top.
        -sINITIAL_MEMORY=1GB
        # Deep guest call chains need a LOT of stack even with tail calls
        # working. Level loading overflowed a 64 MB stack ("RangeError: Maximum
        # call stack size exceeded" while reading media/nivel01/*.tpl) -- the
        # Windows build reserves 512 MB for exactly this reason, and the
        # runtime's own runaway-recursion guard only trips at 300 MB, so the
        # guest genuinely uses hundreds of MB here.
        -sSTACK_SIZE=384MB
        -sDEFAULT_PTHREAD_STACK_SIZE=384MB
        # WebGL2 == GLES 3.0, which is what the renderer targets.
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sFULL_ES3=1
        -sEXIT_RUNTIME=0
        -sASSERTIONS=1
        # Keep the wasm name section. Every recompiled function is named
        # func_<guest-VA>, so a JS stack trace with names IS a guest backtrace:
        # "func_800ddd34" instead of "wasm-function[41207]". This is the single
        # thing that makes a browser-side crash identifiable at all, and unlike
        # -g it is a LINK-only flag (no recompile of the 54 generated buckets)
        # that keeps full -O3 codegen. Costs ~1 MB of .wasm in symbol names.
        --profiling-funcs
        # Save persistence. The guest writes saves with plain fopen/fwrite into
        # "nand/", which on this target lands in MEMFS and dies with the tab.
        # IDBFS backs that one directory with IndexedDB instead. It is mounted
        # and synced from the browser MAIN thread (the shell's preRun) because
        # that is where the FS lives -- every FS syscall the guest worker makes
        # already begins with `if (ENVIRONMENT_IS_PTHREAD) proxyToMainThread(..)`,
        # so the guest's own file I/O needs no changes at all.
        # NOT -sASYNCIFY: the synchronous emscripten_idb_* API would need it and
        # it is rejected elsewhere in this file (cannot unwind the frame
        # limiter's busy-spin). Without it those calls are an unconditional
        # abort(), so a save would kill the page.
        -lidbfs.js
        # FS/IDBFS are reachable as bare globals today only because the build is
        # neither MODULARIZE'd nor Closure-compiled. Naming them makes the
        # shell's dependency a contract instead of an accident.
        -sEXPORTED_RUNTIME_METHODS=FS,IDBFS,addRunDependency,removeRunDependency
        # Custom page: the stock shell leaves the canvas at its 300x150 default,
        # so the game rendered into a corner of the drawing buffer and the page
        # showed a tiny image. The shell sets the buffer to 1280x720 and scales
        # the DISPLAY to fit the viewport at 16:9.
        "--shell-file=${ROBOX_WEB_SHELL}"
    )
    set_property(TARGET robox APPEND PROPERTY LINK_DEPENDS "${ROBOX_WEB_SHELL}")
    # Game data. The engine opens everything with plain relative-path fopen, so
    # the files must exist in MEMFS under the same names the desktop build sees.
    # --preload-file builds a .data bundle the loader fetches before main runs;
    # nothing in the engine needs to know it is on the web.
    # (~44 MB: Assets 35 + DOL 2 + mods 7.6.)
    # mods/ is listed file by file rather than as a directory so the web build
    # can carry its OWN mods.cfg (tools/web_mods.cfg) instead of the desktop
    # one. Co-op is off there: a second player is a second entity simulated and
    # drawn every frame, and this is the target that cannot spare it.
    target_link_options(robox PRIVATE
        "--preload-file=${CMAKE_SOURCE_DIR}/Assets@/Assets"
        "--preload-file=${CMAKE_SOURCE_DIR}/Robox USA.dol@/Robox USA.dol"
        "--preload-file=${CMAKE_SOURCE_DIR}/build/mods/robox.dls@/mods/robox.dls"
        "--preload-file=${ROBOX_WEB_MODS}@/mods/mods.cfg"
        # Sits beside the executable on desktop, so mount it at the root here
        # too -- the loader looks for a bare "controls.cfg" on every target.
        "--preload-file=${CMAKE_SOURCE_DIR}/controls.cfg@/controls.cfg"
    )
    set_property(TARGET robox APPEND PROPERTY LINK_DEPENDS
                 "${CMAKE_SOURCE_DIR}/tools/web_mods.cfg")
    set_target_properties(robox PROPERTIES SUFFIX ".html")
elseif(ROBOX_3DS)
    # citro3d/citro2d (PICA200 GPU), ctru (kernel/hid/ndsp/romfs). ctru is listed
    # last because both citro3d and the already-linked SDL2 depend on it.
    target_link_libraries(robox PRIVATE citro3d citro2d ctru m)
    # Route the engine's bare relative fopen() paths to RomFS (reads) / SD
    # (writes). See __wrap_fopen in sdk/platform_3ds.c.
    target_link_options(robox PRIVATE -Wl,--wrap=fopen)
elseif(WIN32)
    # comdlg32/shell32/ole32: the setup's "locate files manually" opens the
    # real Windows file and folder pickers (sdk/robox_setup.c). SDL2 has no
    # file dialog of its own -- that arrived in SDL3.
    # uuid supplies CLSID_FileOpenDialog / IID_IFileDialog for the folder picker.
    target_link_libraries(robox PRIVATE opengl32 winmm comdlg32 shell32 ole32 uuid)
elseif(ANDROID)
    # Android has no libGL and no libpthread (pthreads are in libc). Every GL
    # entry point is resolved at runtime through SDL_GL_GetProcAddress, so
    # GLESv3 is linked for the core symbols and EGL for context management.
    target_link_libraries(robox PRIVATE m GLESv3 EGL android log)
elseif(APPLE)
    target_link_libraries(robox PRIVATE m pthread "-framework OpenGL")
else()
    target_link_libraries(robox PRIVATE m pthread GL)
endif()

# Deep guest call chains become deep host call chains; give the main
# thread plenty of stack.
# Also statically link the MinGW runtime (libwinpthread/libgcc/libstdc++):
# the exe must run outside an MSYS2 shell, where mingw64/bin is not on PATH.
# libstdc++ matters because fmidi and sdk/robox_synth.cc are C++ — without
# -static-libstdc++ the exe dies with "libstdc++-6.dll was not found" on any
# machine that lacks MSYS2.
if(WIN32)
    target_link_options(robox PRIVATE -Wl,--stack,536870912
                        -static-libgcc -static-libstdc++
                        -Wl,-Bstatic,--whole-archive
                        -lwinpthread -Wl,--no-whole-archive,-Bdynamic)
    # WinHTTP backs sdk/robox_net.c (robox.net.fetch). Part of Windows, so no
    # vendored dependency, and it handles TLS and redirects itself. Everywhere
    # else that file compiles to a stub that fails cleanly.
    target_link_libraries(robox PRIVATE winhttp)
endif()

# ---------------------------------------------------------------------------
# 3DS packaging: wrap the ELF into a .3dsx with an embedded RomFS so a single
# file boots in Citra (File -> Load) with no SD-card copy. Everything the engine
# opens by relative path is staged into RomFS and reached at runtime as
# romfs:/<path> by the fopen shim (sdk/robox_io.c).
# ---------------------------------------------------------------------------
if(ROBOX_3DS)
    set(ROBOX_ROMFS "${CMAKE_BINARY_DIR}/romfs")
    # Assemble once. file(COPY) of the 35 MB Assets tree on every reconfigure
    # would be painful, so only stage when the tree is missing; delete
    # build/romfs to force a re-stage after the game data changes.
    if(NOT EXISTS "${ROBOX_ROMFS}/Robox USA.dol")
        message(STATUS "robox: staging RomFS at ${ROBOX_ROMFS}")
        file(MAKE_DIRECTORY "${ROBOX_ROMFS}")
        file(COPY "${CMAKE_SOURCE_DIR}/Assets"        DESTINATION "${ROBOX_ROMFS}")
        file(COPY "${CMAKE_SOURCE_DIR}/Robox USA.dol" DESTINATION "${ROBOX_ROMFS}")
        if(EXISTS "${CMAKE_SOURCE_DIR}/controls.cfg")
            file(COPY "${CMAKE_SOURCE_DIR}/controls.cfg" DESTINATION "${ROBOX_ROMFS}")
        endif()
        if(EXISTS "${CMAKE_SOURCE_DIR}/build/mods/robox.dls")
            file(MAKE_DIRECTORY "${ROBOX_ROMFS}/mods")
            file(COPY "${CMAKE_SOURCE_DIR}/build/mods/robox.dls"
                 DESTINATION "${ROBOX_ROMFS}/mods")
            # Co-op off on the handheld (a second simulated+drawn entity every
            # frame is exactly what this target cannot spare), same as web.
            configure_file("${CMAKE_SOURCE_DIR}/tools/web_mods.cfg"
                           "${ROBOX_ROMFS}/mods/mods.cfg" COPYONLY)
        endif()
    endif()

    ctr_generate_smdh(OUTPUT "${CMAKE_BINARY_DIR}/robox.smdh"
        NAME        "ROBOX"
        DESCRIPTION "Robox (WiiWare) 3DS recomp"
        AUTHOR      "Robox recomp")
    ctr_create_3dsx(robox
        SMDH  "${CMAKE_BINARY_DIR}/robox.smdh"
        ROMFS "${ROBOX_ROMFS}")
endif()

add_custom_target(regenerate
    COMMAND python "${CMAKE_SOURCE_DIR}/recompile_wii.py" "${CMAKE_SOURCE_DIR}/Robox USA.dol" --no-scaffold
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running PPC->C recompile pass"
)
