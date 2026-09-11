// Tree supports by Thomas Rahm, losely based on Tree Supports by CuraEngine.
// Original source of Thomas Rahm's tree supports:
// https://github.com/ThomasRahm/CuraEngine
//
// Original CuraEngine copyright:
// Copyright (c) 2021 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef slic3r_TreeSupportCommon_hpp
#define slic3r_TreeSupportCommon_hpp

#include "../libslic3r.h"
#include "../Exception.hpp"
#include "../Polygon.hpp"
#include "SupportCommon.hpp"

#include <algorithm>
#include <string_view>

namespace Slic3r
{
    // The number of vertices in each circle.
    static constexpr const size_t SUPPORT_TREE_CIRCLE_RESOLUTION = 25;
namespace TreeSupport3D
{
using LayerIndex = int;

enum class InterfacePreference
{
    InterfaceAreaOverwritesSupport,
    SupportAreaOverwritesInterface,
    InterfaceLinesOverwriteSupport,
    SupportLinesOverwriteInterface,
    Nothing
};

inline bool organic_support_interval_is_printable(
    coordf_t distance, coordf_t min_layer_height, coordf_t max_layer_height)
{
    if (distance < min_layer_height - EPSILON ||
        min_layer_height <= 0. || max_layer_height <= 0.)
        return false;

    const size_t min_steps = std::max<size_t>(
        1, size_t(std::ceil((distance - EPSILON) / max_layer_height)));
    const size_t max_steps =
        size_t(std::floor((distance + EPSILON) / min_layer_height));
    return min_steps <= max_steps;
}

inline bool organic_support_intervals_are_printable(const SlicingParameters &slicing_params)
{
    const coordf_t layer_height = slicing_params.layer_height;
    if (layer_height <= 0.)
        return false;

    const coordf_t top_gap = std::max<coordf_t>(0., slicing_params.gap_support_object);
    const size_t top_gap_layers = size_t(std::ceil(
        std::max<coordf_t>(0., top_gap - EPSILON) / layer_height));
    const coordf_t contact_phase = std::clamp<coordf_t>(
        top_gap_layers * layer_height - top_gap, 0., layer_height);

    // Exact Organic contacts form a phase-shifted grid. All possible distances
    // can be composed from one initial shifted interval and nominal intervals.
    return organic_support_interval_is_printable(
               layer_height,
               slicing_params.min_layer_height,
               slicing_params.max_suport_layer_height) &&
           organic_support_interval_is_printable(
               layer_height + contact_phase,
               slicing_params.min_layer_height,
               slicing_params.max_suport_layer_height);
}

struct TreeSupportMeshGroupSettings {
    TreeSupportMeshGroupSettings() = default;
    explicit TreeSupportMeshGroupSettings(const PrintObject &print_object)
    {
        const PrintConfig       &print_config       = print_object.print()->config();
        const PrintObjectConfig &config             = print_object.config();
        const SlicingParameters &slicing_params     = print_object.slicing_parameters();
    //    const std::vector<unsigned int>  printing_extruders = print_object.object_extruders();

        // Support must be enabled and set to Tree style.
        //assert(config.support_material);
        //assert(config.support_material_style == smsTree || config.support_material_style == smsOrganic);

        // Calculate maximum external perimeter width over all printing regions, taking into account the default layer height.
        coordf_t external_perimeter_width = 0.;
        for (size_t region_id = 0; region_id < print_object.num_printing_regions(); ++ region_id) {
            const PrintRegion &region = print_object.printing_region(region_id);
            external_perimeter_width = std::max<coordf_t>(external_perimeter_width, region.flow(print_object, frExternalPerimeter, config.layer_height).width());
        }

        this->layer_height              = scaled<coord_t>(config.layer_height.value);
        this->resolution                = scaled<coord_t>(print_config.resolution.value);
        // Arache feature
        this->min_feature_size          = scaled<coord_t>(config.min_feature_size.value);
        // +1 makes the threshold inclusive
        this->support_angle             = 0.5 * M_PI - std::clamp<double>((config.support_threshold_angle + 1) * M_PI / 180., 0., 0.5 * M_PI);
        this->support_line_width        = support_material_flow(&print_object, config.layer_height).scaled_width();
        this->support_roof_line_width   = support_material_interface_flow(&print_object, config.layer_height).scaled_width();
        const int bottom_interface_layers = number_of_support_interface_bottom_layers(config);
        this->support_bottom_enable     = bottom_interface_layers > 0;
        this->support_bottom_height     = this->support_bottom_enable ?
            bottom_interface_layers * this->layer_height :
            0;
        this->support_material_buildplate_only = config.support_on_build_plate_only;
        this->support_xy_distance       = scaled<coord_t>(config.support_object_xy_distance.value);
        this->support_xy_distance_1st_layer = scaled<coord_t>(config.support_object_first_layer_gap.value);
        // Separation of interfaces, it is likely smaller than support_xy_distance.
        this->support_xy_distance_overhang = std::min(this->support_xy_distance, scaled<coord_t>(0.5 * external_perimeter_width));
        this->support_top_distance      = scaled<coord_t>(slicing_params.gap_support_object);
        this->support_bottom_distance   = scaled<coord_t>(slicing_params.gap_object_support);
        this->support_roof_enable       = config.support_interface_top_layers.value > 0;
        this->support_roof_layers       = config.support_interface_top_layers.value;
        this->support_floor_enable      = bottom_interface_layers > 0;
        this->support_floor_layers      = bottom_interface_layers;
        this->support_roof_pattern      = config.support_interface_pattern;
        this->support_pattern           = config.support_base_pattern;
        this->support_line_spacing      = scaled<coord_t>(config.support_base_pattern_spacing.value);
        this->support_wall_count        = std::max(1, config.tree_support_wall_count.value);  // at least 1 wall for organic tree support
        this->support_roof_line_distance = scaled<coord_t>(config.support_interface_spacing.value) + this->support_roof_line_width;
        this->support_tree_branch_distance = scaled<coord_t>(config.tree_support_branch_distance_organic.value);
        this->support_tree_angle          = std::clamp<double>(config.tree_support_branch_angle_organic * M_PI / 180., 0., 0.5 * M_PI - EPSILON);
        this->support_tree_angle_slow     = std::clamp<double>(config.tree_support_angle_slow * M_PI / 180., 0., this->support_tree_angle - EPSILON);
        this->support_tree_branch_diameter = scaled<coord_t>(config.tree_support_branch_diameter_organic.value);
        this->support_tree_branch_diameter_angle  = std::clamp<double>(config.tree_support_branch_diameter_angle * M_PI / 180., 0., 0.5 * M_PI - EPSILON);
        this->support_tree_top_rate       = config.tree_support_top_rate.value; // percent
        const bool organic_interface_stack_is_printable =
            config.support_interface_top_layers.value <= 1 ||
            slicing_params.max_suport_layer_height >= slicing_params.layer_height - EPSILON;
        const bool organic_layer_intervals_are_printable =
            organic_support_intervals_are_printable(slicing_params);
        bool organic_object_layers_match_construction_grid = true;
        for (size_t layer_idx = 0; layer_idx < print_object.layer_count(); ++ layer_idx) {
            const coordf_t expected_print_z =
                slicing_params.object_print_z_min +
                slicing_params.first_object_layer_height +
                layer_idx * slicing_params.layer_height;
            if (std::abs(print_object.get_layer(layer_idx)->print_z - expected_print_z) > EPSILON) {
                organic_object_layers_match_construction_grid = false;
                break;
            }
        }
        this->independent_support_layer_height =
            print_config.independent_support_layer_height &&
            ! print_config.enable_prime_tower &&
            slicing_params.raft_layers() == 0 &&
            ! config.precise_z_height &&
            ! config.thick_bridges &&
            organic_interface_stack_is_printable &&
            organic_layer_intervals_are_printable &&
            organic_object_layers_match_construction_grid;
    //    this->support_tree_tip_diameter = this->support_line_width;
        this->support_tree_tip_diameter = std::clamp(scaled<coord_t>(config.tree_support_tip_diameter.value), (coord_t)0, this->support_tree_branch_diameter);
    }

/*********************************************************************/
/* Print parameters, not support specific:                           */
/*********************************************************************/
    coord_t                         layer_height                            { scaled<coord_t>(0.15) };
    // Maximum Deviation (meshfix_maximum_deviation)
    // The maximum deviation allowed when reducing the resolution for the Maximum Resolution setting. If you increase this, 
    // the print will be less accurate, but the g-code will be smaller. Maximum Deviation is a limit for Maximum Resolution, 
    // so if the two conflict the Maximum Deviation will always be held true.
    coord_t                         resolution                              { scaled<coord_t>(0.025) };
    // Minimum Feature Size (aka minimum line width) - Arachne specific
    // Minimum thickness of thin features. Model features that are thinner than this value will not be printed, while features thicker 
    // than the Minimum Feature Size will be widened to the Minimum Wall Line Width.
    coord_t                         min_feature_size                        { scaled<coord_t>(0.1) };

/*********************************************************************/
/* General support parameters:                                       */
/*********************************************************************/

