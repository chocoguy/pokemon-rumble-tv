# The Apple TV app: the app shell (platform/tvos) around the game, the port and the Metal
# backend, with the icons, Top Shelf images and launch screen (Resources/Images.xcassets)
# and the player's imported game contents copied into the bundle. Xcode generator only
# (the presets tvos and tvos-sim); tools/tvos/port builds it for the installer.

if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    message(STATUS "RumbleTV (the app) needs the Xcode generator; building only the libraries")
    return()
endif()

# Keep the bundle ID the same from build to build: the saves are kept under it.
set(RUMBLE_BUNDLE_ID "com.example.rumble-tv" CACHE STRING "The app's bundle identifier")
# The team that signs device builds; empty for the simulator.
set(RUMBLE_DEVELOPMENT_TEAM "" CACHE STRING "Apple developer Team ID for device builds")

if(NOT EXISTS "${RUMBLE_CONTENT_DIR}/00000001.app" AND NOT EXISTS "${RUMBLE_CONTENT_DIR}/00000000.app")
    message(FATAL_ERROR "No game contents in ${RUMBLE_CONTENT_DIR}. Import them with tools/import_wad.py "
                        "(or tools/tvos/port prepare), or set RUMBLE_CONTENT_DIR.")
endif()
if(NOT EXISTS "${RUMBLE_GENERATED_DIR}/include/system_message.inc")
    message(FATAL_ERROR "No ${RUMBLE_GENERATED_DIR}/include/system_message.inc: run tools/import_wad.py first.")
endif()

set(_assets "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Images.xcassets")
file(GLOB _app_sources CONFIGURE_DEPENDS "${ROOT}/platform/tvos/*.m" "${ROOT}/platform/tvos/*.h")

add_executable(RumbleTV MACOSX_BUNDLE ${_app_sources} ${_assets})
target_include_directories(RumbleTV PRIVATE "${ROOT}/port/include" "${ROOT}/platform/metal")
target_compile_options(RumbleTV PRIVATE -fobjc-arc)
target_link_libraries(RumbleTV PRIVATE rumble_fork rumble_port rumble_metal rumble_audio rumble_stubs c++ compression
    "-framework UIKit" "-framework Foundation" "-framework Metal" "-framework QuartzCore"
    "-framework CoreGraphics" "-framework ImageIO" "-framework GameController" "-framework AVFoundation")

set_target_properties(RumbleTV PROPERTIES
    OUTPUT_NAME Rumble
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${RUMBLE_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "Rumble"
    MACOSX_BUNDLE_BUNDLE_VERSION "${RUMBLE_BUILD_NUMBER}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    # As a resource, Xcode compiles the catalog (the layered AppIcon brand assets) into Assets.car.
    RESOURCE "${_assets}"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${RUMBLE_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${RUMBLE_DEVELOPMENT_TEAM}"
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    # The game's 1 GB address window is zero-fill data in the executable, which port_mem.c
    # finds through the executable's header: no dead-code stripping, no debug dylib.
    XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING NO
    XCODE_ATTRIBUTE_ENABLE_DEBUG_DYLIB NO
    XCODE_GENERATE_SCHEME ON)

add_custom_command(TARGET RumbleTV POST_BUILD
    COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/embed_content.sh" "${RUMBLE_CONTENT_DIR}"
    VERBATIM)
