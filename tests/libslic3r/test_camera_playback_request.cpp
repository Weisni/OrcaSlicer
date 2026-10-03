#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/CameraPlaybackRequest.hpp"

using Slic3r::GUI::CameraPlaybackRequest;

TEST_CASE("A successful print requests camera playback only once", "[CameraPlaybackRequest]")
{
    CameraPlaybackRequest request;
    const CameraPlaybackRequest::State ready {"printer-a", true, true, true, false};
    CHECK_FALSE(request.consume_if_ready(ready));
    request.request("printer-a");
    CHECK(request.consume_if_ready(ready));
    CHECK_FALSE(request.consume_if_ready(ready));
}

TEST_CASE("Camera playback waits until the target view and camera are ready", "[CameraPlaybackRequest]")
{
    CameraPlaybackRequest request;
    request.request("printer-a");
    CameraPlaybackRequest::State waiting {"printer-a", true, true, true, false};
    SECTION("The device view is still hidden") { waiting.visible = false; }
    SECTION("Printer information has not arrived") { waiting.ready = false; }
    SECTION("The camera is not available yet") { waiting.camera_available = false; }
    SECTION("The camera is busy during print upload") { waiting.busy = true; }
    SECTION("The monitor has no printer yet") { waiting.printer_id.clear(); }
    CHECK_FALSE(request.consume_if_ready(waiting));
    CHECK(request.consume_if_ready({"printer-a", true, true, true, false}));
}

TEST_CASE("Switching to another printer cancels pending camera playback", "[CameraPlaybackRequest]")
{
    CameraPlaybackRequest request;
    request.request("printer-a");
    CHECK_FALSE(request.consume_if_ready({"printer-b", true, true, true, false}));
    CHECK_FALSE(request.consume_if_ready({"printer-a", true, true, true, false}));
}

TEST_CASE("Manual camera controls cancel pending automatic playback", "[CameraPlaybackRequest]")
{
    CameraPlaybackRequest request;
    request.request("printer-a");
    CHECK_FALSE(request.consume_if_ready({"printer-a", false, true, true, false}));
    request.cancel();
    CHECK_FALSE(request.consume_if_ready({"printer-a", true, true, true, false}));
}

TEST_CASE("A new print replaces an older pending camera request", "[CameraPlaybackRequest]")
{
    CameraPlaybackRequest request;
    request.request("printer-a");
    request.request("printer-b");
    CHECK(request.consume_if_ready({"printer-b", true, true, true, false}));
    CHECK_FALSE(request.consume_if_ready({"printer-a", true, true, true, false}));
}

TEST_CASE("An empty printer cannot request automatic camera playback", "[CameraPlaybackRequest]")
{
    CameraPlaybackRequest request;
    request.request("");
    CHECK_FALSE(request.consume_if_ready({"", true, true, true, false}));
    CHECK_FALSE(request.consume_if_ready({"printer-a", true, true, true, false}));
}
