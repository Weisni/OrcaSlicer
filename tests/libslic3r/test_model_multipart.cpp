#include "libslic3r/Model.hpp"

#include <catch2/catch_all.hpp>

using namespace Slic3r;

static BoundingBoxf3 world_bounding_box(const ModelObject& object, const ModelVolume& volume)
{
    return volume.mesh().transformed_bounding_box(object.instances.front()->get_matrix() * volume.get_matrix());
}

TEST_CASE("Selected objects become one multipart object without moving the others", "[Model][Multipart]")
{
    Model model;

    ModelObject* base = model.add_object("Donut", "donut.3mf", make_cube(20., 20., 10.));
    base->add_instance()->set_offset(Vec3d(10., 20., 5.));

    ModelObject* sprinkle = model.add_object("Sprinkle", "donut.3mf", make_cube(2., 2., 2.));
    sprinkle->add_instance()->set_offset(Vec3d(14., 27., 18.));

    ModelObject* separate = model.add_object("Decoration", "donut.3mf", make_cube(4., 4., 4.));
    separate->add_instance()->set_offset(Vec3d(80., 90., 7.));

    const BoundingBoxf3 expected_assembly = [] (const ModelObject& first, const ModelObject& second) {
        BoundingBoxf3 bounds = first.bounding_box_exact();
        bounds.merge(second.bounding_box_exact());
        return bounds;
    }(*base, *sprinkle);
    const BoundingBoxf3 expected_separate = separate->bounding_box_exact();

    model.convert_multipart_object({0, 1}, 4);

    REQUIRE(model.objects.size() == 2);
    REQUIRE(model.objects[0]->volumes.size() == 2);
    model.objects[0]->add_instance();
    CHECK(model.objects[1]->name == "Decoration");
    CHECK(model.objects[0]->bounding_box_exact().min.isApprox(expected_assembly.min));
    CHECK(model.objects[0]->bounding_box_exact().max.isApprox(expected_assembly.max));
    CHECK(model.objects[1]->bounding_box_exact().min.isApprox(expected_separate.min));
    CHECK(model.objects[1]->bounding_box_exact().max.isApprox(expected_separate.max));
}

TEST_CASE("Multipart assembly keeps relative Z positions when placed on the bed", "[Model][Multipart][Regression]")
{
    Model model;

    ModelObject* base = model.add_object("Donut", "donut.3mf", make_cube(20., 20., 10.));
    base->add_instance()->set_offset(Vec3d(0., 0., 5.));

    ModelObject* sprinkle = model.add_object("Sprinkle", "donut.3mf", make_cube(2., 2., 2.));
    sprinkle->add_instance()->set_offset(Vec3d(4., 7., 18.));

    const double z_distance_before = sprinkle->bounding_box_exact().center().z() - base->bounding_box_exact().center().z();

    model.convert_multipart_object({0, 1}, 4);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 2);
    model.objects.front()->add_instance();

    const double volume_z_distance_before =
        model.objects.front()->volumes[1]->mesh().transformed_bounding_box(model.objects.front()->volumes[1]->get_matrix()).center().z() -
        model.objects.front()->volumes[0]->mesh().transformed_bounding_box(model.objects.front()->volumes[0]->get_matrix()).center().z();
    model.objects.front()->ensure_on_bed(false);
    const double volume_z_distance_after =
        model.objects.front()->volumes[1]->mesh().transformed_bounding_box(model.objects.front()->volumes[1]->get_matrix()).center().z() -
        model.objects.front()->volumes[0]->mesh().transformed_bounding_box(model.objects.front()->volumes[0]->get_matrix()).center().z();

    CHECK(volume_z_distance_before == Catch::Approx(z_distance_before));
    CHECK(volume_z_distance_after == Catch::Approx(z_distance_before));
    CHECK(model.objects.front()->min_z() == Catch::Approx(0.));
}

TEST_CASE("Multiple group numbers create independent multipart assemblies", "[Model][Multipart]")
{
    Model model;
    for (size_t object_idx = 0; object_idx < 5; ++object_idx) {
        ModelObject* object = model.add_object(("Part " + std::to_string(object_idx)).c_str(), "groups.3mf", make_cube(2., 2., 2.));
        object->add_instance()->set_offset(Vec3d(double(object_idx * 10), 0., double(object_idx * 3)));
    }

    BoundingBoxf3 expected_group_1 = model.objects[0]->bounding_box_exact();
    expected_group_1.merge(model.objects[2]->bounding_box_exact());
    BoundingBoxf3 expected_group_2 = model.objects[1]->bounding_box_exact();
    expected_group_2.merge(model.objects[3]->bounding_box_exact());

    model.convert_multipart_objects({1, 2, 1, 2, 0}, 4);

    REQUIRE(model.objects.size() == 3);
    REQUIRE(model.objects[0]->volumes.size() == 2);
    REQUIRE(model.objects[1]->volumes.size() == 2);
    REQUIRE(model.objects[2]->volumes.size() == 1);
    model.objects[0]->add_instance();
    model.objects[1]->add_instance();
    CHECK(model.objects[0]->bounding_box_exact().min.isApprox(expected_group_1.min));
    CHECK(model.objects[0]->bounding_box_exact().max.isApprox(expected_group_1.max));
    CHECK(model.objects[1]->bounding_box_exact().min.isApprox(expected_group_2.min));
    CHECK(model.objects[1]->bounding_box_exact().max.isApprox(expected_group_2.max));
    CHECK(model.objects[2]->name == "Part 4");
}