    // Support Overhang Angle
    // The minimum angle of overhangs for which support is added. At a value of 0° all overhangs are supported, 90° will not provide any support.
    double                          support_angle                           { 50. * M_PI / 180. };
    // Support Line Width
    // Width of a single support structure line.
    coord_t                         support_line_width                      { scaled<coord_t>(0.4) };
    // Support Roof Line Width: Width of a single support roof line.
    coord_t                         support_roof_line_width                 { scaled<coord_t>(0.4) };
    // Enable Support Floor (aka bottom interfaces)
    // Generate a dense slab of material between the bottom of the support and the model. This will create a skin between the model and support.
    bool                            support_bottom_enable                   { false };
    // Support Floor Thickness
    // The thickness of the support floors. This controls the number of dense layers that are printed on top of places of a model on which support rests.
    coord_t                         support_bottom_height                   { scaled<coord_t>(1.) };
    bool                            support_material_buildplate_only        { false };
    // Support X/Y Distance
    // Distance of the support structure from the print in the X/Y directions.
    // minimum: 0, maximum warning: 1.5 * machine_nozzle_tip_outer_diameter
    coord_t                         support_xy_distance                     { scaled<coord_t>(0.7) };
    coord_t                         support_xy_distance_1st_layer           { scaled<coord_t>(0.7) };
    // Minimum Support X/Y Distance
    // Distance of the support structure from the overhang in the X/Y directions.
    // minimum_value: 0,  minimum warning": support_xy_distance - support_line_width * 2, maximum warning: support_xy_distance
    coord_t                         support_xy_distance_overhang            { scaled<coord_t>(0.2) };
    // Support Top Distance
    // Distance from the top of the support to the print.
    coord_t                         support_top_distance                    { scaled<coord_t>(0.1) };
    // Support Bottom Distance
    // Distance from the print to the bottom of the support.
    coord_t                         support_bottom_distance                 { scaled<coord_t>(0.1) };
    //FIXME likely not needed, optimization for clipping of interface layers
    // When checking where there's model above and below the support, take steps of the given height. Lower values will slice slower, while higher values 
    // may cause normal support to be printed in some places where there should have been support interface.
    coord_t                         support_interface_skip_height           { scaled<coord_t>(0.3) };
    // Support Infill Line Directions
    // A list of integer line directions to use. Elements from the list are used sequentially as the layers progress and when the end 
    // of the list is reached, it starts at the beginning again. The list items are separated by commas and the whole list is contained 
    // in square brackets. Default is an empty list which means use the default angle 0 degrees.
//    std::vector<double>             support_infill_angles                   {};
    // Enable Support Roof
    // Generate a dense slab of material between the top of support and the model. This will create a skin between the model and support.
    bool                            support_roof_enable                     { false };
    // Support Roof Thickness
    // The thickness of the support roofs. This controls the amount of dense layers at the top of the support on which the model rests.
    coord_t                         support_roof_layers                     { 2 };
    bool                            support_floor_enable                    { false };
    coord_t                         support_floor_layers                    { 2 };
    // Minimum Support Roof Area
    // Minimum area size for the roofs of the support. Polygons which have an area smaller than this value will be printed as normal support.
    double                          minimum_roof_area                       { scaled<double>(scaled<double>(1.)) };
    // A list of integer line directions to use. Elements from the list are used sequentially as the layers progress 
    // and when the end of the list is reached, it starts at the beginning again. The list items are separated
    // by commas and the whole list is contained in square brackets. Default is an empty list which means
    // use the default angles (alternates between 45 and 135 degrees if interfaces are quite thick or 90 degrees).
    std::vector<double>             support_roof_angles                     {};
    // Support Roof Pattern (aka top interface)
    // The pattern with which the roofs of the support are printed.
    SupportMaterialInterfacePattern support_roof_pattern                    { smipAuto };
    // Support Pattern
    // The pattern of the support structures of the print. The different options available result in sturdy or easy to remove support.
    SupportMaterialPattern          support_pattern                         { smpRectilinear };
    // Support Line Distance
    // Distance between the printed support structure lines. This setting is calculated by the support density.
    coord_t                         support_line_spacing                    { scaled<coord_t>(2.66 - 0.4) };
    // Support Floor Horizontal Expansion
    // Amount of offset applied to the floors of the support.
    coord_t                         support_bottom_offset                   { scaled<coord_t>(0.) };
    // Support Wall Line Count
    // The number of walls with which to surround support infill. Adding a wall can make support print more reliably 
    // and can support overhangs better, but increases print time and material used.
    // tree: 1, zig-zag: 0, concentric: 1
    int                             support_wall_count                      { 1 };
    // Support Roof Line Distance
    // Distance between the printed support roof lines. This setting is calculated by the Support Roof Density, but can be adjusted separately.
    coord_t                         support_roof_line_distance              { scaled<coord_t>(0.4) };
    // Minimum Support Area
    // Minimum area size for support polygons. Polygons which have an area smaller than this value will not be generated.
    coord_t                         minimum_support_area                    { scaled<coord_t>(0.) };
    // Minimum Support Floor Area
    // Minimum area size for the floors of the support. Polygons which have an area smaller than this value will be printed as normal support.
    coord_t                         minimum_bottom_area                     { scaled<coord_t>(1.0) };
    // Support Horizontal Expansion
    // Amount of offset applied to all support polygons in each layer. Positive values can smooth out the support areas and result in more sturdy support.
    coord_t                         support_offset                          { scaled<coord_t>(0.) };

/*********************************************************************/
/* Parameters for the Cura tree supports implementation:             */
/*********************************************************************/

