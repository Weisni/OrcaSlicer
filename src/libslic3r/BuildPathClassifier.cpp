#include "BuildPathClassifier.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>

namespace Slic3r {

std::optional<boost::filesystem::path> cmake_build_root_for_executable(
    const boost::filesystem::path &executable)
{
    if (!boost::algorithm::iequals(executable.filename().string(), "quack-slicer.exe"))
        return std::nullopt;

    boost::filesystem::path directory = executable.parent_path();
    while (!directory.empty() && directory != directory.root_path()) {
        boost::system::error_code ec;
        if (boost::filesystem::is_regular_file(directory / "CMakeCache.txt", ec))
            return directory;
        directory = directory.parent_path();
    }

    return std::nullopt;
}

} // namespace Slic3r