TEST_CASE("Parts moved between assemblies keep their world positions", "[Model][AssemblyTransfer]")
{
    Model model;
    ModelObject* source = model.add_object("Source", "parts.3mf", make_cube(2., 2., 2.));
    source->add_instance()->set_offset(Vec3d(20., 30., 5.));
    source->add_volume(make_cube(3., 3., 3.))->set_offset(Vec3d(4., 5., 6.));
    source->add_volume(make_cube(4., 4., 4.))->set_offset(Vec3d(8., 9., 10.));

    ModelObject* target = model.add_object("Target", "parts.3mf", make_cube(5., 5., 5.));
    target->add_instance()->set_offset(Vec3d(-40., 12., 3.));

    const BoundingBoxf3 expected_first = world_bounding_box(*source, *source->volumes[1]);
    const BoundingBoxf3 expected_second = world_bounding_box(*source, *source->volumes[2]);

    REQUIRE(model.move_volumes_to_object(0, {1, 2}, 1) == 1);
    REQUIRE(source->volumes.size() == 1);
    REQUIRE(target->volumes.size() == 3);
    CHECK(world_bounding_box(*target, *target->volumes[1]).min.isApprox(expected_first.min));
    CHECK(world_bounding_box(*target, *target->volumes[1]).max.isApprox(expected_first.max));
    CHECK(world_bounding_box(*target, *target->volumes[2]).min.isApprox(expected_second.min));
    CHECK(world_bounding_box(*target, *target->volumes[2]).max.isApprox(expected_second.max));
}

TEST_CASE("Parts can form a new assembly or leave as individual objects", "[Model][AssemblyTransfer]")
{
    Model model;
    ModelObject* source = model.add_object("Source", "parts.3mf", make_cube(2., 2., 2.));
    source->add_instance()->set_offset(Vec3d(11., 13., 17.));
    source->add_volume(make_cube(3., 3., 3.))->set_offset(Vec3d(5., 7., 9.));
    source->add_volume(make_cube(4., 4., 4.))->set_offset(Vec3d(12., 14., 16.));
    source->add_volume(make_cube(5., 5., 5.))->set_offset(Vec3d(20., 22., 24.));

    const BoundingBoxf3 expected_assembly_first = world_bounding_box(*source, *source->volumes[1]);
    const BoundingBoxf3 expected_assembly_second = world_bounding_box(*source, *source->volumes[2]);
    const size_t assembly_idx = model.move_volumes_to_new_object(0, {1, 2}, "Assembly");

    REQUIRE(assembly_idx == 1);
    REQUIRE(model.objects[assembly_idx]->volumes.size() == 2);
    CHECK(world_bounding_box(*model.objects[assembly_idx], *model.objects[assembly_idx]->volumes[0]).min.isApprox(expected_assembly_first.min));
    CHECK(world_bounding_box(*model.objects[assembly_idx], *model.objects[assembly_idx]->volumes[1]).max.isApprox(expected_assembly_second.max));

    const BoundingBoxf3 expected_extracted = world_bounding_box(*source, *source->volumes[1]);
    const std::vector<size_t> extracted = model.extract_volumes_to_objects(0, {1});
    REQUIRE(extracted.size() == 1);
    REQUIRE(model.objects[extracted.front()]->volumes.size() == 1);
    CHECK(world_bounding_box(*model.objects[extracted.front()], *model.objects[extracted.front()]->volumes.front()).min.isApprox(expected_extracted.min));
    CHECK(world_bounding_box(*model.objects[extracted.front()], *model.objects[extracted.front()]->volumes.front()).max.isApprox(expected_extracted.max));
}

TEST_CASE("Placing a multi-object model on the bed preserves relative positions", "[Model][Multipart][Regression]")
{
    Model model;
    ModelObject* lower = model.add_object("Lower", "standard.3mf", make_cube(20., 20., 10.));
    lower->add_instance()->set_offset(Vec3d(10., 20., 7.));
    ModelObject* upper = model.add_object("Upper", "standard.3mf", make_cube(2., 2., 2.));
    upper->add_instance()->set_offset(Vec3d(14., 27., 25.));

    const Vec3d relative_center_before = upper->bounding_box_exact().center() - lower->bounding_box_exact().center();
    model.place_on_bed_preserving_relative_positions();
    const Vec3d relative_center_after = upper->bounding_box_exact().center() - lower->bounding_box_exact().center();

    CHECK(model.bounding_box_exact().min.z() == Catch::Approx(0.));
    CHECK(relative_center_after.isApprox(relative_center_before));
}