    // Tree Support Maximum Branch Angle
    // The maximum angle of the branches, when the branches have to avoid the model. Use a lower angle to make them more vertical and more stable. Use a higher angle to be able to have more reach.
    // minimum: 0, minimum warning: 20, maximum: 89, maximum warning": 85
    double                          support_tree_angle                      { 60. * M_PI / 180. };
    // Tree Support Branch Diameter Angle
    // The angle of the branches' diameter as they gradually become thicker towards the bottom. An angle of 0 will cause the branches to have uniform thickness over their length. 
    // A bit of an angle can increase stability of the tree support.
    // minimum: 0, maximum: 89.9999, maximum warning: 15
    double                          support_tree_branch_diameter_angle      { 5.  * M_PI / 180. };
    // Tree Support Branch Distance
    // How far apart the branches need to be when they touch the model. Making this distance small will cause 
    // the tree support to touch the model at more points, causing better overhang but making support harder to remove.
    coord_t                         support_tree_branch_distance            { scaled<coord_t>(1.) };
    // Tree Support Branch Diameter
    // The diameter of the thinnest branches of tree support. Thicker branches are more sturdy. Branches towards the base will be thicker than this.
    // minimum: 0.001, minimum warning: support_line_width * 2
    coord_t                         support_tree_branch_diameter            { scaled<coord_t>(2.) };

/*********************************************************************/
/* Parameters new to the Thomas Rahm's tree supports implementation: */
/*********************************************************************/

    // Tree Support Preferred Branch Angle
    // The preferred angle of the branches, when they do not have to avoid the model. Use a lower angle to make them more vertical and more stable. Use a higher angle for branches to merge faster.
    // minimum: 0, minimum warning: 10, maximum: support_tree_angle, maximum warning: support_tree_angle-1
    double                          support_tree_angle_slow                 { 50. * M_PI / 180. };
    // Tree Support Diameter Increase To Model
    // The most the diameter of a branch that has to connect to the model may increase by merging with branches that could reach the buildplate.
    // Increasing this reduces print time, but increases the area of support that rests on model
    // minimum: 0
    coord_t                         support_tree_max_diameter_increase_by_merges_when_support_to_model { scaled<coord_t>(1.0) };
    // Tree Support Minimum Height To Model
    // How tall a branch has to be if it is placed on the model. Prevents small blobs of support. This setting is ignored when a branch is supporting a support roof.
    // minimum: 0, maximum warning: 5
    coord_t                         support_tree_min_height_to_model        { scaled<coord_t>(1.0) };
    // Tree Support Inital Layer Diameter
    // Diameter every branch tries to achieve when reaching the buildplate. Improves bed adhesion.
    // minimum: 0, maximum warning: 20
    coord_t                         support_tree_bp_diameter                { scaled<coord_t>(7.5) };
    // Tree Support Branch Density
    // Adjusts the density of the support structure used to generate the tips of the branches. A higher value results in better overhangs,
    // but the supports are harder to remove. Use Support Roof for very high values or ensure support density is similarly high at the top.
    // ->
    // Adjusts the density of the support structure used to generate the tips of the branches.
    // A higher value results in better overhangs but the supports are harder to remove, thus it is recommended to enable top support interfaces
    // instead of a high branch density value if dense interfaces are needed.
    // 5%-35%
    double                          support_tree_top_rate                   { 15. };
    // Tree Support Tip Diameter
    // The diameter of the top of the tip of the branches of tree support.
    // minimum: min_wall_line_width, minimum warning: min_wall_line_width+0.05, maximum_value: support_tree_branch_diameter, value: support_line_width
    coord_t                         support_tree_tip_diameter               { scaled<coord_t>(0.4) };
    // Organic support may use a separate printable Z schedule. Rafts are kept on
    // the legacy synchronized schedule until their contact stack can be planned
    // together with the Organic output layers.
    bool                            independent_support_layer_height        { false };

