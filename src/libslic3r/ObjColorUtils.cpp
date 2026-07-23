#include "ObjColorUtils.hpp"
#include "Model.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <boost/log/trivial.hpp>

bool obj_color_deal_algo(std::vector<Slic3r::RGBA> & input_colors,
                         std::vector<Slic3r::RGBA> & cluster_colors_from_algo,
                         std::vector<int> &         cluster_labels_from_algo,
                         char &                     cluster_number,
                         int                        max_cluster)
{
    QuantKMeans quant(10);
    quant.apply(input_colors, cluster_colors_from_algo, cluster_labels_from_algo, (int) cluster_number, max_cluster);
    if (cluster_number == -1) {
        return false;
    }
    return true;
}

static Slic3r::RGBA parse_hex_rgba(const std::string& color_str)
{
    std::string hex = color_str;
    if (!hex.empty() && hex.front() == '#')
        hex.erase(hex.begin());

    if (hex.size() != 6 && hex.size() != 8)
        return Slic3r::UNDEFINE_COLOR;

    char* end = nullptr;
    const unsigned long value = std::strtoul(hex.c_str(), &end, 16);
    if (end == nullptr || *end != '\0')
        return Slic3r::UNDEFINE_COLOR;

    const float r = float((value >> (hex.size() == 8 ? 24 : 16)) & 0xff) / 255.f;
    const float g = float((value >> (hex.size() == 8 ? 16 : 8)) & 0xff) / 255.f;
    const float b = float((value >> (hex.size() == 8 ? 8 : 0)) & 0xff) / 255.f;
    const float a = hex.size() == 8 ? float(value & 0xff) / 255.f : 1.f;
    return { r, g, b, a };
}

bool extract_colors_to_obj_dialog(
    Slic3r::Model* model,
    const std::map<int, std::vector<std::string>>& color_group_map,
    const Slic3r::VolumeColorInfoMap& volume_color_data,
    Slic3r::ObjDialogInOut& out)
{
    if (!model || model->objects.empty() || color_group_map.empty() || volume_color_data.empty())
        return false;

    std::unordered_map<std::string, Slic3r::RGBA> color_str_to_rgba;
    for (const auto& group : color_group_map) {
        std::vector<Slic3r::RGBA> group_colors;
        group_colors.reserve(group.second.size());
        for (const std::string& color_str : group.second) {
            Slic3r::RGBA rgba = parse_hex_rgba(color_str);
            color_str_to_rgba[color_str] = rgba;
            group_colors.push_back(rgba);
        }
        out.color_group_map.insert({ group.first, std::move(group_colors) });
    }

    std::set<std::pair<int, int>> used_color_indices;
    for (Slic3r::ModelObject* object : model->objects) {
        if (object == nullptr)
            continue;
        for (Slic3r::ModelVolume* volume : object->volumes) {
            if (volume == nullptr || !volume->is_model_part())
                continue;

            auto vol_color_iter = volume_color_data.find(volume->id().id);
            if (vol_color_iter == volume_color_data.end())
                continue;

            const size_t face_count = volume->mesh().its.indices.size();
            const Slic3r::VolumeColorInfo& vol_color_info = vol_color_iter->second;
            if (vol_color_info.triangle_colors.size() != face_count)
                continue;

            for (const Slic3r::TriangleColor& binding : vol_color_info.triangle_colors) {
                auto group_iter = color_group_map.find(binding.pid);
                if (group_iter == color_group_map.end())
                    continue;
                for (int color_index : binding.indices)
                    if (color_index >= 0 && color_index < int(group_iter->second.size()))
                        used_color_indices.emplace(binding.pid, color_index);
            }
        }
    }

    if (used_color_indices.empty()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": all 3MF colors are undefined";
        return false;
    }

    // Preserve resource and color-index order so the dialog and its mappings are stable.
    for (const auto& group : color_group_map) {
        for (size_t color_index = 0; color_index < group.second.size(); ++color_index) {
            if (used_color_indices.count({group.first, int(color_index)}) == 0)
                continue;
            const Slic3r::RGBA& color = color_str_to_rgba[group.second[color_index]];
            if (Slic3r::color_is_equal(color, Slic3r::UNDEFINE_COLOR))
                continue;
            if (std::find_if(out.input_colors.begin(), out.input_colors.end(), [&color](const Slic3r::RGBA& existing) {
                    return Slic3r::color_is_equal(existing, color);
                }) == out.input_colors.end()) {
                out.input_colors.push_back(color);
            }
        }
    }

    out.is_single_color = out.input_colors.size() == 1;
    return !out.input_colors.empty();
}

std::vector<size_t> compact_new_filament_mappings(
    const size_t existing_filament_count,
    std::vector<int>& filament_mappings)
{
    std::map<int, size_t> source_index_by_filament;
    for (size_t mapping_index = 0; mapping_index < filament_mappings.size(); ++mapping_index) {
        const int filament_id = filament_mappings[mapping_index];
        if (filament_id > 0 && size_t(filament_id) > existing_filament_count)
            source_index_by_filament.emplace(filament_id, mapping_index);
    }

    std::map<int, int> compacted_filament_ids;
    std::vector<size_t> source_indices;
    source_indices.reserve(source_index_by_filament.size());
    int next_filament_id = int(existing_filament_count) + 1;
    for (const auto& [filament_id, source_index] : source_index_by_filament) {
        compacted_filament_ids.emplace(filament_id, next_filament_id++);
        source_indices.emplace_back(source_index);
    }

    for (int& filament_id : filament_mappings) {
        auto compacted_id = compacted_filament_ids.find(filament_id);
        if (compacted_id != compacted_filament_ids.end())
            filament_id = compacted_id->second;
    }

    return source_indices;
}
