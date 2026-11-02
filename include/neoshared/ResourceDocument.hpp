#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <system_error>
#include <stdexcept>
namespace neoshared {
// Source order when choosing between texture formats.
enum class ResourcePriority { Working, Mod, Directory, Image, Archive, Key };
// Owned snapshot. No pointer back into a tree, retained file session, or archive.
struct ResourceDocument {
    std::string identity;
    std::string fileName;
    std::string sourceDescription;
    std::uint16_t type{};
    std::vector<std::uint8_t> bytes;
    // A detached edit must never be saved over these inputs (including aliases).
    std::vector<std::filesystem::path> protectedInputs;
    std::filesystem::path sourcePath{};
    ResourcePriority priority{ResourcePriority::Working};
    std::string module{};
};
inline bool sameResourcePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec) && !ec) return true;
    const auto left = std::filesystem::weakly_canonical(std::filesystem::absolute(a), ec);
    if (ec) throw std::runtime_error("Cannot inspect path: " + a.string());
    const auto right = std::filesystem::weakly_canonical(std::filesystem::absolute(b), ec);
    if (ec) throw std::runtime_error("Cannot inspect path: " + b.string());
    return left == right;
}
inline void checkResourceOutputs(const std::vector<std::filesystem::path>& outputs,
                                 const std::vector<std::filesystem::path>& protectedInputs,
                                 bool compareOutputs = true) {
    if (outputs.empty()) return;
    for (const auto& output : outputs)
        if (output.empty()) throw std::runtime_error("Choose a separate output filename.");
    if (protectedInputs.empty() && (!compareOutputs || outputs.size() == 1)) return;
    auto inputs = protectedInputs;
    std::sort(inputs.begin(), inputs.end());
    inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
    const auto resolve = [](const std::filesystem::path& path) {
        std::error_code ec;
        auto result = std::filesystem::weakly_canonical(std::filesystem::absolute(path), ec);
        if (ec) throw std::runtime_error("Cannot inspect path: " + path.string());
        return result;
    };
    std::vector<std::filesystem::path> sources;
    sources.reserve(inputs.size() + outputs.size());
    for (const auto& input : inputs) {
        if (!input.empty()) sources.push_back(resolve(input));
    }
    for (const auto& output : outputs) {
        const auto destination = resolve(output);
        std::error_code ec;
        for (const auto& source : sources)
            if (source == destination || (std::filesystem::equivalent(destination, source, ec) && !ec))
                throw std::runtime_error("That destination is a source resource, archive, KEY, or an alias. Choose a separate working file.");
        if (compareOutputs) sources.push_back(destination);
    }
}
inline void checkResourceOutput(const std::filesystem::path& output,
                                const std::vector<std::filesystem::path>& protectedInputs) {
    checkResourceOutputs({output}, protectedInputs);
}
}
