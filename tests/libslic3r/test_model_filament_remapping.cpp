#include <catch2/catch_all.hpp>

#include <algorithm>

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

TEST_CASE("Imported single-part objects expose their mapped base filament", "[Model][FilamentRemapping][3mfColorMapping]")
{
    Model model;
    const std::vector<unsigned char> filament_ids{7, 3, 5, 1, 6, 4, 2};
    for (const unsigned char filament_id : filament_ids) {
        ModelObject* object = model.add_object();
        ModelVolume* volume = object->add_volume(make_cube(10., 10., 10.));
        // Standard 3MF mapping sets the volume base before applying face colors.
        volume->config.set("extruder", filament_id);
    }

    REQUIRE(Model::obj_import_color_deal(filament_ids, std::nullopt, &model, [](int) { return false; },
        [&model](int volume_id, int, int) {
            for (const ModelObject* object : model.objects)
                if (object->volumes.front()->id().id == volume_id)
                    return object->volumes.front()->config.extruder();
            return 1;
        }));

    for (size_t i = 0; i < filament_ids.size(); ++i) {
        CAPTURE(i);
        const ModelObject* object = model.objects[i];
        REQUIRE(object->config.has("extruder"));
        CHECK(object->config.extruder() == filament_ids[i]);
        CHECK(object->volumes.front()->extruder_id() == filament_ids[i]);
        CHECK(object->volumes.front()->mmu_segmentation_facets.empty());
    }
}

TEST_CASE("Imported multipart objects retain their default and individual base filaments", "[Model][FilamentRemapping][3mfColorMapping]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->config.set("extruder", 2);
    object->add_volume(make_cube(10., 10., 10.))->config.set("extruder", 3);
    object->add_volume(make_cube(10., 10., 10.))->config.set("extruder", 4);

    REQUIRE(Model::obj_import_color_deal({3, 4}, std::nullopt, &model, [](int) { return false; },
        [object](int volume_id, int, int) {
            return object->volumes.front()->id().id == volume_id ? 3 : 4;
        }));

    CHECK(object->config.extruder() == 2);
    CHECK(object->volumes[0]->extruder_id() == 3);
    CHECK(object->volumes[1]->extruder_id() == 4);
}

TEST_CASE("Imported face painting retains the mapped single-part base filament", "[Model][FilamentRemapping][3mfColorMapping]")
{
    Model model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(10., 10., 10.));
    volume->config.set("extruder", 3);

    REQUIRE(Model::obj_import_color_deal({3, 6}, std::nullopt, &model, [](int) { return false; },
        [](int, int triangle_id, int) { return triangle_id == 0 ? 6 : 3; }));

    REQUIRE(object->config.has("extruder"));
    CHECK(object->config.extruder() == 3);
    CHECK(volume->extruder_id() == 3);
    CHECK(volume->get_extruders_from_multi_material_painting() == std::vector<size_t>{5});
}

TEST_CASE("OBJ color import retains its explicit first filament", "[Model][FilamentRemapping][3mfColorMapping]")
{
    Model model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(10., 10., 10.));
    volume->config.set("extruder", 3);

    REQUIRE(Model::obj_import_color_deal({3}, 1, &model, [](int) { return false; },
        [](int, int, int) { return 3; }));

    CHECK(object->config.extruder() == 1);
    CHECK(volume->extruder_id() == 1);
    CHECK(volume->get_extruders_from_multi_material_painting() == std::vector<size_t>{2});
}

TEST_CASE("Replacing imported color mappings updates object defaults and painted faces", "[Model][FilamentRemapping][3mfColorMapping]")
{
    Model model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(10., 10., 10.));
    volume->config.set("extruder", 3);
    REQUIRE(Model::obj_import_color_deal({3, 6}, std::nullopt, &model, [](int) { return false; },
        [](int, int triangle_id, int) { return triangle_id == 0 ? 6 : 3; }));

    // A second Replace applies a different palette to the same preview model.
    volume->config.set("extruder", 7);
    REQUIRE(Model::obj_import_color_deal({7, 2}, std::nullopt, &model, [](int) { return false; },
        [](int, int triangle_id, int) { return triangle_id == 0 ? 2 : 7; }));

    REQUIRE(object->config.has("extruder"));
    CHECK(object->config.extruder() == 7);
    CHECK(volume->extruder_id() == 7);
    CHECK(volume->get_extruders_from_multi_material_painting() == std::vector<size_t>{1});
}

TEST_CASE("Resetting facet annotations clears their used states before repainting", "[Model][FilamentRemapping][FacetsAnnotation]")
{
    const int annotation_index = GENERATE(0, 1, 2, 3);
    Model model;
    ModelVolume* volume = model.add_object()->add_volume(make_cube(10., 10., 10.));
    const std::vector<FacetsAnnotation*> annotations{
        &volume->mmu_segmentation_facets, &volume->supported_facets,
        &volume->seam_facets, &volume->fuzzy_skin_facets};
    FacetsAnnotation& annotation = *annotations[annotation_index];
    // Serialized leaf states 1 and 2 are shared by all facet annotations.
    annotation.set_triangle_from_string(0, "4");
    REQUIRE(annotation.get_data().used_states[1]);

    annotation.reset();

    CHECK(annotation.empty());
    CHECK(annotation.get_data().bitstream.empty());
    CHECK(std::none_of(annotation.get_data().used_states.begin(), annotation.get_data().used_states.end(),
        [](bool used) { return used; }));
    annotation.set_triangle_from_string(0, "8");
    CHECK_FALSE(annotation.get_data().used_states[1]);
    CHECK(annotation.get_data().used_states[2]);
}
