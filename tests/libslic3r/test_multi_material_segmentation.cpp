#include <catch2/catch_all.hpp>

#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/TriangleMesh.hpp"

using Catch::Matchers::WithinAbs;
using namespace Slic3r;

TEST_CASE("Painted facets create closed inward prisms", "[MultiMaterialSegmentation]")
{
    indexed_triangle_set facet;
    facet.vertices = {
        Vec3f(0.f, 0.f, 1.f),
        Vec3f(2.f, 0.f, 1.f),
        Vec3f(0.f, 3.f, 1.f),
    };
    facet.indices.emplace_back(0, 1, 2);

    const float penetration_depth = 0.75f;
    const indexed_triangle_set prism = MMUSegmentationDetail::make_closed_facet_prisms(facet, penetration_depth);

    REQUIRE(prism.vertices.size() == 6);
    REQUIRE(prism.indices.size() == 8);
    REQUIRE(its_num_open_edges(prism) == 0);
    REQUIRE(its_volume(prism) > 0.0);

    for (size_t idx = 0; idx < 3; ++idx) {
        const Vec3f displacement = prism.vertices[idx + 3] - prism.vertices[idx];
        REQUIRE_THAT(double(displacement.norm()), WithinAbs(double(penetration_depth), 1e-6));
        REQUIRE_THAT(double(displacement.z()), WithinAbs(-double(penetration_depth), 1e-6));
    }
}

TEST_CASE("Facet prism depth follows a sloped surface normal", "[MultiMaterialSegmentation]")
{
    indexed_triangle_set facet;
    facet.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(2.f, 0.f, 0.f),
        Vec3f(0.f, 2.f, 2.f),
    };
    facet.indices.emplace_back(0, 1, 2);

    const float penetration_depth = 1.25f;
    const indexed_triangle_set prism = MMUSegmentationDetail::make_closed_facet_prisms(facet, penetration_depth);
    const Vec3f outward_normal = ((facet.vertices[1] - facet.vertices[0]).cross(
                                      facet.vertices[2] - facet.vertices[0])).normalized();

    REQUIRE(prism.vertices.size() == 6);
    REQUIRE(prism.indices.size() == 8);
    REQUIRE(its_num_open_edges(prism) == 0);

    for (size_t idx = 0; idx < 3; ++idx) {
        const Vec3f displacement = prism.vertices[idx + 3] - prism.vertices[idx];
        REQUIRE_THAT(double(displacement.norm()), WithinAbs(double(penetration_depth), 1e-6));
        REQUIRE_THAT(double(displacement.dot(outward_normal)), WithinAbs(-double(penetration_depth), 1e-6));
    }
}

TEST_CASE("Facet prism generation skips invalid input", "[MultiMaterialSegmentation]")
{
    indexed_triangle_set facets;
    facets.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(2.f, 0.f, 0.f),
    };
    facets.indices.emplace_back(0, 1, 2);
    facets.indices.emplace_back(0, 1, 8);

    const indexed_triangle_set prisms = MMUSegmentationDetail::make_closed_facet_prisms(facets, 1.f);

    REQUIRE(prisms.vertices.empty());
    REQUIRE(prisms.indices.empty());
    REQUIRE(MMUSegmentationDetail::make_closed_facet_prisms(facets, 0.f).indices.empty());
}
