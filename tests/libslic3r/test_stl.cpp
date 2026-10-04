#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/STL.hpp"
#include "test_utils.hpp"

#include <fstream>

using namespace Slic3r;

static inline std::string stl_path(const char* path)
{
	return std::string(TEST_DATA_DIR) + "/test_stl/" + path;
}

SCENARIO("Reading an STL file", "[stl]") {
	GIVEN("umlauts in the path of a binary STL file, Czech characters in the file name") {
        WHEN("STL file is read") {
			Slic3r::Model model;
			THEN("load should succeed") {
                REQUIRE(Slic3r::load_stl(stl_path("Geräte/20mmbox-čřšřěá.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
            }
        }
    }
	GIVEN("in ASCII format") {
		WHEN("line endings LF") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-LF.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
		WHEN("line endings CRLF") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-CRLF.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
#if 0
		// ASCII STLs ending with just carriage returns are not supported. These were used by the old Macs, while the Unix based MacOS uses LFs as any other Unix.
		WHEN("line endings CR") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-CR.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}

#endif
		WHEN("nonstandard STL file (text after ending tags, invalid normals, for example infinities)") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-nonstandard.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
	}
}

TEST_CASE("ASCII STL metadata stays within its fixed-width buffers", "[STL][Regression]")
{
    const auto write_single_triangle = [](const ScopedTemporaryFile &file, const std::string &solid_name) {
        std::ofstream out(file.string());
        REQUIRE(out.is_open());
        out << "solid " << solid_name << '\n';
        // ADMesh probes 128 bytes after its binary-header offset before deciding
        // that a file is ASCII. Two valid facets keep even the shortest fixture
        // above that minimum without adding syntax the parser would reject.
        for (int i = 0; i < 2; ++i) {
            out << "facet normal 0 0 1\n"
                << "outer loop\n"
                << "vertex 0 0 0\n"
                << "vertex 1 0 0\n"
                << "vertex 0 1 0\n"
                << "endloop\n"
                << "endfacet\n";
        }
        out << "endsolid " << solid_name << '\n';
    };

    const auto read_metadata = [&](const std::string &solid_name) {
        ScopedTemporaryFile file(".stl");
        write_single_triangle(file, solid_name);

        std::pair<std::string, std::string> captured;
        const auto capture_metadata = [&](int, int, bool &, std::string &model_id, std::string &country) {
            captured = {model_id, country};
        };

        stl_file stl;
        REQUIRE(stl_open(&stl, file.string().c_str(), capture_metadata));
        return captured;
    };

    SECTION("valid metadata is preserved") {
        const auto [model_id, country] = read_metadata("MW 1.0 model-id US");
        CHECK(model_id == "model-id");
        CHECK(country == "US");
    }

    SECTION("metadata beyond the solid-name buffer is ignored") {
        const auto [model_id, country] = read_metadata(std::string(256, 'x') + " MW 1.0 model-id US");
        CHECK(model_id.empty());
        CHECK(country.empty());
    }

    SECTION("a full solid-name buffer ending in MW is ignored") {
        const auto [model_id, country] = read_metadata(std::string(253, 'x') + "MW");
        CHECK(model_id.empty());
        CHECK(country.empty());
    }

    SECTION("oversized metadata fields are rejected without token reassignment") {
        const auto [model_id, country] = read_metadata("MW 1.0 " + std::string(140, 'm') + " " + std::string(20, 'c'));
        CHECK(model_id.empty());
        CHECK(country.empty());
    }
}
