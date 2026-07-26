#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_helpers.hpp" // get access to init_print, etc

#include <algorithm>
#include <cmath>

using namespace Slic3r::Test;
using namespace Slic3r;

namespace {

constexpr double bridge_underside_z = 5.0;
constexpr double organic_object_layer_height = 0.2;

DynamicPrintConfig organic_bridge_config(
    bool independent_layer_height, double requested_top_gap, int top_interface_layers = 3)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_type", "tree(auto)" },
        { "support_style", "organic" },
        { "layer_height", organic_object_layer_height },
        { "initial_layer_print_height", organic_object_layer_height },
        { "support_top_z_distance", requested_top_gap },
        { "independent_support_layer_height", independent_layer_height },
        { "support_interface_top_layers", top_interface_layers },
        { "support_interface_spacing", 0.0 },
        { "support_threshold_angle", 30 },
        { "support_remove_small_overhang", false },
        { "max_bridge_length", 0.0 },
        { "bridge_no_support", false },
        { "support_on_build_plate_only", true },
    });
    return config;
}

void init_organic_bridge(
    Print &print, bool independent_layer_height, double requested_top_gap, int top_interface_layers = 3)
{
    init_and_process_print(
        { TestMesh::bridge }, print,
        organic_bridge_config(independent_layer_height, requested_top_gap, top_interface_layers));
}

bool layer_has_support_interface(const SupportLayer &layer)
{
    return std::any_of(
        layer.support_fills.entities.begin(),
        layer.support_fills.entities.end(),
        [](const ExtrusionEntity *entity) {
            return entity->role() == erSupportMaterialInterface;
        });
}

const SupportLayer* lowest_support_interface_layer(const PrintObject &object)
{
    const SupportLayer *lowest = nullptr;
    for (const SupportLayer *layer : object.support_layers())
        if (layer_has_support_interface(*layer) && (lowest == nullptr || layer->print_z < lowest->print_z))
            lowest = layer;
    return lowest;
}

double highest_support_interface_z(const PrintObject &object)
{
    const SupportLayer *highest = nullptr;
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer_has_support_interface(*layer) && (highest == nullptr || layer->print_z > highest->print_z))
            highest = layer;
    }
    REQUIRE(highest != nullptr);
    return highest->print_z;
}

size_t support_interface_layer_count(const PrintObject &object)
{
    const ConstSupportLayerPtrsAdaptor support_layers = object.support_layers();
    return size_t(std::count_if(support_layers.begin(), support_layers.end(),
        [](const SupportLayer *layer) {
            return layer_has_support_interface(*layer);
        }));
}

double highest_extruded_support_z(const PrintObject &object)
{
    double highest = 0.;
    for (const SupportLayer *layer : object.support_layers())
        if (! layer->support_fills.entities.empty())
            highest = std::max(highest, layer->print_z);
    return highest;
}

} // namespace

TEST_CASE("Three raft layers are created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ cube(20) }, print, {
        { "enable_support", 1 },
        { "raft_layers",    3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

TEST_CASE("Enforced support layers are generated", "[SupportMaterial]")
{
    // enforce_support_layers forces support on the first N layers even with support off.
    Slic3r::Print baseline;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, baseline, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    REQUIRE(baseline.objects().front()->support_layers().empty());

    Slic3r::Print enforced;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, enforced, {
        { "enable_support",         0 },
        { "enforce_support_layers", 100 }
    });
    REQUIRE(enforced.objects().front()->support_layers().size() > 0);
}

SCENARIO("Support layer Z honors contact distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}
	};

    GIVEN("A print object having one modelObject") {
        WHEN("Layer height = 0.2 and first layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.4 },
                { "dont_support_bridges",       false },
			});
			bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
        WHEN("Layer height = 0.2 and first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.3 },
                { "dont_support_bridges",       false },
            });
            bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
    }
}

// extrude_support once held a `static` lambda capturing `this`, so a second export in the
// same process dereferenced a returned stack frame (ASan: stack-use-after-return).
TEST_CASE("Support G-code emission survives a second slice in the same process", "[SupportMaterial][Regression]")
{
    const std::string first = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(first, "support").empty());

    const std::string second = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(second, "support").empty());
}

