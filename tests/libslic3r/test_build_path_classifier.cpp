#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include "libslic3r/BuildPathClassifier.hpp"

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

struct TempDirectory {
    fs::path path = fs::temp_directory_path() / fs::unique_path("quackslicer-build-path-%%%%-%%%%");

    TempDirectory() { fs::create_directories(path); }
    ~TempDirectory()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("CMake build executables are detected independently of build folder name", "[ShellIntegration]")
{
    TempDirectory temp;
    const fs::path build_root = temp.path / "arbitrary-output-folder";
    const fs::path executable = build_root / "src" / "Release" / "quack-slicer.exe";

    fs::create_directories(executable.parent_path());
    {
        fs::ofstream cache(build_root / "CMakeCache.txt");
        cache << "CMAKE_GENERATOR=Visual Studio";
    }

    CHECK(cmake_build_root_for_executable(executable) == build_root);
}

TEST_CASE("Installed and unrelated executables are not classified as builds", "[ShellIntegration]")
{
    TempDirectory temp;
    const fs::path installed = temp.path / "Program Files" / "QuackSlicer" / "quack-slicer.exe";
    const fs::path other_exe = temp.path / "output" / "src" / "Release" / "other.exe";

    CHECK_FALSE(is_cmake_build_executable(installed));

    fs::create_directories(other_exe.parent_path());
    {
        fs::ofstream cache(temp.path / "output" / "CMakeCache.txt");
        cache << "CMAKE_GENERATOR=Visual Studio";
    }
    CHECK_FALSE(is_cmake_build_executable(other_exe));
}
