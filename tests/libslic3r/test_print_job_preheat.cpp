#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "slic3r/GUI/Jobs/PrintJobPreheat.hpp"

using Slic3r::GUI::PrintJobPreheat::max_temperature;

TEST_CASE("Chamber preheat ignores hotter filaments on other plates", "[PrintJobPreheat][Regression]")
{
    CHECK(max_temperature({0, 55, 60, 60, 60, 60}, {2}) == 55);
    CHECK(max_temperature({0, 56, 60, 60, 60, 60}, {2}) == 56);
}

TEST_CASE("A plate without a chamber target does not inherit another plate's target", "[PrintJobPreheat]")
{
    CHECK(max_temperature({0, 55, 60}, {1}) == 0);
}

TEST_CASE("Multiple printing filaments use their highest temperature", "[PrintJobPreheat]")
{
    CHECK(max_temperature({45, 55, 80}, {1, 2}) == 55);
    CHECK(max_temperature({45, 55, 80}, {1, 3}) == 80);
}

TEST_CASE("Preheat filament identities are one based", "[PrintJobPreheat]")
{
    CHECK(max_temperature({45, 55, 60}, {1}) == 45);
    CHECK(max_temperature({45, 55, 60}, {2}) == 55);
    CHECK(max_temperature({45, 55, 60}, {3}) == 60);
}

TEST_CASE("Unknown plate materials do not heat from unrelated settings", "[PrintJobPreheat]")
{
    CHECK(max_temperature({55, 60}, {}) == 0);
    CHECK(max_temperature({}, {1}) == 0);
    CHECK(max_temperature({55, 60}, {0, -1, 3}) == 0);
    CHECK(max_temperature({55, 60}, {0, -1, 2, 3}) == 60);
}

TEST_CASE("Filament order and duplicate identities do not change preheat", "[PrintJobPreheat]")
{
    CHECK(max_temperature({45, 55, 80}, {2, 1, 2}) == 55);
}

TEST_CASE("Bed restoration ignores unused high temperature filaments", "[PrintJobPreheat]")
{
    CHECK(max_temperature({45, 110, 120}, {1}) == 45);
    CHECK(max_temperature({45, 110, 120}, {2}) == 110);
}

TEST_CASE("Nonpositive filament targets leave preheat disabled", "[PrintJobPreheat]")
{
    CHECK(max_temperature({-1, 0, 60}, {1, 2}) == 0);
}


using Slic3r::GUI::PrintJobPreheat::PlateauDetector;

TEST_CASE("A quiet hot bed chamber window permits continuing below target", "[PrintJobPreheat]")
{
    PlateauDetector detector;
    for (int t = 0; t < 600; t += 5)
        REQUIRE_FALSE(detector.update(t, 52, true));
    CHECK(detector.update(600, 52, true));
}

TEST_CASE("One degree sensor noise does not postpone a chamber plateau", "[PrintJobPreheat]")
{
    PlateauDetector detector;
    bool plateau = false;
    for (int t = 0; t <= 600; t += 5)
        plateau = detector.update(t, 52 + (t / 5) % 2, true);
    CHECK(plateau);
}

TEST_CASE("A rising chamber is not a plateau", "[PrintJobPreheat]")
{
    PlateauDetector detector;
    for (int t = 0; t <= 900; t += 5)
        REQUIRE_FALSE(detector.update(t, 45 + t / 300, true));
}

TEST_CASE("A cooling chamber below an earlier peak is not a plateau", "[PrintJobPreheat][Regression]")
{
    PlateauDetector detector;
    for (int t = 0; t <= 900; t += 5)
        REQUIRE_FALSE(detector.update(t, 55 - t / 300, true));
}

TEST_CASE("The plateau window starts only with a hot bed", "[PrintJobPreheat]")
{
    PlateauDetector detector;
    for (int t = 0; t < 1200; t += 5)
        REQUIRE_FALSE(detector.update(t, 30, t >= 600));
    CHECK(detector.update(1200, 30, true));
}

TEST_CASE("Interrupted or stale readings require a new plateau window", "[PrintJobPreheat]")
{
    for (bool stale : {false, true}) {
        PlateauDetector detector;
        for (int t = 0; t <= 595; t += 5)
            detector.update(t, 52, true);
        REQUIRE_FALSE(detector.update(stale ? 600 : 630, 52, !stale));
        REQUIRE_FALSE(detector.update(635, 52, true));
    }
}

TEST_CASE("A temperature dip resets plateau evidence until recovery settles", "[PrintJobPreheat]")
{
    PlateauDetector detector;
    for (int t = 0; t < 600; t += 5)
        detector.update(t, 54, true);
    CHECK_FALSE(detector.update(600, 50, true));
    for (int t = 605; t < 1200; t += 5)
        REQUIRE_FALSE(detector.update(t, 54, true));
    CHECK(detector.update(1205, 54, true));
}

