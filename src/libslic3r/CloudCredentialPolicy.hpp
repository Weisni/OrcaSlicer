#pragma once

namespace Slic3r::CloudCredentialPolicy {
// The store service is shared across processes, unlike the profile directory.
constexpr const char *service_name(bool local_test, const char *test_service = "QuackSlicerBetaTest/Auth")
{
    return local_test ? test_service : "QuackSlicer/Auth";
}
} // namespace Slic3r::CloudCredentialPolicy
