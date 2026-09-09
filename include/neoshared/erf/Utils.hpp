#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <array>
#include <vector>

namespace neoshared::erf {

constexpr std::size_t kDataSafeIoBufferSize = 256u * 1024u;

struct FileIdentity {
    bool valid = false;
    std::uintmax_t size = 0;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_fraction = 0;
    std::int64_t changed_seconds = 0;
    std::int64_t changed_fraction = 0;
#if defined(_WIN32)
    std::uint32_t volume_serial = 0;
    std::uint32_t file_index_high = 0;
    std::uint32_t file_index_low = 0;
#else
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
#endif
};

struct PathIdentity {
    bool valid = false;
    bool is_directory = false;
    bool is_regular = false;
#if defined(_WIN32)
    std::uint32_t volume_serial = 0;
    std::uint32_t file_index_high = 0;
    std::uint32_t file_index_low = 0;
#else
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
#endif
};

std::string ascii_lower(std::string value);
std::string ascii_upper(std::string value);
std::string path_to_string(const std::filesystem::path& path);

bool is_number(const std::string& text);

std::filesystem::path include_trailing_separator(const std::filesystem::path& path);
std::string filename_string(const std::filesystem::path& path);
std::string extension_string(const std::filesystem::path& path);
std::string resource_stem_from_text(const std::string& filename);

bool file_exists(const std::filesystem::path& path);
bool directory_exists(const std::filesystem::path& path);
bool paths_refer_to_same_existing_file(const std::filesystem::path& a, const std::filesystem::path& b);
bool paths_refer_to_same_existing_file_or_location(const std::filesystem::path& a, const std::filesystem::path& b);
void copy_file_overwrite(const std::filesystem::path& source, const std::filesystem::path& destination);
FileIdentity copy_file_overwrite_limited(const std::filesystem::path& source, const std::filesystem::path& destination, std::uintmax_t max_bytes, const FileIdentity* expected_source_identity = nullptr);
FileIdentity copy_file_create_new_limited(const std::filesystem::path& source, const std::filesystem::path& destination, std::uintmax_t max_bytes, const FileIdentity* expected_source_identity = nullptr);
std::uintmax_t regular_file_size_after_open(const std::filesystem::path& path);
FileIdentity capture_regular_file_identity(const std::filesystem::path& path);
bool same_regular_file_identity(const std::filesystem::path& path, const FileIdentity& expected);
// Identity alone is appropriate for owned-temp cleanup. Indexed readers also
// require the size and modification/change revision captured before indexing.
bool same_regular_file_revision(const std::filesystem::path& path, const FileIdentity& expected);
void ensure_same_regular_file_identity(const std::filesystem::path& path, const FileIdentity& expected, const std::string& context);
PathIdentity capture_path_identity(const std::filesystem::path& path);
bool same_path_identity(const std::filesystem::path& path, const PathIdentity& expected);

struct ReplacementTargetState {
    PathIdentity identity{};
};
ReplacementTargetState capture_replacement_target_state(const std::filesystem::path& path);
void ensure_replacement_target_unchanged(const std::filesystem::path& path, const ReplacementTargetState& expected, const std::string& context);
void remove_file_if_same_identity_noexcept(const std::filesystem::path& filename, const FileIdentity& expected) noexcept;
void remove_tree_if_same_identity_noexcept(const std::filesystem::path& folder, const PathIdentity& expected) noexcept;
std::uint64_t write_regular_file_to_stream_limited(std::ostream& out, const std::filesystem::path& source, std::uintmax_t max_bytes, const FileIdentity* expected_source_identity = nullptr);
std::vector<std::filesystem::path> files_in_folder(const std::filesystem::path& folder, bool names_only = true, bool sort_for_determinism = false, bool close_search_handle = true);


std::string string_to_resref(const std::string& text, bool extended);

} // namespace neoshared::erf