TEST_CASE("Invalid temperatures cannot establish a plateau", "[PrintJobPreheat]")
{
    for (double value : {0., -1., 200., std::numeric_limits<double>::quiet_NaN()}) {
        PlateauDetector detector;
        for (int t = 0; t <= 700; t += 5)
            REQUIRE_FALSE(detector.update(t, value, true));
    }
}


TEST_CASE("Preheat requires a recent temperature push rather than command traffic", "[PrintJobPreheat]")
{
    using Slic3r::GUI::PrintJobPreheat::telemetry_fresh;
    CHECK_FALSE(telemetry_fresh(10000, 0));
    CHECK(telemetry_fresh(10000, 10000));
    CHECK(telemetry_fresh(40000, 10000));
    CHECK_FALSE(telemetry_fresh(40001, 10000));
    CHECK_FALSE(telemetry_fresh(9000, 10000));
    PlateauDetector detector;
    for (int t = 0; t <= 700; t += 5)
        REQUIRE_FALSE(detector.update(t, 52, telemetry_fresh(10000 + t * 1000, 10000)));
}


TEST_CASE("A brief door opening invalidates an almost complete plateau", "[PrintJobPreheat][Regression]")
{
    for (double drop : {1., 2.}) {
        PlateauDetector detector;
        for (int t = 0; t < 600; t += 5)
            REQUIRE_FALSE(detector.update(t, 54, true, 0));
        // The contact opened and closed between polls; the event count persists.
        CHECK_FALSE(detector.update(600, 54-drop, true, 1));
        for (int t = 605; t < 1205; t += 5)
            REQUIRE_FALSE(detector.update(t, 54, true, 1));
        CHECK(detector.update(1205, 54, true, 1));
    }
}

TEST_CASE("A lower plateau after a door opening waits for temperature recovery", "[PrintJobPreheat][Regression]")
{
    for (double drop : {1., 2.}) {
        PlateauDetector detector;
        for (int t = 0; t < 600; t += 5)
            detector.update(t, 54, true, 0);
        for (int t = 600; t < 900; t += 5)
            REQUIRE_FALSE(detector.update(t, 54-drop, false, 1));
        // The door is now closed, but the old warmth has not returned.
        for (int t = 900; t < 1800; t += 5)
            REQUIRE_FALSE(detector.update(t, 54-drop, true, 1));
        for (int t = 1800; t < 2400; t += 5)
            REQUIRE_FALSE(detector.update(t, 54, true, 1));
        CHECK(detector.update(2400, 54, true, 1));
    }
}

TEST_CASE("Repeated door openings restart the recovered plateau window", "[PrintJobPreheat]")
{
    PlateauDetector detector;
    for (int t = 0; t < 600; t += 5)
        detector.update(t, 54, true, 0);
    CHECK_FALSE(detector.update(600, 54, false, 1));
    for (int t = 605; t < 1205; t += 5)
        REQUIRE_FALSE(detector.update(t, 54, true, 1));
    CHECK_FALSE(detector.update(1205, 54, true, 2));
}


TEST_CASE("P2S door status rejects missing or malformed flags", "[PrintJobPreheat]")
{
    using Slic3r::GUI::PrintJobPreheat::p2s_door_open;
    CHECK(p2s_door_open("46258000") == false);
    CHECK(p2s_door_open("46A58000") == true);
    for (auto flags : {"", "46258000junk", "-1", "not-hex", "10000000000000000"})
        CHECK_FALSE(p2s_door_open(flags).has_value());
}

TEST_CASE("P2S plateaus require fresh closed-door status", "[PrintJobPreheat]")
{
    using Slic3r::GUI::PrintJobPreheat::plateau_ready;
    CHECK(plateau_ready(100000, 99999, true, true, 99999));
    CHECK_FALSE(plateau_ready(100000, 99999, true, true, -99999));
    CHECK_FALSE(plateau_ready(100000, 99999, true, true, 0));
    CHECK_FALSE(plateau_ready(100000, 99999, true, true, 60000));
    CHECK_FALSE(plateau_ready(100000, 60000, true, true, 99999));
    CHECK_FALSE(plateau_ready(100000, 99999, false, true, 99999));
    CHECK(plateau_ready(100000, 99999, true, false, 0));
}


TEST_CASE("A door transition during the telemetry snapshot cannot release a plateau", "[PrintJobPreheat][Regression]")
{
    using Slic3r::GUI::PrintJobPreheat::plateau_ready;
    // Count was read before an open/close cycle; status was read afterwards.
    CHECK_FALSE(plateau_ready(100000, 99999, true, true, 99999, false));
    PlateauDetector detector;
    for (int t = 0; t < 600; t += 5)
        detector.update(t, 54, true, 0);
    CHECK_FALSE(detector.update(600, 53, false, 1));
    for (int t = 605; t <= 1205; t += 5)
        REQUIRE_FALSE(detector.update(t, 53, true, 1));
}
