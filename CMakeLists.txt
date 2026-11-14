# Native port of Pokemon Rumble: macOS build (and tvOS link check) for phases 1-6.
# The tvOS app itself is built by the xcodegen project in tvos/ (phase 7).
#
#   cmake -S . -B build/mac -G Ninja && cmake --build build/mac
#   cmake -S . -B build/tvos -G Ninja -DCMAKE_SYSTEM_NAME=tvOS && cmake --build build/tvos
cmake_minimum_required(VERSION 3.24)
project(PokemonRumbleTV C CXX)

set(CMAKE_OSX_ARCHITECTURES arm64)
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(PORT_SDK appletvos)
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

# -DPORT_SANITIZE=ON: AddressSanitizer and UndefinedBehaviorSanitizer on everything
# (the phase 2 check runs this build).
option(PORT_SANITIZE "Build with ASan and UBSan" OFF)
if(PORT_SANITIZE)
    # -O0 like the normal build: optimisation changes which inline game functions are
    # emitted, and the fork relies on the unoptimised set.
    set(SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize=vptr,function -fno-omit-frame-pointer
        -fsanitize-ignorelist=${ROOT}/port/sanitize_ignorelist.txt)
    foreach(target rumble_fork rumble_port)
        target_compile_options(${target} PRIVATE ${SANITIZE_FLAGS})
    endforeach()
endif()

# Stubs for everything neither the fork nor the port defines yet.
set(LINK_STUBS ${CMAKE_BINARY_DIR}/link_stubs.c)
add_custom_command(
    OUTPUT ${LINK_STUBS}
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/gen_link_stubs.py --sdk ${PORT_SDK} --out ${LINK_STUBS}
            $<TARGET_FILE:rumble_fork> $<TARGET_FILE:rumble_port>
    DEPENDS rumble_fork rumble_port ${ROOT}/tools/gen_link_stubs.py
    COMMENT "Generating link stubs")

add_executable(rumble ${ROOT}/platform/headless/main.c ${LINK_STUBS})
set_source_files_properties(${ROOT}/platform/headless/main.c PROPERTIES COMPILE_OPTIONS "${FORK_COMMON_FLAGS};${FORK_C_FLAGS};-g")
target_link_libraries(rumble PRIVATE rumble_fork rumble_port)
if(PORT_SANITIZE)
    target_compile_options(rumble PRIVATE ${SANITIZE_FLAGS})
    target_link_options(rumble PRIVATE ${SANITIZE_FLAGS})
endif()

# ctest: the asset fixups over every content file, and the phase 2 boot check (run it
# from the PORT_SANITIZE build for ASan/UBSan). Both need the imported game files.
enable_testing()
add_test(NAME check_assets COMMAND rumble --check-assets)
add_test(NAME boot_600_frames COMMAND ${ROOT}/tests/headless/boot.sh $<TARGET_FILE:rumble> 600 ${CMAKE_BINARY_DIR}/boot-test)
