#pragma once
#include <neoshared/bif/KeyBifArchive.hpp>
#include <neoshared/bif/LooseArchiveCatalog.hpp>
#include <neoshared/bif/ResourceQuery.hpp>
#include <neoshared/ResourceDocument.hpp>
#include <neoshared/ResourceResolver.hpp>
namespace neoshared::bif {
neoshared::ResourceDocument readResourceDocument(
    const KeyBifArchive& key, const LooseArchiveCatalog& loose,
    const ResourceSelection& selection, const JobControl& job = {});
// Hosting callers pass KotOR's canonical type IDs; Jade sessions translate by
// extension, matching the browser's ordinary companion-resource lookup.
std::optional<neoshared::ResourceDocument> resolveResourceDocument(
    const KeyBifArchive& key, const LooseArchiveCatalog& loose,
    const std::string& resref, std::uint16_t type, const JobControl& job = {});
// Copies indexed metadata without reading or decoding resources. Native
// snapshots retain synchronized archive readers independently of the browser.
neoshared::ResourceResolver captureResourceResolver(
    const KeyBifArchive& key, const LooseArchiveCatalog& loose);
}
