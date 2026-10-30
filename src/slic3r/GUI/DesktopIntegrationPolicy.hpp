#pragma once

#include <string_view>

namespace Slic3r::GUI::DesktopIntegrationPolicy {
// Old profiles may contain official Orca paths. Never remove those entries,
// and keep the local test application's registration separate from production.
constexpr bool owns_path(std::string_view path, std::string_view app_key)
{
    if (app_key.empty())
        return false;
    const auto separator = path.find_last_of("/\\");
    auto name = separator == std::string_view::npos ? path : path.substr(separator + 1);
    constexpr std::string_view desktop_namespace = "com.quackslicer.";
    const bool desktop = name.substr(0, desktop_namespace.size()) == desktop_namespace;
    if (desktop)
        name.remove_prefix(desktop_namespace.size());
    if (name.substr(0, app_key.size()) != app_key)
        return false;
    name.remove_prefix(app_key.size());
    if (desktop) {
        constexpr std::string_view viewer = ".GcodeViewer";
        constexpr std::string_view downloader = ".URLProtocol-";
        if (name.substr(0, viewer.size()) == viewer)
            name.remove_prefix(viewer.size());
        else if (name.substr(0, downloader.size()) == downloader)
            return name.size() > downloader.size() + 8 && name.substr(name.size() - 8) == ".desktop";
        return name == ".desktop" || name == "-alpha.desktop" || name == "-beta.desktop";
    }
    constexpr std::string_view viewer = "-gcodeviewer";
    if (name.substr(0, viewer.size()) == viewer)
        name.remove_prefix(viewer.size());
    return name == ".png" || name == "-alpha.png" || name == "-beta.png";
}
} // namespace Slic3r::GUI::DesktopIntegrationPolicy
