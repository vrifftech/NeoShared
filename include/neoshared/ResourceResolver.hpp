#pragma once

#include <neoshared/ResourceDocument.hpp>
#include <neoshared/bif/JobControl.hpp>
#include <functional>
#include <iterator>
#include <optional>
#include <utility>

namespace neoshared {
// A resource lookup whose reads participate in the caller's cancellation and
// progress reporting. A captured native resolver owns its catalog snapshot;
// browser-backed resolvers must execute on the JavaScript/UI thread.
using ResourceResolver = std::function<std::optional<ResourceDocument>(
    const std::string&, std::uint16_t, const bif::JobControl&)>;
inline constexpr std::uint16_t textureTypes[] = {3007,3006,2033,3};
inline bool isTextureType(std::uint16_t type) {
    return std::find(std::begin(textureTypes),std::end(textureTypes),type)!=std::end(textureTypes);
}
template<class Resolve> std::optional<ResourceDocument> resolveTexture(const std::string& name,const Resolve& resolve) {
    std::optional<ResourceDocument> selected;
    for(const auto type:textureTypes) {
        auto resource=resolve(name,type);
        if(resource&&(!selected||resource->priority<selected->priority))selected=std::move(resource);
        if(selected&&selected->priority==ResourcePriority::Working)break;
    }
    return selected;
}
}
