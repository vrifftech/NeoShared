#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <cstddef>
#include <vector>

namespace neogff {

using UInt32 = std::uint32_t;
using FourChar = std::array<char, 4>;
using GffVector3 = std::array<float, 3>;
using GffVector4 = std::array<float, 4>;

bool IsUnsignedDecimal(const std::string& text);
bool IsSignedDecimal(const std::string& text);
bool IsDecimalNumber(const std::string& text);

std::uint32_t ParseUInt32Decimal(const std::string& text);
std::uint64_t ParseUInt64Decimal(const std::string& text);
std::int32_t ParseInt32Decimal(const std::string& text);
std::int64_t ParseInt64Decimal(const std::string& text);
double ParseDoubleDecimal(const std::string& text);
float ParseFloatDecimal(const std::string& text);

// Canonical text boundary for GFF three- and four-component floating-point
// fields. Components are separated by '|'. Empty, missing, or surplus
// components and non-finite values are rejected.
GffVector3 ParseGffVector3Text(const std::string& text);
GffVector4 ParseGffVector4Text(const std::string& text);
std::string FormatGffVector3Text(const GffVector3& value);
std::string FormatGffVector4Text(const GffVector4& value);

std::filesystem::path ResolveOutputTarget(const std::filesystem::path& target);
std::filesystem::path MakeSiblingTempPath(const std::filesystem::path& target, const std::string& tag = "neogff");
void ReplaceFileWithTemp(const std::filesystem::path& tempFile, const std::filesystem::path& targetFile);
void FlushFileToDisk(const std::filesystem::path& file);

class SafeOutputFile {
public:
    explicit SafeOutputFile(const std::filesystem::path& path);
    ~SafeOutputFile();

    SafeOutputFile(const SafeOutputFile&) = delete;
    SafeOutputFile& operator=(const SafeOutputFile&) = delete;
    SafeOutputFile(SafeOutputFile&&) noexcept;
    SafeOutputFile& operator=(SafeOutputFile&&) noexcept;

    void writeBytes(const void* data, std::size_t size);
    template <typename T>
    void writePod(const T& value) {
        writeBytes(&value, sizeof(T));
    }
    void seek(std::uint64_t offset);
    std::uint64_t position() const;
    void flush();
    void close();
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    void closeNoThrow() noexcept;

    std::filesystem::path path_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    std::uint64_t position_ = 0;
    bool closed_ = true;
};
void RemoveFileNoThrow(const std::filesystem::path& file);
std::string ReadRegularFileBytes(const std::filesystem::path& file);

std::string FormatNumber(float value);
std::string FormatNumber(double value);
std::string ToLowerAscii(std::string value);

} // namespace neogff
