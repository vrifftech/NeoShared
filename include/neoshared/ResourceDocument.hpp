#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <system_error>
#include <stdexcept>
namespace neoshared {
// Owned snapshot. No pointer back into a tree, retained file session, or archive.
struct ResourceDocument {
    std::string identity;
    std::string fileName;
    std::string sourceDescription;
    std::uint16_t type{};
    std::vector<std::uint8_t> bytes;
    // A detached edit must never be saved over these inputs (including aliases).
    std::vector<std::filesystem::path> protectedInputs;
};
inline bool sameResourcePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec) return true;
    const auto left = std::filesystem::weakly_canonical(std::filesystem::absolute(a), ec);
    if (ec) throw std::runtime_error("Cannot inspect path: " + a.string());
    const auto right = std::filesystem::weakly_canonical(std::filesystem::absolute(b), ec);
    if (ec) throw std::runtime_error("Cannot inspect path: " + b.string());
    return left == right;
}
inline void checkResourceOutput(const std::filesystem::path& output,
                                const std::vector<std::filesystem::path>& protectedInputs) {
    if (output.empty()) throw std::runtime_error("Choose a separate output filename.");
    for (const auto& input : protectedInputs) if (sameResourcePath(output, input))
        throw std::runtime_error("That destination is a source resource, archive, KEY, or an alias. Choose a separate working file.");
}
}