TEST_CASE("Organic support honors an exact top Z gap with independent layer heights", "[SupportMaterial][Regression]")
{
    const double requested_top_gap = GENERATE(0.0, 0.01, 0.05, 0.10, 0.19, 0.20, 0.25, 0.30);
    Print print;
    init_organic_bridge(print, true, requested_top_gap);

    const PrintObject &object = *print.objects().front();
    const double interface_z = highest_support_interface_z(object);
    CHECK(support_interface_layer_count(object) == 3);
    const SupportLayer *previous_interface = nullptr;
    for (const SupportLayer *layer : object.support_layers())
        if (layer_has_support_interface(*layer)) {
            if (previous_interface != nullptr)
                CHECK_THAT(layer->bottom_z(),
                    Catch::Matchers::WithinAbs(previous_interface->print_z, 1e-4));
            previous_interface = layer;
        }
    REQUIRE_THAT(interface_z, Catch::Matchers::WithinAbs(bridge_underside_z - requested_top_gap, 1e-4));
    CHECK_THAT(bridge_underside_z - interface_z,
        Catch::Matchers::WithinAbs(requested_top_gap, 1e-4));

    const double max_layer_height = object.slicing_parameters().max_suport_layer_height;
    const double min_layer_height = object.slicing_parameters().min_layer_height;
    for (const SupportLayer *layer : object.support_layers()) {
        CHECK(layer->height >= min_layer_height - EPSILON);
        CHECK(layer->height <= max_layer_height + EPSILON);
    }
    CHECK(highest_extruded_support_z(object) <=
        bridge_underside_z - requested_top_gap + EPSILON);

    const SupportLayer *lowest_interface = lowest_support_interface_layer(object);
    REQUIRE(lowest_interface != nullptr);
    double support_below_z = 0.;
    for (const SupportLayer *layer : object.support_layers())
        if (! layer->support_fills.entities.empty() && layer->print_z < lowest_interface->print_z - EPSILON)
            support_below_z = std::max(support_below_z, layer->print_z);
    CHECK_THAT(support_below_z, Catch::Matchers::WithinAbs(lowest_interface->bottom_z(), 1e-4));
}

TEST_CASE("Organic support without an interface honors an exact top Z gap", "[SupportMaterial][Regression]")
{
    const double requested_top_gap = GENERATE(0.01, 0.19);
    Print print;
    init_organic_bridge(print, true, requested_top_gap, 0);

    CHECK(support_interface_layer_count(*print.objects().front()) == 0);
    const double support_z = highest_extruded_support_z(*print.objects().front());
    REQUIRE_THAT(support_z, Catch::Matchers::WithinAbs(bridge_underside_z - requested_top_gap, 1e-4));
    CHECK_THAT(bridge_underside_z - support_z,
        Catch::Matchers::WithinAbs(requested_top_gap, 1e-4));
}

TEST_CASE("Organic support falls back when exact Z intervals are not printable", "[SupportMaterial][Regression]")
{
    DynamicPrintConfig config = organic_bridge_config(true, 0.19, 0);
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.3" },
        { "min_layer_height", "0.11" },
        { "max_layer_height", "0.15" },
    });

    Print print;
    init_and_process_print({ TestMesh::bridge }, print, config);

    const PrintObject &object = *print.objects().front();
    CHECK_THAT(object.slicing_parameters().max_suport_layer_height,
        Catch::Matchers::WithinAbs(0.15, 1e-4));
    const double support_z = highest_extruded_support_z(object);
    REQUIRE_THAT(support_z, Catch::Matchers::WithinAbs(4.8, 1e-4));
    CHECK_THAT(bridge_underside_z - support_z,
        Catch::Matchers::WithinAbs(organic_object_layer_height, 1e-4));
}

TEST_CASE("Organic support retains a quantized top Z gap without independent layer heights", "[SupportMaterial][Regression]")
{
    Print print;
    constexpr double requested_top_gap = 0.1;
    init_organic_bridge(print, false, requested_top_gap);

    const double interface_z = highest_support_interface_z(*print.objects().front());
    const double quantized_gap =
        std::round(requested_top_gap / organic_object_layer_height + EPSILON) * organic_object_layer_height;
    REQUIRE_THAT(interface_z, Catch::Matchers::WithinAbs(4.8, 1e-4));
    CHECK_THAT(bridge_underside_z - interface_z,
        Catch::Matchers::WithinAbs(quantized_gap, 1e-4));
    CHECK(highest_extruded_support_z(*print.objects().front()) <= interface_z + EPSILON);
}

TEST_CASE("Organic exact Z extends a small branch tip to a contact without an interface", "[SupportMaterial][Regression]")
{
    DynamicPrintConfig config = organic_bridge_config(true, 0.001, 0);
    config.set_deserialize_strict({
        { "layer_height", "0.1" },
        { "initial_layer_print_height", "0.1" },
        { "nozzle_diameter", "0.3" },
        { "support_line_width", "0.101" },
        { "tree_support_tip_diameter", "0.105" },
        { "tree_support_branch_diameter_organic", "0.21" },
        { "min_layer_height", "0.02" },
        { "max_layer_height", "0.04" },
    });

    Print print;
    init_and_process_print({ TestMesh::bridge }, print, config);

    const PrintObject &object = *print.objects().front();
    CHECK(support_interface_layer_count(object) == 0);
    REQUIRE_THAT(highest_extruded_support_z(object),
        Catch::Matchers::WithinAbs(bridge_underside_z - 0.001, 1e-4));
}