    // Support Interface Priority
    // How support interface and support will interact when they overlap. Currently only implemented for support roof.
    //enum                           support_interface_priority { support_lines_overwrite_interface_area };
};

/*!
 * \brief This struct contains settings used in the tree support. Thanks to this most functions do not need to know of meshes etc. Also makes the code shorter.
 */
struct TreeSupportSettings
{
public:
    TreeSupportSettings() = default; // required for the definition of the config variable in the TreeSupportGenerator class.
    explicit TreeSupportSettings(const TreeSupportMeshGroupSettings &mesh_group_settings, const SlicingParameters &slicing_params)
        : support_line_width(mesh_group_settings.support_line_width),
          layer_height(mesh_group_settings.layer_height),
          branch_radius(mesh_group_settings.support_tree_branch_diameter / 2),
          min_radius(mesh_group_settings.support_tree_tip_diameter / 2), // The actual radius is 50 microns larger as the resulting branches will be increased by 50 microns to avoid rounding errors effectively increasing the xydistance
          maximum_move_distance((mesh_group_settings.support_tree_angle < M_PI / 2.) ? (coord_t)(tan(mesh_group_settings.support_tree_angle) * layer_height) : std::numeric_limits<coord_t>::max()),
          maximum_move_distance_slow((mesh_group_settings.support_tree_angle_slow < M_PI / 2.) ? (coord_t)(tan(mesh_group_settings.support_tree_angle_slow) * layer_height) : std::numeric_limits<coord_t>::max()),
          support_bottom_layers(mesh_group_settings.support_bottom_enable ? (mesh_group_settings.support_bottom_height + layer_height / 2) / layer_height : 0),
          // Ensure lines always stack nicely even if layer height is large.
          tip_layers(std::max((branch_radius - min_radius) / (support_line_width / 3), branch_radius / layer_height)),
          branch_radius_increase_per_layer(tan(mesh_group_settings.support_tree_branch_diameter_angle) * layer_height),
          max_to_model_radius_increase(mesh_group_settings.support_tree_max_diameter_increase_by_merges_when_support_to_model / 2),
          min_dtt_to_model(round_up_divide(mesh_group_settings.support_tree_min_height_to_model, layer_height)),
          increase_radius_until_radius(mesh_group_settings.support_tree_branch_diameter / 2),
          increase_radius_until_layer(increase_radius_until_radius <= branch_radius ? tip_layers * (increase_radius_until_radius / branch_radius) : (increase_radius_until_radius - branch_radius) / branch_radius_increase_per_layer),
          support_rests_on_model(! mesh_group_settings.support_material_buildplate_only),
          xy_distance(mesh_group_settings.support_xy_distance),
          xy_min_distance(std::min(mesh_group_settings.support_xy_distance, mesh_group_settings.support_xy_distance_overhang)),
          bp_radius(mesh_group_settings.support_tree_bp_diameter / 2),
          // Increase by half a line overlap, but not faster than 40 degrees angle (0 degrees means zero increase in radius).
          bp_radius_increase_per_layer(std::min(tan(0.7) * layer_height, 0.5 * support_line_width)),
          z_distance_bottom_layers(size_t(round(double(mesh_group_settings.support_bottom_distance) / double(layer_height)))),
          z_distance_top_layers(mesh_group_settings.independent_support_layer_height ?
              size_t(std::ceil(std::max(0., double(mesh_group_settings.support_top_distance) - double(SCALED_EPSILON)) / double(layer_height))) :
              size_t(round(double(mesh_group_settings.support_top_distance) / double(layer_height)))),
    //              support_infill_angles(mesh_group_settings.support_infill_angles),
          support_roof_angles(mesh_group_settings.support_roof_angles),
          roof_pattern(mesh_group_settings.support_roof_pattern),
          support_pattern(mesh_group_settings.support_pattern),
          support_roof_line_width(mesh_group_settings.support_roof_line_width),
          support_line_spacing(mesh_group_settings.support_line_spacing),
          support_bottom_offset(mesh_group_settings.support_bottom_offset),
          support_wall_count(mesh_group_settings.support_wall_count),
          resolution(mesh_group_settings.resolution),
          support_roof_line_distance(mesh_group_settings.support_roof_line_distance), // in the end the actual infill has to be calculated to subtract interface from support areas according to interface_preference.
          settings(mesh_group_settings),
          min_feature_size(mesh_group_settings.min_feature_size),
          independent_support_layer_height(mesh_group_settings.independent_support_layer_height)
    {
        // At least one tip layer must be defined.
        assert(tip_layers > 0);
    
        layer_start_bp_radius = (bp_radius - branch_radius) / bp_radius_increase_per_layer;
    
        if (TreeSupportSettings::zero_top_z_gap) {
            // safeOffsetInc can only work in steps of the size xy_min_distance in the worst case => xy_min_distance has to be a bit larger than 0 in this worst case and should be large enough for performance to not suffer extremely
            // When for all meshes the z bottom and top distance is more than one layer though the worst case is xy_min_distance + min_feature_size
            // This is not the best solution, but the only one to ensure areas can not lag though walls at high maximum_move_distance.
            xy_min_distance = std::max(xy_min_distance, scaled<coord_t>(0.1));
            xy_distance     = std::max(xy_distance, xy_min_distance);
        }
    
    //            const std::unordered_map<std::string, InterfacePreference> interface_map = { { "support_area_overwrite_interface_area", InterfacePreference::SupportAreaOverwritesInterface }, { "interface_area_overwrite_support_area", InterfacePreference::InterfaceAreaOverwritesSupport }, { "support_lines_overwrite_interface_area", InterfacePreference::SupportLinesOverwriteInterface }, { "interface_lines_overwrite_support_area", InterfacePreference::InterfaceLinesOverwriteSupport }, { "nothing", InterfacePreference::Nothing } };
    //            interface_preference = interface_map.at(mesh_group_settings.get<std::string>("support_interface_priority"));
    //FIXME this was the default
    //            interface_preference = InterfacePreference::SupportLinesOverwriteInterface;
        //interface_preference = InterfacePreference::SupportAreaOverwritesInterface;
        interface_preference = InterfacePreference::InterfaceAreaOverwritesSupport;
    
        if (slicing_params.raft_layers() > 0) {
            // Fill in raft_layers with the heights of the layers below the first object layer.
            // First layer
            double z = slicing_params.first_print_layer_height;
            this->raft_layers.emplace_back(z);
            // Raft base layers
            for (size_t i = 1; i < slicing_params.base_raft_layers; ++ i) {
                z += slicing_params.base_raft_layer_height;
                this->raft_layers.emplace_back(z);
            }
            // Raft interface layers
            for (size_t i = 0; i + 1 < slicing_params.interface_raft_layers; ++ i) {
                z += slicing_params.interface_raft_layer_height;
                this->raft_layers.emplace_back(z);
            }
            // Raft contact layer
            if (slicing_params.raft_layers() > 1) {
                z = slicing_params.raft_contact_top_z;
                this->raft_layers.emplace_back(z);
            }
            if (double dist_to_go = slicing_params.object_print_z_min - z; dist_to_go > EPSILON) {
                // Layers between the raft contacts and bottom of the object.
                // ORCA: Bias by EPSILON so near-equal gaps do not get an extra split from FP noise.
                auto nsteps = int(ceil((dist_to_go - EPSILON) / slicing_params.max_suport_layer_height));
                double step = dist_to_go / nsteps;
                for (int i = 0; i < nsteps; ++ i) {
                    z += step;
                    this->raft_layers.emplace_back(z);
                }
            }
        }
    }

    // some static variables dependent on other meshes that are not currently processed.
    // Has to be static because TreeSupportConfig will be used in TreeModelVolumes as this reduces redundancy.
    inline static bool zero_top_z_gap = false;
    /*!
     * \brief Width of a single line of support.
     */
    coord_t support_line_width;
    /*!
     * \brief Height of a single layer
     */
    coord_t layer_height;
    /*!
     * \brief Radius of a branch when it has left the tip.
     */
    coord_t branch_radius;
    /*!
     * \brief smallest allowed radius, required to ensure that even at DTT 0 every circle will still be printed
     */
    coord_t min_radius;
    /*!
     * \brief How far an influence area may move outward every layer at most.
     */
    coord_t maximum_move_distance;
    /*!
     * \brief How far every influence area will move outward every layer if possible.
     */
    coord_t maximum_move_distance_slow;
    /*!
     * \brief Amount of bottom layers. 0 if disabled.
     */
    size_t support_bottom_layers;
    /*!
     * \brief Amount of effectiveDTT increases are required to reach branch radius.
     */
    size_t tip_layers;
    /*!
     * \brief How much a branch radius increases with each layer to guarantee the prescribed tree widening.
     */
    double branch_radius_increase_per_layer;
    /*!
     * \brief How much a branch resting on the model may grow in radius by merging with branches that can reach the buildplate.
     */
    coord_t max_to_model_radius_increase;
    /*!
     * \brief If smaller (in layers) than that, all branches to model will be deleted
     */
    size_t min_dtt_to_model;
    /*!
     * \brief Increase radius in the resulting drawn branches, even if the avoidance does not allow it. Will be cut later to still fit.
     */
    coord_t increase_radius_until_radius;
    /*!
     * \brief Same as increase_radius_until_radius, but contains the DTT at which the radius will be reached.
     */
    size_t increase_radius_until_layer;
    /*!
     * \brief True if the branches may connect to the model.
     */
    bool support_rests_on_model;
    /*!
     * \brief How far should support be from the model.
     */
    coord_t xy_distance;
    /*!
     * \brief A minimum radius a tree trunk should expand to at the buildplate if possible.
     */
    coord_t bp_radius;
    /*!
     * \brief The layer index at which an increase in radius may be required to reach the bp_radius.
     */
    LayerIndex layer_start_bp_radius;
    /*!
     * \brief How much one is allowed to increase the tree branch radius close to print bed to reach the required bp_radius at layer 0.
     * Note that this radius increase will not happen in the tip, to ensure the tip is structurally sound.
     */
    double bp_radius_increase_per_layer;
    /*!
     * \brief minimum xy_distance. Only relevant when Z overrides XY, otherwise equal to xy_distance-
     */
    coord_t xy_min_distance;
    /*!
     * \brief Amount of layers distance required the top of the support to the model
     */
    size_t z_distance_top_layers;
    /*!
     * \brief Amount of layers distance required from the top of the model to the bottom of a support structure.
     */
    size_t z_distance_bottom_layers;
    /*!
     * \brief User specified angles for the support infill.
     */
//        std::vector<double> support_infill_angles;
    /*!
     * \brief User specified angles for the support roof infill.
     */
    std::vector<double> support_roof_angles;
    /*!
     * \brief Pattern used in the support roof. May contain non relevant data if support roof is disabled.
     */
    SupportMaterialInterfacePattern roof_pattern;
    /*!
     * \brief Pattern used in the support infill.
     */
    SupportMaterialPattern support_pattern;
    /*!
     * \brief Line width of the support roof.
     */
    coord_t support_roof_line_width;
    /*!
     * \brief Distance between support infill lines.
     */
    coord_t support_line_spacing;
    /*!
     * \brief Offset applied to the support floor area.
     */
    coord_t support_bottom_offset;
    /*
     * \brief Amount of walls the support area will have.
     */
    int support_wall_count;
    /*
     * \brief Maximum allowed deviation when simplifying.
     */
    coord_t resolution;
    /*
     * \brief Distance between the lines of the roof.
     */
    coord_t support_roof_line_distance;
    /*
     * \brief How overlaps of an interface area with a support area should be handled.
     */
    InterfacePreference interface_preference;

