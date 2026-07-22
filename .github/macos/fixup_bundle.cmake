include(BundleUtilities)

# Xcode generator puts the build type in the build directory; the Ninja
# release CI does not.
set(BUILD_PREFIX "")
if (CMAKE_GENERATOR STREQUAL "Xcode")
    set(BUILD_PREFIX "${CMAKE_BUILD_TYPE}/")
endif()

set(APPS "${BUILD_PREFIX}WaveRace64Recomp.app/Contents/MacOS/WaveRace64Recomp")
set(DIRS "${BUILD_PREFIX}WaveRace64Recomp.app/Contents/Frameworks" "/opt/local/lib")

file(REAL_PATH ${APPS} APPS)

set(RESOLVED_DIRS "")
foreach(DIR IN LISTS DIRS)
    string(REPLACE "~" "$ENV{HOME}" DIR "${DIR}")
    if(EXISTS "${DIR}")
        file(REAL_PATH "${DIR}" RESOLVED_DIR)
        list(APPEND RESOLVED_DIRS "${RESOLVED_DIR}")
    endif()
endforeach()

message(STATUS "Bundle fixup paths:")
message(STATUS "  App: ${APPS}")
message(STATUS "  Search dirs: ${RESOLVED_DIRS}")

# Copies the MacPorts SDL2/freetype dylibs into Contents/Frameworks and
# rewrites the binary's load paths so the .app is self-contained.
fixup_bundle("${APPS}" "" "${RESOLVED_DIRS}")
