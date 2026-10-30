#include "slic3r/GUI/DesktopIntegrationPolicy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <initializer_list>
#include <string_view>

using Slic3r::GUI::DesktopIntegrationPolicy::owns_path;

TEST_CASE("Desktop cleanup owns only the current application registrations", "[DesktopIntegrationPolicy]")
{
    for (std::string_view path : {
             "/home/user/.local/share/applications/com.quackslicer.QuackSlicer.desktop",
             "/home/user/.local/share/applications/com.quackslicer.QuackSlicer-beta.desktop",
             "/home/user/.local/share/applications/com.quackslicer.QuackSlicer.GcodeViewer.desktop",
             "/home/user/.local/share/applications/com.quackslicer.QuackSlicer.URLProtocol-orcaslicer.desktop",
             "/home/user/.local/share/icons/QuackSlicer.png",
             "/home/user/.local/share/icons/QuackSlicer-gcodeviewer-beta.png"}) {
        INFO(path);
        CHECK(owns_path(path, "QuackSlicer"));
    }
}

TEST_CASE("Desktop cleanup preserves official Orca and unrelated registrations", "[DesktopIntegrationPolicy]")
{
    for (std::string_view path : {
             "", "/home/user/.local/share/applications/com.orcaslicer.OrcaSlicer.desktop",
             "/home/user/.local/share/applications/com.orcaslicer.OrcaSlicer-beta.desktop",
             "/home/user/.local/share/applications/OrcaSlicerGcodeViewer.desktop",
             "/home/user/.local/share/applications/OrcaSlicerURLProtocol-orcaslicer.desktop",
             "/home/user/.local/share/icons/OrcaSlicer.png",
             "/home/user/.local/share/icons/QuackSlicerOther.png",
             "/home/user/.local/share/applications/com.quackslicer.QuackSlicer.desktop.backup"}) {
        INFO(path);
        CHECK_FALSE(owns_path(path, "QuackSlicer"));
    }
}

TEST_CASE("Desktop cleanup separates the local test app from production", "[DesktopIntegrationPolicy]")
{
    constexpr auto production = "/home/user/.local/share/applications/com.quackslicer.QuackSlicer.desktop";
    constexpr auto local_test = "/home/user/.local/share/applications/com.quackslicer.QuackSlicerBetaTest.desktop";
    CHECK_FALSE(owns_path(local_test, "QuackSlicer"));
    CHECK_FALSE(owns_path(production, "QuackSlicerBetaTest"));
    CHECK(owns_path(local_test, "QuackSlicerBetaTest"));
    CHECK_FALSE(owns_path(production, ""));
}
