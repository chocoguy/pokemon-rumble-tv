# Native port of Pokemon Rumble: the macOS headless runner, and the static libraries the
# tvOS app links (tvos/tools/build.sh builds them here, then the xcodegen project).
#
#   cmake -S . -B build/mac -G Ninja && cmake --build build/mac
#   cmake -S . -B build/tvos -G Ninja -DCMAKE_SYSTEM_NAME=tvOS && cmake --build build/tvos
#   cmake -S . -B build/tvos-sim -G Ninja -DCMAKE_SYSTEM_NAME=tvOS -DCMAKE_OSX_SYSROOT=appletvsimulator
cmake_minimum_required(VERSION 3.24)
project(PokemonRumbleTV C CXX OBJC)

set(CMAKE_OSX_ARCHITECTURES arm64)
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    # -DCMAKE_OSX_SYSROOT=appletvsimulator builds for the simulator.
    if(CMAKE_OSX_SYSROOT MATCHES "appletvsimulator")
        set(PORT_SDK appletvsimulator)
    else()
        set(PORT_SDK appletvos)
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0)
else()
    set(PORT_SDK macosx)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Flags shared with tools/compile_check.py.
file(STRINGS ${ROOT}/port/compile_flags.txt FLAG_LINES)
set(FORK_COMMON_FLAGS "")
set(FORK_C_FLAGS "")
set(FORK_CXX_FLAGS "")
set(expect_include_path FALSE)
foreach(line IN LISTS FLAG_LINES)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    if(line MATCHES "^c: *(.*)$")
        list(APPEND FORK_C_FLAGS "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^cxx: *(.*)$")
        list(APPEND FORK_CXX_FLAGS "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^-I(.*)$")
        list(APPEND FORK_COMMON_FLAGS "-I${ROOT}/${CMAKE_MATCH_1}")
    elseif(expect_include_path)
        list(APPEND FORK_COMMON_FLAGS "${ROOT}/${line}")
        set(expect_include_path FALSE)
    elseif(line STREQUAL "-include")
        list(APPEND FORK_COMMON_FLAGS "-include")
        set(expect_include_path TRUE)
    else()
        list(APPEND FORK_COMMON_FLAGS "${line}")
    endif()
endforeach()
set(FORK_FLAGS ${FORK_COMMON_FLAGS} $<$<COMPILE_LANGUAGE:C>:${FORK_C_FLAGS}> $<$<COMPILE_LANGUAGE:CXX>:${FORK_CXX_FLAGS}>)

# The fork's units: the same list compile_check.py checks.
execute_process(COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/compile_check.py --list
                OUTPUT_VARIABLE FORK_SOURCES COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ROOT}/tools/compile_check.py ${ROOT}/port/compile_flags.txt)

add_library(rumble_fork STATIC ${FORK_SOURCES})
target_compile_options(rumble_fork PRIVATE ${FORK_FLAGS} -g)
set_target_properties(rumble_fork PROPERTIES CXX_STANDARD 98 CXX_EXTENSIONS ON)

# The port: the host side of the Wii SDK (grows phase by phase).
file(GLOB PORT_SOURCES CONFIGURE_DEPENDS ${ROOT}/port/*/*.c ${ROOT}/port/*/*.cpp)
add_library(rumble_port STATIC ${PORT_SOURCES})
target_compile_options(rumble_port PRIVATE ${FORK_FLAGS} -g)

# The GPU backend (Metal), shared with the tvOS app. Plain Objective-C with ARC: it
# includes only port_gfx.h, not the fork's headers.
add_library(rumble_metal STATIC ${ROOT}/platform/metal/port_gfx_metal.m)
target_include_directories(rumble_metal PRIVATE ${ROOT}/port/include)
target_compile_options(rumble_metal PRIVATE -fobjc-arc -g -Wall)

