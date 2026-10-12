#include <catch2/catch_test_macros.hpp>
#include "libslic3r/HaMaterialProfileCache.hpp"
#include <fstream>
#include <chrono>
using namespace Slic3r::HaMaterialProfile;
namespace {
struct CacheDirectory {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("ha-profile-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CacheDirectory() {
#ifdef _WIN32
        root = L"\\\\?\\" + std::filesystem::absolute(root).wstring();
#endif
    }
    ~CacheDirectory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
Json cache_profile() {
    return seal({{"schema_version",1},{"name","Material"},{"material_type","PLA"},
        {"settings",{{"filament_type","PLA"},{"filament_flow_ratio","0.98"}}},
        {"dependencies",{{"inherits",""},{"filament_id","GFA00"},{"vendor",""}}}});
}
}
TEST_CASE("Repeated roll downloads reuse a validated persistent content entry", "[HaMaterialProfileCache]")
{
    CacheDirectory files;
    const auto payload = cache_profile();
    const auto summary = profile_summary(payload);
    int downloads = 0;
    {
        PersistentCache cache(files.root,"schema1");
        const auto fetch = [&] { ++downloads; return payload; };
        CHECK(cache.get("endpoint",summary,fetch) == payload);
        CHECK(cache.get("endpoint",summary,fetch) == payload);
        CHECK(downloads == 1);
    }
    PersistentCache restarted(files.root,"schema1");
    CHECK(restarted.find("endpoint",summary) == payload);
    CHECK(restarted.find("other endpoint",summary).is_null());
    PersistentCache upgraded(files.root,"schema2");
    CHECK(upgraded.find("endpoint",summary).is_null());
}
TEST_CASE("A changed association cannot substitute different downloaded content", "[HaMaterialProfileCache]")
{
    CacheDirectory files;
    PersistentCache cache(files.root,"schema1");
    const auto payload = cache_profile();
    auto changed = payload;
    changed["settings"]["filament_flow_ratio"] = "1.03";
    changed = seal(changed);
    CHECK_THROWS(cache.get("endpoint",profile_summary(payload),[&] { return changed; }));
    CHECK(cache.find("endpoint",profile_summary(payload)).is_null());
}
TEST_CASE("Corrupt persistent content is ignored and downloaded again", "[HaMaterialProfileCache]")
{
    CacheDirectory files;
    const auto payload = cache_profile();
    {
        PersistentCache cache(files.root,"schema1");
        cache.get("endpoint",profile_summary(payload),[&] { return payload; });
    }
    if (std::filesystem::exists(files.root))
        for (const auto& file : std::filesystem::recursive_directory_iterator(files.root))
            if (file.is_regular_file()) { std::ofstream out(file.path()); out << "{}"; }
    PersistentCache cache(files.root,"schema1");
    CHECK(cache.find("endpoint",profile_summary(payload)).is_null());
    CHECK(cache.get("endpoint",profile_summary(payload),[&] { return payload; }) == payload);
    PersistentCache restarted(files.root,"schema1");
    CHECK(restarted.find("endpoint",profile_summary(payload)) == payload);
}

TEST_CASE("Published profile preloading includes every base and nozzle variant once", "[HaMaterialProfileCache]")
{
    const auto payload = cache_profile();
    auto variant = payload;
    variant["settings"]["filament_flow_ratio"] = "1.03";
    variant = seal(variant);
    const Json snapshot = {{"profiles",Json::array({profile_summary(payload),profile_summary(variant)})},
        {"spools",Json::array({{{"uuid","first"},{"material_profile",profile_summary(payload)},
            {"material_profile_variants",{{"nozzle",profile_summary(variant)}}}},
            {{"uuid","second"},{"material_profile",profile_summary(payload)}}})}};
    const auto catalog = published_contents(snapshot);
    REQUIRE(catalog.size() == 2);
    CacheDirectory files;
    PersistentCache cache(files.root,"schema1");
    int downloads = 0;
    for (int iteration = 0; iteration < 2; ++iteration)
        for (const auto& entry : catalog) cache.get("endpoint",entry.summary,[&] {
            ++downloads; return digest(entry.summary) == digest(payload) ? payload : variant;
        });
    CHECK(downloads == 2);
    const auto cached = cache.find("endpoint",profile_summary(payload));
    REQUIRE_FALSE(cached.is_null());
    CHECK(managed_name(cached) == managed_name(payload));
    CHECK_FALSE(catalog[0].spool.empty());
    CHECK_FALSE(catalog[1].spool.empty());
}

TEST_CASE("A settings schema mismatch is rejected before it enters the persistent cache", "[HaMaterialProfileCache]")
{
    CacheDirectory files;
    PersistentCache cache(files.root,"new-schema",{"filament_type","filament_flow_ratio","new_setting"});
    const auto payload = cache_profile();
    CHECK_THROWS(cache.get("endpoint",profile_summary(payload),[&] { return payload; }));
    CHECK(cache.find("endpoint",profile_summary(payload)).is_null());
}

TEST_CASE("Conflicting metadata for one digest cannot enter the published catalog", "[HaMaterialProfileCache]")
{
    const auto summary = profile_summary(cache_profile());
    auto conflicting = summary;
    conflicting["name"] = "Tampered summary";
    CHECK_THROWS(published_contents({{"profiles",Json::array({summary,conflicting})}}));
}

TEST_CASE("RAM eviction retains validated content on disk", "[HaMaterialProfileCache]")
{
    CacheDirectory files;
    PersistentCache cache(files.root,"schema1");
    std::vector<Json> payloads;
    for (int i = 0; i < 65; ++i) {
        auto payload = cache_profile();
        payload["name"] = "Label " + std::to_string(i);
        payloads.push_back(seal(payload));
    }
    std::sort(payloads.begin(),payloads.end(),[](const auto& a,const auto& b) { return digest(a) < digest(b); });
    for (const auto& payload : payloads) cache.get("endpoint",profile_summary(payload),[&] { return payload; });
    const auto evicted = payloads.front();
    PersistentCache restarted(files.root,"schema1");
    CHECK(restarted.find("endpoint",profile_summary(evicted)) == evicted);
    for (const auto& file : std::filesystem::recursive_directory_iterator(files.root))
        if (file.path().filename() == digest(evicted) + ".json") { std::ofstream out(file.path()); out << "{}"; }
    CHECK(cache.find("endpoint",profile_summary(evicted)).is_null());
}

TEST_CASE("An offline roster restores only with every current content cached for its endpoint", "[HaMaterialProfileCache]")
{
    CacheDirectory files;
    const auto payload = cache_profile();
    auto second = payload;
    second["settings"]["filament_flow_ratio"] = "1.03";
    second = seal(second);
    const Json roster = {{"profiles",Json::array({profile_summary(payload),profile_summary(second)})}};
    PersistentCache cache(files.root,"schema1");
    cache.get("endpoint",profile_summary(payload),[&] { return payload; });
    CHECK_FALSE(restorable_contents(cache,"endpoint",roster));
    cache.get("endpoint",profile_summary(second),[&] { return second; });
    PersistentCache restarted(files.root,"schema1");
    CHECK(restorable_contents(restarted,"endpoint",roster));
    CHECK_FALSE(restorable_contents(restarted,"other endpoint",roster));
    PersistentCache upgraded(files.root,"schema2");
    CHECK_FALSE(restorable_contents(upgraded,"endpoint",roster));
}
