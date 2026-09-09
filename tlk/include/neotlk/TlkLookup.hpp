#pragma once

#include "neotlk/TlkFile.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neotlk {

enum class TlkResolutionStatus {
    Resolved, NotLoaded, Missing, TextNotPresent, Skipped, EmbeddedNul, EncodingUnavailable, ContextUnavailable
};
struct TlkResolution {
    TlkResolutionStatus status = TlkResolutionStatus::Missing;
    std::optional<std::string> text;
    std::uint32_t flags = 0;
    std::filesystem::path source;
    std::string problem() const;
};

class TlkLookup {
public:
    void clear();
    void load(const std::filesystem::path& file);
    void load(const std::filesystem::path& file, TextEncoding interpretation);

    bool loaded() const noexcept { return loaded_; }
    const std::filesystem::path& filename() const noexcept { return filename_; }
    std::uint32_t languageId() const noexcept { return languageId_; }
    bool isClassicV30() const noexcept { return classicV30_; }
    std::size_t count() const noexcept { return strings_.empty() ? sparseStrings_.size() : strings_.size(); }

    // Exact index / sparse ID. Raw access is for inspection, not dialogue text.
    std::optional<std::string> resolveRaw(std::uint32_t strref) const;
    // Single-table effective text. No silent StrRef mask or implicit fallback.
    TlkResolution resolveEntry(std::uint32_t strref) const;
    std::optional<std::string> resolve(std::uint32_t strref) const;

private:
    void loadImpl(const std::filesystem::path& file, std::optional<TextEncoding> interpretation);
    bool loaded_ = false;
    bool classicV30_ = false;
    bool nativeEncodingUsable_ = false;
    std::filesystem::path filename_;
    std::uint32_t languageId_ = 0;
    std::vector<std::string> strings_;
    std::vector<std::uint32_t> flags_;
    std::unordered_map<std::uint32_t, std::string> sparseStrings_;
};

// Explicit KotOR context only: caller supplies table order, language and variant.
// Does not discover tables or infer installation/module precedence. Unavailable
// listed candidates are an error, not permission to choose a lower table.
struct KotorTlkContext {
    std::vector<const TlkLookup*> maleTables;
    std::vector<const TlkLookup*> femaleTables;
};
enum class TlkGender { Male, Female };
TlkResolution resolveKotorText(const KotorTlkContext& context, std::uint32_t strref,
                              std::uint32_t languageId = 0,
                              TlkGender gender = TlkGender::Male);

} // namespace neotlk
