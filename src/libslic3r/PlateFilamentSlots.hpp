#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "IMEXHelpers.hpp"
#include "Config.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

// Relocate logical filament slots without changing physical head identities.
inline void remap_plate_filament_slots(DynamicPrintConfig &config, const std::map<int, int> &moves, size_t new_total)
{
    if (moves.empty()) return;
    const auto destination = [&](int source) {
        const auto moved = moves.find(source);
        return moved == moves.end() ? source : moved->second;
    };
    std::vector<bool> occupied(new_total, false);
    for (const auto &[from, to] : moves)
        if (from < 0 || to < 0 || size_t(from) >= new_total || size_t(to) >= new_total)
            throw std::runtime_error("Plate filament permutation exceeds the material slots");
    for (size_t source = 0; source < new_total; ++source) {
        const auto target = size_t(destination(int(source)));
        if (occupied[target])
            throw std::runtime_error("Plate filament permutation contains duplicate destinations");
        occupied[target] = true;
    }
    auto staged = config;
    for (const auto *key : {"filament_map", "filament_nozzle_map", "filament_volume_map"}) {
        auto *option = staged.option<ConfigOptionInts>(key);
        if (!option || option->values.empty()) continue;
        if (option->values.size() > new_total)
            throw std::runtime_error("Plate filament mapping exceeds the material slots");
        auto original = option->values;
        original.resize(new_total, std::string(key) == "filament_map" ? 1 : 0);
        option->values.resize(new_total);
        for (size_t source = 0; source < new_total; ++source)
            option->values[size_t(destination(int(source)))] = original[source];
    }
    const auto remap_reference = [&](int slot) {
        return slot > 0 ? destination(slot - 1) + 1 : slot;
    };
    for (const auto *key : {"support_filament", "support_interface_filament"})
        if (auto *option = staged.option<ConfigOptionInt>(key))
            option->value = remap_reference(option->value);
    if (auto *option = staged.option<ConfigOptionString>("imex_head_filament_map")) {
        auto heads = parse_imex_head_filament_map(option->value);
        bool changed = false;
        for (auto &[head, slot] : heads) {
            const int mapped = remap_reference(slot);
            changed |= mapped != slot;
            slot = mapped;
        }
        if (changed) {
            option->value.clear();
            for (const auto &[head, slot] : heads) {
                if (!option->value.empty()) option->value += ',';
                option->value += std::to_string(head) + ':' + std::to_string(slot);
            }
        }
    }
    const auto remap_sequence = [&](std::vector<int> &sequence) {
        if (sequence.empty() || (sequence.size() == 1 && sequence.front() == 0)) return;
        for (auto &slot : sequence) slot = remap_reference(slot);
        for (size_t slot = 1; slot <= new_total; ++slot)
            if (std::find(sequence.begin(), sequence.end(), int(slot)) == sequence.end())
                sequence.push_back(int(slot));
    };
    if (auto *option = staged.option<ConfigOptionInts>("first_layer_print_sequence"))
        remap_sequence(option->values);
    if (auto *option = staged.option<ConfigOptionInts>("other_layers_print_sequence"); option && !option->values.empty()) {
        const auto *counts = staged.option<ConfigOptionInt>("other_layers_print_sequence_nums");
        if (!counts || counts->value <= 0 || option->values.size() % size_t(counts->value) != 0 ||
            option->values.size() / size_t(counts->value) <= 2)
            throw std::runtime_error("Plate layer sequence metadata is inconsistent");
        const auto original = option->values;
        const size_t stride = original.size() / size_t(counts->value);
        option->values.clear();
        for (size_t start = 0; start < original.size(); start += stride) {
            // Each block starts with two layer bounds, followed by logical slots.
            option->values.push_back(original[start]);
            option->values.push_back(original[start + 1]);
            std::vector<int> sequence(original.begin() + start + 2, original.begin() + start + stride);
            remap_sequence(sequence);
            option->values.insert(option->values.end(), sequence.begin(), sequence.end());
        }
    }
    config.swap(staged);
}

} // namespace Slic3r
