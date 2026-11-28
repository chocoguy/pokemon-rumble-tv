# Rumble TV Installer: the app players download (signed with a Developer ID and notarized by
# tools/release.sh). It builds the game from their own game files (the WAD they installed in
# Dolphin) at a GitHub release tag and installs it on their Apple TVs, through tools/tvos/port. It
# doesn't update itself: a new major version is a new installer (docs/release.md). Its version is the
# release it was built for (RUMBLE_VERSION, which tools/release.sh passes only for N.0.0), and it
# builds the releases of that major version. Built with the Xcode generator only.

set(RUMBLE_INSTALLER_BUNDLE_ID "com.vanillacoffeesoft.RumbleTVInstaller"
    CACHE STRING "Bundle identifier for Rumble TV Installer")
# Ad hoc for local builds; tools/release.sh signs with "Developer ID Application".
set(RUMBLE_INSTALLER_SIGN_IDENTITY "-" CACHE STRING "Code signing identity for Rumble TV Installer")
# The Developer ID team (tools/release.sh passes it); empty for ad hoc builds.
set(RUMBLE_INSTALLER_TEAM "" CACHE STRING "Apple developer Team ID that signs the installer")

set(_sources
    Sources/AppModel.swift
    Sources/Doctor.swift
    Sources/InstallerApp.swift
    Sources/PortEvent.swift
    Sources/Releases.swift
    Sources/SemVer.swift
    Sources/Shell.swift
    Sources/Source.swift
    Sources/Views.swift
    Sources/Workspace.swift)
# Assets.xcassets holds AccentColor (#FFB3EA), the app's accent.
add_executable(RumbleTVInstaller MACOSX_BUNDLE ${_sources} AppIcon.icns Assets.xcassets)
set_source_files_properties(AppIcon.icns Assets.xcassets PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

set_target_properties(RumbleTVInstaller PROPERTIES
    OUTPUT_NAME "Rumble TV Installer"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${RUMBLE_INSTALLER_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "Rumble TV Installer"
    MACOSX_BUNDLE_BUNDLE_VERSION "${RUMBLE_BUILD_NUMBER}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE AppIcon
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${RUMBLE_INSTALLER_BUNDLE_ID}"
    XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME AccentColor
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${RUMBLE_INSTALLER_SIGN_IDENTITY}"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${RUMBLE_INSTALLER_TEAM}"
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
    XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--timestamp"
    XCODE_GENERATE_SCHEME ON)
if(RUMBLE_INSTALLER_SIGN_IDENTITY STREQUAL "-")
    # No timestamp server for ad hoc signatures.
    set_target_properties(RumbleTVInstaller PROPERTIES XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "")
endif()

target_link_libraries(RumbleTVInstaller PRIVATE
    "-framework AppKit"
    "-framework SwiftUI")
