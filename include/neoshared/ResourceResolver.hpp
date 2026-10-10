#pragma once

#include <neoshared/ResourceDocument.hpp>
#include <neoshared/bif/JobControl.hpp>
#include <functional>
#include <optional>

namespace neoshared {
// A resource lookup whose reads participate in the caller's cancellation and
// progress reporting. A captured native resolver owns its catalog snapshot;
// browser-backed resolvers must execute on the JavaScript/UI thread.
using ResourceResolver = std::function<std::optional<ResourceDocument>(
    const std::string&, std::uint16_t, const bif::JobControl&)>;
}
