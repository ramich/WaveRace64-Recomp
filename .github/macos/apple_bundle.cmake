# macOS .app bundle for WaveRace64Recomp (adapted from Zelda64Recomp /
# BanjoRecomp). Turns the executable into a self-contained .app: MacPorts
# SDL2/freetype dylibs bundled into Contents/Frameworks, assets under
# Contents/Resources, ad-hoc code signature. arm64 only.
#
# NOTE: unlike Zelda we do NOT use the max_prot ld64 wrapper — WR64's
# recompiled code is AOT and its mods are texture packs only (no runtime
# function patching / JIT), so no writable+executable segments are needed.
# The entitlements are kept for parity/safety.

set(ENTITLEMENTS_FILE ${CMAKE_SOURCE_DIR}/.github/macos/entitlements.plist)

set_target_properties(WaveRace64Recomp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "WaveRace64Recomp"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.github.waverace64recomp"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${ENTITLEMENTS_FILE}
)

# Build an .icns from the icon PNG (best-effort: iconutil is macOS-only and
# the source PNG may not be a perfect power-of-two, so a failure here must
# not break the build — the bundle just falls back to a generic icon).
set(ICON_SOURCE ${CMAKE_SOURCE_DIR}/resources/wr64_icon_preview.png)
if (EXISTS ${ICON_SOURCE})
    set(ICONSET_DIR ${CMAKE_BINARY_DIR}/AppIcon.iconset)
    set(ICNS_FILE ${CMAKE_BINARY_DIR}/resources/AppIcon.icns)

    add_custom_command(
            OUTPUT ${ICNS_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${ICONSET_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy ${ICON_SOURCE} ${ICONSET_DIR}/icon_512x512.png
            COMMAND ${CMAKE_COMMAND} -E copy ${ICON_SOURCE} ${ICONSET_DIR}/icon_512x512@2x.png
            COMMAND iconutil -c icns ${ICONSET_DIR} -o ${ICNS_FILE} || ${CMAKE_COMMAND} -E touch ${ICNS_FILE}
            COMMENT "Creating macOS app icon (.icns)"
            VERBATIM
    )
    add_custom_target(create_icns ALL DEPENDS ${ICNS_FILE})
    set_source_files_properties(${ICNS_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(WaveRace64Recomp PRIVATE ${ICNS_FILE})
    add_dependencies(WaveRace64Recomp create_icns)
endif()

configure_file(${CMAKE_SOURCE_DIR}/.github/macos/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)

# Post-build: bundle the dylibs, copy assets, set rpath, ad-hoc sign.
add_custom_command(TARGET WaveRace64Recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -D CMAKE_BUILD_TYPE=$<CONFIG> -D CMAKE_GENERATOR=${CMAKE_GENERATOR} -P ${CMAKE_SOURCE_DIR}/.github/macos/fixup_bundle.cmake

    # Copy launcher assets into the bundle (drop the scss source dir).
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${CMAKE_BINARY_DIR}/temp_assets
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/temp_assets/scss
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/temp_assets $<TARGET_BUNDLE_DIR:WaveRace64Recomp>/Contents/Resources/assets
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/temp_assets

    COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks/" $<TARGET_BUNDLE_DIR:WaveRace64Recomp>/Contents/MacOS/WaveRace64Recomp

    COMMAND codesign --verbose=4 --options=runtime --no-strict --sign - --entitlements ${ENTITLEMENTS_FILE} --deep --force $<TARGET_BUNDLE_DIR:WaveRace64Recomp>

    COMMENT "Performing post-build steps for macOS bundle"
    VERBATIM
)