    /*
     * \brief The infill class wants a settings object. This one will be the correct one for all settings it uses.
     */
    TreeSupportMeshGroupSettings settings;

    /*
     * \brief Minimum thickness of any model features.
     */
    coord_t min_feature_size;

    // Keep the tree construction grid conservative while allowing its printable
    // slices to be generated on exact contact Z coordinates.
    bool independent_support_layer_height { false };

    // Extra raft layers below the object.
    std::vector<coordf_t> raft_layers;

public:
    bool operator==(const TreeSupportSettings& other) const
    {
        return branch_radius == other.branch_radius && tip_layers == other.tip_layers && branch_radius_increase_per_layer == other.branch_radius_increase_per_layer && layer_start_bp_radius == other.layer_start_bp_radius && bp_radius == other.bp_radius && 
               // as a recalculation of the collision areas is required to set a new min_radius.
               bp_radius_increase_per_layer == other.bp_radius_increase_per_layer && min_radius == other.min_radius && xy_min_distance == other.xy_min_distance &&
               xy_distance - xy_min_distance == other.xy_distance - other.xy_min_distance && // if the delta of xy_min_distance and xy_distance is different the collision areas have to be recalculated.
               support_rests_on_model == other.support_rests_on_model && increase_radius_until_layer == other.increase_radius_until_layer && min_dtt_to_model == other.min_dtt_to_model && max_to_model_radius_increase == other.max_to_model_radius_increase && maximum_move_distance == other.maximum_move_distance && maximum_move_distance_slow == other.maximum_move_distance_slow && z_distance_bottom_layers == other.z_distance_bottom_layers && support_line_width == other.support_line_width && 
               support_line_spacing == other.support_line_spacing && support_roof_line_width == other.support_roof_line_width && // can not be set on a per-mesh basis currently, so code to enable processing different roof line width in the same iteration seems useless.
               support_bottom_offset == other.support_bottom_offset && support_wall_count == other.support_wall_count && support_pattern == other.support_pattern && roof_pattern == other.roof_pattern && // can not be set on a per-mesh basis currently, so code to enable processing different roof patterns in the same iteration seems useless.
               support_roof_angles == other.support_roof_angles && 
               //support_infill_angles == other.support_infill_angles && 
               increase_radius_until_radius == other.increase_radius_until_radius && support_bottom_layers == other.support_bottom_layers && layer_height == other.layer_height && z_distance_top_layers == other.z_distance_top_layers && resolution == other.resolution && // Infill generation depends on deviation and resolution.
               support_roof_line_distance == other.support_roof_line_distance && interface_preference == other.interface_preference
               && min_feature_size == other.min_feature_size // interface_preference should be identical to ensure the tree will correctly interact with the roof.
               && independent_support_layer_height == other.independent_support_layer_height
               // The infill class now wants the settings object and reads a lot of settings, and as the infill class is used to calculate support roof lines for interface-preference. Not all of these may be required to be identical, but as I am not sure, better safe than sorry
#if 0
                && (interface_preference == InterfacePreference::InterfaceAreaOverwritesSupport || interface_preference == InterfacePreference::SupportAreaOverwritesInterface
                // Perimeter generator parameters
                   || 
                        (settings.get<bool>("fill_outline_gaps") == other.settings.get<bool>("fill_outline_gaps") && 
                         settings.get<coord_t>("min_bead_width") == other.settings.get<coord_t>("min_bead_width") && 
                         settings.get<double>("wall_transition_angle") == other.settings.get<double>("wall_transition_angle") && 
                         settings.get<coord_t>("wall_transition_length") == other.settings.get<coord_t>("wall_transition_length") && 
                         settings.get<Ratio>("wall_split_middle_threshold") == other.settings.get<Ratio>("wall_split_middle_threshold") && 
                         settings.get<Ratio>("wall_add_middle_threshold") == other.settings.get<Ratio>("wall_add_middle_threshold") && 
                         settings.get<int>("wall_distribution_count") == other.settings.get<int>("wall_distribution_count") && 
                         settings.get<coord_t>("wall_transition_filter_distance") == other.settings.get<coord_t>("wall_transition_filter_distance") && 
                         settings.get<coord_t>("wall_transition_filter_deviation") == other.settings.get<coord_t>("wall_transition_filter_deviation") && 
                         settings.get<coord_t>("wall_line_width_x") == other.settings.get<coord_t>("wall_line_width_x") && 
                         settings.get<int>("meshfix_maximum_extrusion_area_deviation") == other.settings.get<int>("meshfix_maximum_extrusion_area_deviation"))
                    )
#endif
               && raft_layers == other.raft_layers
            ;
    }

    /*!
     * \brief Get the Radius part will have based on numeric values.
     * \param distance_to_top[in] The effective distance_to_top of the element
     * \param elephant_foot_increases[in] The elephant_foot_increases of the element.
     * \return The radius an element with these attributes would have.
     */
    [[nodiscard]] inline coord_t getRadius(size_t distance_to_top, const double elephant_foot_increases = 0) const
    {
        return (distance_to_top <= tip_layers ? min_radius + (branch_radius - min_radius) * distance_to_top / tip_layers : // tip
                       branch_radius + // base
                       (distance_to_top - tip_layers) * branch_radius_increase_per_layer)
               + // gradual increase
               elephant_foot_increases * (std::max(bp_radius_increase_per_layer - branch_radius_increase_per_layer, 0.0));
    }

    /*!
     * \brief Get the Radius an element should at least have at a given layer.
     * \param layer_idx[in] The layer.
     * \return The radius every element should aim to achieve.
     */
    [[nodiscard]] inline coord_t recommendedMinRadius(LayerIndex layer_idx) const
    {
        double num_layers_widened = layer_start_bp_radius - layer_idx;
        return num_layers_widened > 0 ? branch_radius + num_layers_widened * bp_radius_increase_per_layer : 0;
    }

#if 0
    /*!
     * \brief Return on which z in microns the layer will be printed. Used only for support infill line generation.
     * \param layer_idx[in] The layer.
     * \return The radius every element should aim to achieve.
     */
    [[nodiscard]] inline coord_t getActualZ(LayerIndex layer_idx)
    {
        return layer_idx < coord_t(known_z.size()) ? known_z[layer_idx] : (layer_idx - known_z.size()) * layer_height + known_z.size() ? known_z.back() : 0;
    }

