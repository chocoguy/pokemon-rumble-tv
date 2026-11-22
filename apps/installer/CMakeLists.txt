# Rumble TV Installer: the app players download (signed with a Developer ID and notarized by
# tools/release.sh). It builds the game from their own game files (the WAD they installed in
# Dolphin) at a GitHub release tag and installs it on their Apple TVs, through tools/tvos/port;
# Sparkle updates the installer itself. Built with the Xcode generator only (it embeds
# Sparkle.framework).

include(FetchContent)

# Sparkle, pinned: the release tarball's SHA-256 is GitHub's digest for it.
FetchContent_Declare(sparkle
    URL https://github.com/sparkle-project/Sparkle/releases/download/2.10.1/Sparkle-2.10.1.tar.xz
    URL_HASH SHA256=1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(sparkle)
set(RUMBLE_SPARKLE_DIR "${sparkle_SOURCE_DIR}" CACHE INTERNAL "Sparkle's framework and tools (bin/)")

set(RUMBLE_INSTALLER_BUNDLE_ID "com.vanillacoffeesoft.RumbleTVInstaller"
    CACHE STRING "Bundle identifier for Rumble TV Installer")
# Ad hoc for local builds; tools/release.sh signs with "Developer ID Application".
set(RUMBLE_INSTALLER_SIGN_IDENTITY "-" CACHE STRING "Code signing identity for Rumble TV Installer")
# Sparkle's public EdDSA key (generate_keys prints it). Empty in local builds, which then never
# offer to update themselves.
set(RUMBLE_SPARKLE_PUBLIC_KEY "" CACHE STRING "Sparkle's public EdDSA key for the installer's updates")
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
add_executable(RumbleTVInstaller MACOSX_BUNDLE ${_sources} AppIcon.icns)
set_source_files_properties(AppIcon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

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
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Manual
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${RUMBLE_INSTALLER_SIGN_IDENTITY}"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${RUMBLE_INSTALLER_TEAM}"
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
    XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "--timestamp"
    XCODE_ATTRIBUTE_FRAMEWORK_SEARCH_PATHS "${sparkle_SOURCE_DIR}"
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/../Frameworks"
    XCODE_EMBED_FRAMEWORKS "${sparkle_SOURCE_DIR}/Sparkle.framework"
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES
    XCODE_EMBED_FRAMEWORKS_REMOVE_HEADERS_ON_COPY YES
    XCODE_GENERATE_SCHEME ON)
if(RUMBLE_INSTALLER_SIGN_IDENTITY STREQUAL "-")
    # No timestamp server for ad hoc signatures.
    set_target_properties(RumbleTVInstaller PROPERTIES XCODE_ATTRIBUTE_OTHER_CODE_SIGN_FLAGS "")
endif()

target_link_libraries(RumbleTVInstaller PRIVATE
    "${sparkle_SOURCE_DIR}/Sparkle.framework"
    "-framework AppKit"
    "-framework SwiftUI")