# -DPORT_SANITIZE=ON: AddressSanitizer and UndefinedBehaviorSanitizer on everything
# (the phase 2 check runs this build).
option(PORT_SANITIZE "Build with ASan and UBSan" OFF)
if(PORT_SANITIZE)
    # -O0 like the normal build: optimisation changes which inline game functions are
    # emitted, and the fork relies on the unoptimised set.
    set(SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize=vptr,function -fno-omit-frame-pointer
        -fsanitize-ignorelist=${ROOT}/port/sanitize_ignorelist.txt)
    foreach(target rumble_fork rumble_port rumble_metal)
        target_compile_options(${target} PRIVATE ${SANITIZE_FLAGS})
    endforeach()
endif()

# Stubs for everything neither the fork nor the port defines yet.
set(LINK_STUBS ${CMAKE_BINARY_DIR}/link_stubs.c)
add_custom_command(
    OUTPUT ${LINK_STUBS}
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/gen_link_stubs.py --sdk ${PORT_SDK} --out ${LINK_STUBS}
            --provides $<TARGET_FILE:rumble_metal> $<TARGET_FILE:rumble_fork> $<TARGET_FILE:rumble_port>
    DEPENDS rumble_fork rumble_port rumble_metal ${ROOT}/tools/gen_link_stubs.py
    COMMENT "Generating link stubs")

add_library(rumble_stubs STATIC ${LINK_STUBS})

add_executable(rumble ${ROOT}/platform/headless/main.c)
set_source_files_properties(${ROOT}/platform/headless/main.c PROPERTIES COMPILE_OPTIONS "${FORK_COMMON_FLAGS};${FORK_C_FLAGS};-g")
target_link_libraries(rumble PRIVATE rumble_fork rumble_port rumble_metal rumble_stubs
    "-framework Foundation" "-framework Metal" "-framework CoreGraphics" "-framework ImageIO")
if(PORT_SANITIZE)
    target_compile_options(rumble PRIVATE ${SANITIZE_FLAGS})
    target_link_options(rumble PRIVATE ${SANITIZE_FLAGS})
endif()

# ctest: the asset fixups over every content file, and the boot checks (run them from the
# PORT_SANITIZE build for ASan/UBSan). All need the imported game files.
enable_testing()
add_test(NAME check_assets COMMAND rumble --check-assets)
add_test(NAME boot_600_frames COMMAND ${ROOT}/tests/headless/boot.sh $<TARGET_FILE:rumble> 600 ${CMAKE_BINARY_DIR}/boot-test)
# Phase 3: through the strap screen to the drawn title screen (title.png for a look).
add_test(NAME title_1900_frames COMMAND ${ROOT}/tests/headless/boot.sh $<TARGET_FILE:rumble> 1900 ${CMAKE_BINARY_DIR}/title-test
         --dump-frame 1800 ${CMAKE_BINARY_DIR}/title-test-title.png)
# A scripted new game into the hub: 3D models, particles and EFB copies (hub.png).
add_test(NAME new_game_7300_frames COMMAND ${ROOT}/tests/headless/boot.sh $<TARGET_FILE:rumble> 7300 ${CMAKE_BINARY_DIR}/new-game-test
         --input ${ROOT}/tests/input/new_game.txt --dump-frame 2701 ${CMAKE_BINARY_DIR}/new-game-test-intro.png
         --dump-frame 7201 ${CMAKE_BINARY_DIR}/new-game-test-hub.png)
# Phase 4: the same with a Classic Controller, as the Apple TV app presents controllers
# (the hub's hint then shows the Classic Controller's a instead of the remote's 2).
add_test(NAME new_game_classic_7300_frames COMMAND ${ROOT}/tests/headless/boot.sh $<TARGET_FILE:rumble> 7300
         ${CMAKE_BINARY_DIR}/new-game-classic-test --input ${ROOT}/tests/input/new_game_classic.txt
         --dump-frame 7201 ${CMAKE_BINARY_DIR}/new-game-classic-test-hub.png)