    /*!
     * \brief Set the z every Layer is printed at. Required for getActualZ to work
     * \param z[in] The z every LayerIndex is printed. Vector is used as a map<LayerIndex,coord_t> with the index of each element being the corresponding LayerIndex
     * \return The radius every element should aim to achieve.
     */
    void setActualZ(std::vector<coord_t>& z)
    {
        known_z = z;
    }
#endif

private:
//    std::vector<coord_t> known_z;
};

static constexpr const bool polygons_strictly_simple = false;

inline double tiny_area_threshold() { return sqr(scaled<double>(0.001)); }

inline void tree_supports_show_error(std::string_view message, bool critical)
{ // todo Remove!  ONLY FOR PUBLIC BETA!!
    printf("Error: %s, critical: %d\n", message.data(), int(critical));
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
    static bool g_showed_critical_error = false;
    static bool g_showed_performance_warning = false;
    auto bugtype = std::string(critical ? " This is a critical bug. It may cause missing or malformed branches.\n" : "This bug should only decrease performance.\n");
    bool show    = (critical && !g_showed_critical_error) || (!critical && !g_showed_performance_warning);
    (critical ? g_showed_critical_error : g_showed_performance_warning) = true;
    if (show)
        MessageBoxA(nullptr, std::string("TreeSupport_2 MOD detected an error while generating the tree support.\nPlease report this back to me with profile and model.\nRevision 5.0\n" + std::string(message) + "\n" + bugtype).c_str(), 
            "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
}

inline double layer_z(const SlicingParameters &slicing_params, const TreeSupportSettings &config, const size_t layer_idx)
{
    return layer_idx >= config.raft_layers.size() ? 
        slicing_params.object_print_z_min + slicing_params.first_object_layer_height + (layer_idx - config.raft_layers.size()) * slicing_params.layer_height :
        config.raft_layers[layer_idx];
}

inline double first_object_support_layer_z(const SlicingParameters &slicing_params)
{
    return slicing_params.object_print_z_min + slicing_params.first_object_layer_height;
}

// Orca: Reverse layer_z() for support layers below the first object layer.
// config.raft_layers may include raft/contact/intermediate support Zs, so do not collapse them to the first object support layer.
// Lowest collision layer
inline LayerIndex layer_idx_ceil(const SlicingParameters &slicing_params, const TreeSupportSettings &config, const double z)
{
    const double first_object_z = first_object_support_layer_z(slicing_params);
    if (!config.raft_layers.empty() && z < first_object_z - EPSILON) {
        auto it = std::lower_bound(config.raft_layers.begin(), config.raft_layers.end(), z - EPSILON);
        return LayerIndex(it == config.raft_layers.end() ? config.raft_layers.size() : std::distance(config.raft_layers.begin(), it));
    }
    return LayerIndex(config.raft_layers.size()) +
        std::max<LayerIndex>(0, LayerIndex(std::ceil((z - first_object_z) / slicing_params.layer_height)));
}
// Highest collision layer
inline LayerIndex layer_idx_floor(const SlicingParameters &slicing_params, const TreeSupportSettings &config, const double z)
{
    const double first_object_z = first_object_support_layer_z(slicing_params);
    if (!config.raft_layers.empty() && z < first_object_z - EPSILON) {
        auto it = std::upper_bound(config.raft_layers.begin(), config.raft_layers.end(), z + EPSILON);
        return LayerIndex(it == config.raft_layers.begin() ? 0 : std::distance(config.raft_layers.begin(), it) - 1);
    }
    return LayerIndex(config.raft_layers.size()) +
        std::max<LayerIndex>(0, LayerIndex(std::floor((z - first_object_z) / slicing_params.layer_height)));
}

inline SupportGeneratorLayer& layer_initialize(
    SupportGeneratorLayer     &layer_new,
    const SlicingParameters   &slicing_params,
    const TreeSupportSettings &config, 
    const size_t               layer_idx)
{
    layer_new.print_z  = layer_z(slicing_params, config, layer_idx);
    layer_new.bottom_z = layer_idx > 0 ? layer_z(slicing_params, config, layer_idx - 1) : 0;
    layer_new.height   = layer_new.print_z - layer_new.bottom_z;
    return layer_new;
}

struct OrganicSupportLayerHeight
{
    coordf_t bottom_z { 0. };
    coordf_t print_z  { 0. };
    coordf_t height   { 0. };
};

// Printable Z schedule for Organic supports. Tree construction and collision
// calculations continue to use their nominal integer layer grid. This schedule
// adds exact top-contact Z coordinates and evenly subdivides the intervals
// between them without exceeding the configured maximum support layer height.
class OrganicSupportLayerSchedule
{
public:
    OrganicSupportLayerSchedule(
        std::vector<coordf_t> internal_print_z,
        std::vector<coordf_t> contact_print_z_by_internal_layer,
        coordf_t              min_layer_height,
        coordf_t              max_layer_height,
        size_t                contact_stack_layers,
        bool                  independent) :
        m_internal_print_z(std::move(internal_print_z)),
        m_contact_print_z_by_internal_layer(std::move(contact_print_z_by_internal_layer))
    {
        assert(m_internal_print_z.size() == m_contact_print_z_by_internal_layer.size());
        if (m_internal_print_z.empty())
            return;

        struct RoofBound {
            size_t   anchor_internal_idx;
            size_t   depth;
            coordf_t print_z;
        };
        std::vector<RoofBound> roof_bounds;

        if (! independent) {
            coordf_t bottom_z = 0.;
            for (coordf_t print_z : m_internal_print_z) {
                m_layers.push_back({ bottom_z, print_z, print_z - bottom_z });
                bottom_z = print_z;
            }
        } else {
            std::vector<coordf_t> bounds;
            bounds.reserve(m_contact_print_z_by_internal_layer.size() * (contact_stack_layers + 1) + 1);
            bounds.emplace_back(m_internal_print_z.front());
            for (size_t internal_idx = 0; internal_idx < m_contact_print_z_by_internal_layer.size(); ++ internal_idx) {
                const coordf_t contact_print_z = m_contact_print_z_by_internal_layer[internal_idx];
                if (! std::isfinite(contact_print_z))
                    continue;

                // Preserve the complete interface stack on the construction
                // grid's physical spacing. This keeps roof polygons close to
                // the Z levels where their collisions were evaluated, while
                // base-support intervals may still use larger independent layers.
                const size_t max_depth = std::min(contact_stack_layers, internal_idx > 0 ? internal_idx - 1 : size_t(0));
                for (size_t depth = 0; depth <= max_depth; ++ depth) {
                    const coordf_t print_z = contact_print_z -
                        (m_internal_print_z[internal_idx] - m_internal_print_z[internal_idx - depth]);
                    if (print_z > bounds.front() + EPSILON) {
                        bounds.emplace_back(print_z);
                        roof_bounds.push_back({ internal_idx, depth, print_z });
                    }
                }
            }
            std::sort(bounds.begin(), bounds.end());
            bounds.erase(std::unique(bounds.begin(), bounds.end(),
                [](coordf_t lhs, coordf_t rhs) { return std::abs(lhs - rhs) < EPSILON; }), bounds.end());

            coordf_t bottom_z = 0.;
            m_layers.push_back({ bottom_z, bounds.front(), bounds.front() - bottom_z });
            bottom_z = bounds.front();
            for (size_t bound_idx = 1; bound_idx < bounds.size(); ++ bound_idx) {
                const coordf_t distance = bounds[bound_idx] - bottom_z;
                const size_t steps = std::max<size_t>(
                    1, size_t(std::ceil((distance - EPSILON) / max_layer_height)));
                const size_t max_steps = size_t(std::floor((distance + EPSILON) / min_layer_height));
                if (steps > max_steps)
                    throw SlicingError(
                        "The exact Organic support Z schedule is incompatible with the configured minimum and maximum layer heights.");
                const coordf_t height = distance / coordf_t(steps);
                for (size_t step_idx = 0; step_idx < steps; ++ step_idx) {
                    const coordf_t print_z = step_idx + 1 == steps ?
                        bounds[bound_idx] : bottom_z + height;
                    m_layers.push_back({ bottom_z, print_z, print_z - bottom_z });
                    bottom_z = print_z;
                }
            }
        }

        m_internal_to_output.reserve(m_internal_print_z.size());
        for (size_t internal_idx = 0; internal_idx < m_internal_print_z.size(); ++ internal_idx) {
            const coordf_t scheduled_z =
                internal_idx > 0 && std::isfinite(m_contact_print_z_by_internal_layer[internal_idx]) ?
                    m_contact_print_z_by_internal_layer[internal_idx] :
                    m_internal_print_z[internal_idx];
            m_internal_to_output.emplace_back(this->closest_layer_idx(scheduled_z));
        }

        m_roof_to_output.resize(m_internal_print_z.size());
        for (const RoofBound &bound : roof_bounds) {
            std::vector<size_t> &depths = m_roof_to_output[bound.anchor_internal_idx];
            if (depths.size() <= bound.depth)
                depths.resize(bound.depth + 1, size_t(-1));
            const size_t output_idx = this->closest_layer_idx(bound.print_z);
            depths[bound.depth] = output_idx;
            m_internal_to_output[bound.anchor_internal_idx - bound.depth] = output_idx;
        }

        m_output_to_internal.resize(m_layers.size());
        for (size_t output_idx = 0; output_idx < m_layers.size(); ++ output_idx) {
            auto it = std::lower_bound(
                m_internal_print_z.begin(), m_internal_print_z.end(), m_layers[output_idx].print_z);
            if (it == m_internal_print_z.end()) {
                m_output_to_internal[output_idx] = m_internal_print_z.size() - 1;
            } else {
                m_output_to_internal[output_idx] =
                    size_t(std::distance(m_internal_print_z.begin(), it));
            }
        }
        std::vector<size_t> explicit_output_to_internal(m_layers.size(), size_t(-1));
        for (const RoofBound &bound : roof_bounds) {
            const size_t output_idx = this->closest_layer_idx(bound.print_z);
            const size_t internal_idx = bound.anchor_internal_idx - bound.depth;
            if (explicit_output_to_internal[output_idx] != size_t(-1) &&
                explicit_output_to_internal[output_idx] != internal_idx)
                throw SlicingError(
                    "Organic support contacts require incompatible printable Z mappings.");
            explicit_output_to_internal[output_idx] = internal_idx;
            m_output_to_internal[output_idx] = internal_idx;
        }
        size_t previous_explicit_internal_idx = 0;
        bool have_explicit_internal_idx = false;
        for (size_t internal_idx : explicit_output_to_internal)
            if (internal_idx != size_t(-1)) {
                if (have_explicit_internal_idx && internal_idx < previous_explicit_internal_idx)
                    throw SlicingError(
                        "Organic support contacts require a non-monotonic printable Z mapping.");
                previous_explicit_internal_idx = internal_idx;
                have_explicit_internal_idx = true;
            }

        // A thick printable layer may initially ceil-map above the next exact
        // contact anchor. Clamp non-anchor mappings between their surrounding
        // explicit anchors so the collision-layer mapping never runs backward.
        size_t upper_internal_idx = m_internal_print_z.size() - 1;
        for (size_t output_idx = m_output_to_internal.size(); output_idx-- > 0;) {
            if (explicit_output_to_internal[output_idx] != size_t(-1))
                upper_internal_idx = explicit_output_to_internal[output_idx];
            else
                m_output_to_internal[output_idx] =
                    std::min(m_output_to_internal[output_idx], upper_internal_idx);
        }
        size_t lower_internal_idx = 0;
        for (size_t output_idx = 0; output_idx < m_output_to_internal.size(); ++ output_idx) {
            if (explicit_output_to_internal[output_idx] != size_t(-1))
                lower_internal_idx = explicit_output_to_internal[output_idx];
            else
                m_output_to_internal[output_idx] =
                    std::max(m_output_to_internal[output_idx], lower_internal_idx);
        }

        assert(std::is_sorted(m_internal_to_output.begin(), m_internal_to_output.end()));
        assert(std::is_sorted(m_output_to_internal.begin(), m_output_to_internal.end()));
    }

    bool empty() const { return m_layers.empty(); }
    size_t size() const { return m_layers.size(); }
    const OrganicSupportLayerHeight& operator[](size_t idx) const { return m_layers[idx]; }

    size_t output_layer_for_internal(size_t internal_idx) const
    {
        assert(internal_idx < m_internal_to_output.size());
        return m_internal_to_output[internal_idx];
    }

    size_t output_layer_for_roof(size_t insert_internal_idx, size_t distance_to_top) const
    {
        const size_t anchor_internal_idx = insert_internal_idx + distance_to_top;
        if (anchor_internal_idx < m_roof_to_output.size() &&
            distance_to_top < m_roof_to_output[anchor_internal_idx].size() &&
            m_roof_to_output[anchor_internal_idx][distance_to_top] != size_t(-1))
            return m_roof_to_output[anchor_internal_idx][distance_to_top];
        return this->output_layer_for_internal(std::min(insert_internal_idx, m_internal_to_output.size() - 1));
    }

    size_t internal_layer_for_output(size_t output_idx) const
    {
        assert(output_idx < m_output_to_internal.size());
        return m_output_to_internal[output_idx];
    }

    size_t closest_layer_idx(coordf_t print_z) const
    {
        auto it = std::lower_bound(m_layers.begin(), m_layers.end(), print_z,
            [](const OrganicSupportLayerHeight &layer, coordf_t z) { return layer.print_z < z; });
        if (it == m_layers.begin())
            return 0;
        if (it == m_layers.end())
            return m_layers.size() - 1;
        const size_t upper_idx = size_t(std::distance(m_layers.begin(), it));
        return print_z - m_layers[upper_idx - 1].print_z <= m_layers[upper_idx].print_z - print_z ?
            upper_idx - 1 : upper_idx;
    }

    coordf_t slice_z(size_t idx) const
    {
        return 0.5 * (m_layers[idx].bottom_z + m_layers[idx].print_z);
    }

    size_t first_layer_with_slice_z_at_least(coordf_t z) const
    {
        auto it = std::lower_bound(m_layers.begin(), m_layers.end(), z,
            [](const OrganicSupportLayerHeight &layer, coordf_t value) {
                return 0.5 * (layer.bottom_z + layer.print_z) < value;
            });
        return size_t(std::distance(m_layers.begin(), it));
    }

    size_t first_layer_with_slice_z_above(coordf_t z) const
    {
        auto it = std::upper_bound(m_layers.begin(), m_layers.end(), z,
            [](coordf_t value, const OrganicSupportLayerHeight &layer) {
                return value < 0.5 * (layer.bottom_z + layer.print_z);
            });
        return size_t(std::distance(m_layers.begin(), it));
    }

    SupportGeneratorLayer& initialize(SupportGeneratorLayer &layer, size_t output_idx) const
    {
        const OrganicSupportLayerHeight &height = m_layers[output_idx];
        layer.print_z  = height.print_z;
        layer.bottom_z = height.bottom_z;
        layer.height   = height.height;
        return layer;
    }

private:
    std::vector<OrganicSupportLayerHeight> m_layers;
    std::vector<coordf_t>                  m_internal_print_z;
    std::vector<coordf_t>                  m_contact_print_z_by_internal_layer;
    std::vector<size_t>                    m_internal_to_output;
    std::vector<size_t>                    m_output_to_internal;
    std::vector<std::vector<size_t>>       m_roof_to_output;
};

inline SupportGeneratorLayer& layer_allocate(
    SupportGeneratorLayerStorage       &layer_storage,
    SupporLayerType                     layer_type,
    const OrganicSupportLayerSchedule  &schedule,
    size_t                              output_layer_idx)
{
    SupportGeneratorLayer &layer = layer_storage.allocate(layer_type);
    return schedule.initialize(layer, output_layer_idx);
}

inline SupportGeneratorLayer& layer_allocate_unguarded(
    SupportGeneratorLayerStorage       &layer_storage,
    SupporLayerType                     layer_type,
    const OrganicSupportLayerSchedule  &schedule,
    size_t                              output_layer_idx)
{
    SupportGeneratorLayer &layer = layer_storage.allocate_unguarded(layer_type);
    return schedule.initialize(layer, output_layer_idx);
}

// Using the std::deque as an allocator.
inline SupportGeneratorLayer& layer_allocate_unguarded(
    SupportGeneratorLayerStorage      &layer_storage,
    SupporLayerType                    layer_type,
    const SlicingParameters           &slicing_params,
    const TreeSupportSettings         &config, 
    size_t                             layer_idx)
{
    SupportGeneratorLayer &layer = layer_storage.allocate_unguarded(layer_type);
    return layer_initialize(layer, slicing_params, config, layer_idx);
}

inline SupportGeneratorLayer& layer_allocate(
    SupportGeneratorLayerStorage      &layer_storage,
    SupporLayerType                    layer_type,
    const SlicingParameters           &slicing_params,
    const TreeSupportSettings         &config, 
    size_t                             layer_idx)
{
    SupportGeneratorLayer &layer = layer_storage.allocate(layer_type);
    return layer_initialize(layer, slicing_params, config, layer_idx);
}

// Used by generate_initial_areas() in parallel by multiple layers.
class InterfacePlacer {
public:
    InterfacePlacer(
        const SlicingParameters         &slicing_parameters, 
        const SupportParameters         &support_parameters,
        const TreeSupportSettings       &config, 
        const OrganicSupportLayerSchedule &layer_schedule,
        SupportGeneratorLayerStorage    &layer_storage, 
        SupportGeneratorLayersPtr       &top_contacts,
        SupportGeneratorLayersPtr       &top_interfaces,
        SupportGeneratorLayersPtr       &top_base_interfaces) 
    :
        slicing_parameters(slicing_parameters), support_parameters(support_parameters), config(config), layer_schedule(layer_schedule),
        layer_storage(layer_storage), top_contacts(top_contacts), top_interfaces(top_interfaces), top_base_interfaces(top_base_interfaces)  
    {}
    InterfacePlacer(const InterfacePlacer& rhs) :
        slicing_parameters(rhs.slicing_parameters), support_parameters(rhs.support_parameters), config(rhs.config), layer_schedule(rhs.layer_schedule),
        layer_storage(rhs.layer_storage), top_contacts(rhs.top_contacts), top_interfaces(rhs.top_interfaces), top_base_interfaces(rhs.top_base_interfaces) 
    {}

    const SlicingParameters    &slicing_parameters;
    const SupportParameters    &support_parameters;
    const TreeSupportSettings  &config;
    const OrganicSupportLayerSchedule &layer_schedule;
    SupportGeneratorLayersPtr&  top_contacts_mutable() { return this->top_contacts; }

public:
    // Insert the contact layer and some of the interface and base interface layers below.
    void add_roofs(std::vector<Polygons> &&new_roofs, const size_t insert_layer_idx)
    {
        if (! new_roofs.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex_layer_storage);
            for (size_t idx = 0; idx < new_roofs.size(); ++ idx)
                if (! new_roofs[idx].empty())
                    add_roof_unguarded(std::move(new_roofs[idx]), insert_layer_idx - idx, idx);
        }
    }

    void add_roof(Polygons &&new_roof, const size_t insert_layer_idx, const size_t dtt_tip)
    {
        std::lock_guard<std::mutex> lock(m_mutex_layer_storage);
        add_roof_unguarded(std::move(new_roof), insert_layer_idx, dtt_tip);
    }

    // called by sample_overhang_area()
    void add_roof_build_plate(Polygons &&overhang_areas, size_t dtt_roof)
    {
        std::lock_guard<std::mutex> lock(m_mutex_layer_storage);
        this->add_roof_to_output_unguarded(
            std::move(overhang_areas),
            layer_schedule.output_layer_for_internal(0),
            std::min(dtt_roof, this->support_parameters.num_top_interface_layers));
    }

    void add_roof_unguarded(Polygons &&new_roofs, const size_t insert_layer_idx, const size_t dtt_roof)
    {
        this->add_roof_to_output_unguarded(
            std::move(new_roofs),
            layer_schedule.output_layer_for_roof(insert_layer_idx, dtt_roof),
            dtt_roof);
    }

    void add_roof_to_output_unguarded(Polygons &&new_roofs, const size_t output_layer_idx, const size_t dtt_roof)
    {
        assert(support_parameters.has_top_contacts);
        assert(dtt_roof <= support_parameters.num_top_interface_layers);
        // ORCA: Reserve one top interface layer but only when top base-interface layers exist.
        // This prevents all interface layers from being classified as base-interface layers
        // and preserves correct top contact and interface behavior.
        size_t interface_threshold = support_parameters.num_top_interface_layers_only();
        if (interface_threshold > 0 && support_parameters.num_top_base_interface_layers > 0)
            --interface_threshold;
        SupportGeneratorLayersPtr &layers =
            dtt_roof == 0 ? this->top_contacts :
            dtt_roof <= interface_threshold ? this->top_interfaces : this->top_base_interfaces;
        SupportGeneratorLayer*& l = layers[output_layer_idx];
        if (l == nullptr)
            l = &layer_allocate_unguarded(layer_storage, dtt_roof == 0 ? SupporLayerType::TopContact : SupporLayerType::TopInterface, 
                    layer_schedule, output_layer_idx);
        // will be unioned in finalize_interface_and_support_areas()
        append(l->polygons, std::move(new_roofs));
    }

private:
    // Outputs
    SupportGeneratorLayerStorage                       &layer_storage;
    SupportGeneratorLayersPtr                          &top_contacts;
    SupportGeneratorLayersPtr                          &top_interfaces;
    SupportGeneratorLayersPtr                          &top_base_interfaces;

    // Mutexes, guards
    std::mutex                                          m_mutex_layer_storage;
};

enum class LineStatus
{
    INVALID,
    TO_MODEL,
    TO_MODEL_GRACIOUS,
    TO_MODEL_GRACIOUS_SAFE,
    TO_BP,
    TO_BP_SAFE
};


} // namespace TreeSupport3D

} // namespace Slic3r

#endif // slic3r_TreeSupportCommon_hpp
