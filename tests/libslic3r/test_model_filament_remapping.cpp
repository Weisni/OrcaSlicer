#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

TEST_CASE("Deleting a filament remaps an explicit volume base color", "[Model][FilamentRemapping]")
{
    Model model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(10., 10., 10.));

    SECTION("a following filament id is shifted down") {
        volume->config.set("extruder", 5);

        volume->update_extruder_count_when_delete_filament(4, 1, 0);

        REQUIRE(volume->config.has("extruder"));
        REQUIRE(volume->config.extruder() == 4);
    }

    SECTION("the deleted base color falls back to the first filament") {
        volume->config.set("extruder", 3);

        volume->update_extruder_count_when_delete_filament(4, 3, 0);

        REQUIRE(volume->config.has("extruder"));
        REQUIRE(volume->config.extruder() == 1);
    }

    SECTION("an explicit replacement is retained") {
        volume->config.set("extruder", 3);

        volume->update_extruder_count_when_delete_filament(4, 3, 2);

        REQUIRE(volume->config.has("extruder"));
        REQUIRE(volume->config.extruder() == 2);
    }
}
