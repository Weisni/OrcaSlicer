#pragma once

namespace Slic3r::CloudCredentialPolicy {
// The store service is shared across processes, unlike the profile directory.
constexpr const char *service_name(bool local_test)
{
    return local_test ? "QuackSlicerBetaTest/Auth" : "QuackSlicer/Auth";
}
} // namespace Slic3r::CloudCredentialPolicy
