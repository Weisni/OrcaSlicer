#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/ObjColorUtils.hpp"

TEST_CASE("Unused appended filament mappings are removed", "[ObjColorUtils]")
{
    std::vector<int> mappings{1, 5, 5, 3, 2};

    const std::vector<size_t> color_sources = compact_new_filament_mappings(2, mappings);

    REQUIRE(color_sources == std::vector<size_t>{3, 1});
    REQUIRE(mappings == std::vector<int>{1, 4, 4, 3, 2});
}

TEST_CASE("A selected high appended filament mapping is compacted", "[ObjColorUtils]")
{
    std::vector<int> mappings{4, 1, 2};

    const std::vector<size_t> color_sources = compact_new_filament_mappings(2, mappings);

    REQUIRE(color_sources == std::vector<size_t>{0});
    REQUIRE(mappings == std::vector<int>{3, 1, 2});
}

TEST_CASE("Existing and unassigned filament mappings remain unchanged", "[ObjColorUtils]")
{
    std::vector<int> mappings{0, 1, 2, -1};
    const std::vector<int> original_mappings = mappings;

    const std::vector<size_t> color_sources = compact_new_filament_mappings(2, mappings);

    REQUIRE(color_sources.empty());
    REQUIRE(mappings == original_mappings);
}

TEST_CASE("Replacement filament mappings start at the first slot", "[ObjColorUtils]")
{
    std::vector<int> mappings{3, 1, 3, 2};

    const std::vector<size_t> color_sources = compact_new_filament_mappings(0, mappings);

    REQUIRE(color_sources == std::vector<size_t>{1, 3, 0});
    REQUIRE(mappings == std::vector<int>{3, 1, 3, 2});
}
