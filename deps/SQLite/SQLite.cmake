orcaslicer_add_cmake_project(SQLite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip
    URL_HASH SHA3_256=d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt.in
        <SOURCE_DIR>/CMakeLists.txt
)

if (MSVC)
    add_debug_dep(dep_SQLite)
endif ()
