# Exercise the generated Windows resource with a fork version distinct from
# the engine/build identifier, including a prerelease suffix in display text.
set(SoftFever_VERSION "9.8.7-beta.1")
set(ORCA_VERSION_MAJOR 9)
set(ORCA_VERSION_MINOR 8)
set(ORCA_VERSION_PATCH 7)
set(SLIC3R_VERSION "02.08.01.55")
set(SLIC3R_BUILD_ID "unrelated-engine-build")
set(SLIC3R_APP_NAME "QuackSlicer")
set(SLIC3R_RESOURCES_DIR "unused-test-resources")
set(output "${CMAKE_CURRENT_BINARY_DIR}/windows-version-resource-test.rc")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../src/dev-utils/platform/msw/OrcaSlicer.rc.in"
               "${output}" @ONLY)
file(READ "${output}" resource)
foreach(expected IN ITEMS
        "FILEVERSION 9,8,7,0"
        "PRODUCTVERSION 9,8,7,0"
        "VALUE \"FileVersion\", \"9.8.7-beta.1\""
        "VALUE \"ProductVersion\", \"9.8.7-beta.1\"")
    string(FIND "${resource}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Generated Windows resource does not expose fork version: ${expected}")
    endif()
endforeach()
file(REMOVE "${output}")
message(STATUS "Windows resource exposes the fork version independently of engine metadata")
