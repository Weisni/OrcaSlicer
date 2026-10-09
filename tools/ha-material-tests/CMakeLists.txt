cmake_minimum_required(VERSION 3.18)
project(ha_material_contract_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HA_DEPENDENCY_ROOT "${REPOSITORY_ROOT}" CACHE PATH "Configured QuackSlicer build root")
set(HA_DEPS "${HA_DEPENDENCY_ROOT}/deps/build/OrcaSlicer_dep/usr/local")
add_executable(ha_material_contract_tests
    "${REPOSITORY_ROOT}/tests/libslic3r/test_ha_material_profile_payload.cpp"
    "${REPOSITORY_ROOT}/tests/libslic3r/test_ha_material_refresh.cpp"
    "${REPOSITORY_ROOT}/tests/libslic3r/test_ha_material_local_override.cpp")
target_sources(ha_material_contract_tests PRIVATE "${REPOSITORY_ROOT}/tests/libslic3r/test_ha_material_profile_cache.cpp")
if(MSVC)
    target_compile_options(ha_material_contract_tests PRIVATE /utf-8 /MP1)
endif()
target_include_directories(ha_material_contract_tests PRIVATE
    "${REPOSITORY_ROOT}/src" "${REPOSITORY_ROOT}/deps_src" "${REPOSITORY_ROOT}/tests/catch2/src"
    "${HA_DEPENDENCY_ROOT}/build/tests/catch2/generated-includes"
    "${HA_DEPS}/include")
find_library(HA_CATCH2_LIBRARY NAMES Catch2 HINTS
    "${HA_DEPENDENCY_ROOT}/build/tests/catch2/src/Release" "${HA_DEPENDENCY_ROOT}/build/tests/catch2/src" REQUIRED)
find_library(HA_CATCH2_MAIN_LIBRARY NAMES Catch2Main HINTS
    "${HA_DEPENDENCY_ROOT}/build/tests/catch2/src/Release" "${HA_DEPENDENCY_ROOT}/build/tests/catch2/src" REQUIRED)
find_library(HA_CRYPTO_LIBRARY NAMES crypto libcrypto HINTS "${HA_DEPS}/lib" REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(ha_material_contract_tests PRIVATE
    "${HA_CATCH2_LIBRARY}" "${HA_CATCH2_MAIN_LIBRARY}" "${HA_CRYPTO_LIBRARY}" Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(ha_material_contract_tests PRIVATE ws2_32 crypt32)
endif()
enable_testing()
add_test(NAME ha_material_contract_tests COMMAND ha_material_contract_tests)
