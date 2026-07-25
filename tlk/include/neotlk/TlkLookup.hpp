#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neotlk {

class TlkLookup {
public:
    void clear();
    void load(const std::filesystem::path& file);

    bool loaded() const noexcept { return loaded_; }
    const std::filesystem::path& filename() const noexcept { return filename_; }
    std::uint32_t languageId() const noexcept { return languageId_; }
    std::size_t count() const noexcept { return strings_.empty() ? sparseStrings_.size() : strings_.size(); }

    std::optional<std::string> resolve(std::uint32_t strref) const;

private:
    bool loaded_ = false;
    std::filesystem::path filename_;
    std::uint32_t languageId_ = 0;
    std::vector<std::string> strings_;
    std::unordered_map<std::uint32_t, std::string> sparseStrings_;
};

} // namespace neotlk
