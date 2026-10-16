#pragma once

#include <algorithm>
#include <cmath>
#include <charconv>
#include <optional>
#include <string_view>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace Slic3r::GUI::PrintJobPreheat {

inline int max_temperature(const std::vector<int>& temperatures, const std::vector<int>& used_filaments)
{
    int maximum = 0;
    // PartPlate uses one-based project filament identities, not AMS tray indices.
    for (int filament : used_filaments) {
        if (filament > 0 && static_cast<std::size_t>(filament) <= temperatures.size())
            maximum = std::max(maximum, temperatures[filament - 1]);
    }
    return maximum;
}

inline bool telemetry_fresh(std::int64_t now_ms, std::int64_t temperature_push_ms)
{
    return temperature_push_ms > 0 && now_ms >= temperature_push_ms && now_ms - temperature_push_ms <= 30000;
}

// P2S reports its contact in print.stat bit 23, matching HA's Bambu integration.
inline std::optional<bool> p2s_door_open(std::string_view stat)
{
    if (stat.empty())
        return std::nullopt;
    std::uint64_t flags = 0;
    const auto result = std::from_chars(stat.data(), stat.data() + stat.size(), flags, 16);
    if (result.ec != std::errc() || result.ptr != stat.data() + stat.size())
        return std::nullopt;
    return (flags & (std::uint64_t(1) << 23)) != 0;
}

inline bool plateau_ready(std::int64_t now_ms, std::int64_t temperature_push_ms,
                          bool bed_ready, bool needs_door, std::int64_t closed_door_push_ms, bool door_snapshot_consistent = true)
{
    return bed_ready && telemetry_fresh(now_ms, temperature_push_ms) &&
           (!needs_door || (door_snapshot_consistent && telemetry_fresh(now_ms, closed_door_push_ms)));
}

// Integer chamber readings need a time window: an old peak also accepts cooling.
// These conservative defaults are a local policy, not a P2S thermal rating.
class PlateauDetector {
    static constexpr int window_seconds = 600;
    std::deque<std::pair<int, double>> m_samples;
    std::uint64_t m_door_open_count = 0;
    double m_last_temperature = 0;
    double m_recovery_temperature = 0;
public:
    bool update(int seconds, double temperature, bool ready, std::uint64_t door_open_count = 0)
    {
        if (door_open_count != m_door_open_count) {
            m_samples.clear();
            m_recovery_temperature = std::max(m_recovery_temperature, m_last_temperature);
            m_door_open_count = door_open_count;
        }
        if (!ready || !std::isfinite(temperature) || temperature <= 0 || temperature >= 150) {
            m_samples.clear();
            return false;
        }
        // A door-induced lower level is not evidence of thermal equilibrium.
        if (temperature < m_recovery_temperature) {
            m_samples.clear();
            return false;
        }
        m_recovery_temperature = 0;
        m_last_temperature = temperature;
        if (!m_samples.empty() && (seconds <= m_samples.back().first || seconds - m_samples.back().first > 15))
            m_samples.clear();
        m_samples.emplace_back(seconds, temperature);
        while (m_samples.size() > 1 && m_samples[1].first <= seconds - window_seconds)
            m_samples.pop_front();
        if (seconds - m_samples.front().first < window_seconds)
            return false;

        double low = temperature, high = temperature;
        double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
        for (const auto& sample : m_samples) {
            const double x = (sample.first - m_samples.front().first) / 60.0;
            low = std::min(low, sample.second);
            high = std::max(high, sample.second);
            sum_x += x;
            sum_y += sample.second;
            sum_xx += x * x;
            sum_xy += x * sample.second;
        }
        const double count = double(m_samples.size());
        const double slope = (count * sum_xy - sum_x * sum_y) / (count * sum_xx - sum_x * sum_x);
        return high - low <= 1.0 && std::abs(slope) <= 0.05;
    }
};

} // namespace Slic3r::GUI::PrintJobPreheat
