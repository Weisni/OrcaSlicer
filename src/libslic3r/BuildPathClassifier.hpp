#pragma once

#include <boost/filesystem/path.hpp>

#include <optional>

namespace Slic3r {

// Returns the enclosing CMake build root for an application executable.
// The build directory itself may have any name; CMakeCache.txt is the marker.
std::optional<boost::filesystem::path> cmake_build_root_for_executable(
    const boost::filesystem::path &executable);

inline bool is_cmake_build_executable(const boost::filesystem::path &executable)
{
    return cmake_build_root_for_executable(executable).has_value();
}

} // namespace Slic3r
