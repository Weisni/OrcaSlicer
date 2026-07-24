# Locate a system SQLite installation for builds that do not use the bundled
# dependency prefix. CMake only started shipping FindSQLite3 after this
# project's minimum supported CMake version.

find_path(SQLite3_INCLUDE_DIR NAMES sqlite3.h)
find_library(SQLite3_LIBRARY_RELEASE NAMES sqlite3)
find_library(SQLite3_LIBRARY_DEBUG NAMES sqlite3d sqlite3)

if (SQLite3_INCLUDE_DIR AND EXISTS "${SQLite3_INCLUDE_DIR}/sqlite3.h")
    file(STRINGS "${SQLite3_INCLUDE_DIR}/sqlite3.h" _sqlite_version_line
        REGEX "^#define[ \t]+SQLITE_VERSION[ \t]+\"[0-9.]+\"")
    string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1"
        SQLite3_VERSION "${_sqlite_version_line}")
    unset(_sqlite_version_line)
endif ()

include(SelectLibraryConfigurations)
select_library_configurations(SQLite3)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SQLite3
    REQUIRED_VARS SQLite3_LIBRARY SQLite3_INCLUDE_DIR
    VERSION_VAR SQLite3_VERSION
)

if (SQLite3_FOUND)
    set(SQLite3_INCLUDE_DIRS "${SQLite3_INCLUDE_DIR}")
    set(SQLite3_LIBRARIES "${SQLite3_LIBRARY}")

    if (NOT TARGET SQLite3::SQLite3)
        add_library(SQLite3::SQLite3 UNKNOWN IMPORTED)
        set_target_properties(SQLite3::SQLite3 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIR}"
        )
        if (SQLite3_LIBRARY_RELEASE)
            set_property(TARGET SQLite3::SQLite3 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
            set_target_properties(SQLite3::SQLite3 PROPERTIES
                IMPORTED_LOCATION_RELEASE "${SQLite3_LIBRARY_RELEASE}"
                MAP_IMPORTED_CONFIG_MINSIZEREL Release
                MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
        endif ()
        if (SQLite3_LIBRARY_DEBUG)
            set_property(TARGET SQLite3::SQLite3 APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
            set_target_properties(SQLite3::SQLite3 PROPERTIES
                IMPORTED_LOCATION_DEBUG "${SQLite3_LIBRARY_DEBUG}")
        endif ()
        if (SQLite3_LIBRARY AND NOT SQLite3_LIBRARY_RELEASE AND NOT SQLite3_LIBRARY_DEBUG)
            set_target_properties(SQLite3::SQLite3 PROPERTIES
                IMPORTED_LOCATION "${SQLite3_LIBRARY}")
        endif ()

        find_package(Threads REQUIRED)
        set_property(TARGET SQLite3::SQLite3 APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES Threads::Threads)
        if (UNIX)
            set_property(TARGET SQLite3::SQLite3 APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES "m;${CMAKE_DL_LIBS}")
        endif ()
    endif ()
endif ()

mark_as_advanced(
    SQLite3_INCLUDE_DIR
    SQLite3_LIBRARY_RELEASE
    SQLite3_LIBRARY_DEBUG
)
