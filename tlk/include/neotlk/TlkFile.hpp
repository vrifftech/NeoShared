// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "neotlk/TextEncoding.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neogff {
class GffFile;
class GffStruct;
}

namespace neotlk {

using UInt32 = std::uint32_t;

constexpr UInt32 TEXT_PRESENT = 0x0001;
constexpr UInt32 SND_PRESENT = 0x0002;
constexpr UInt32 SNDLENGTH_PRESENT = 0x0004;

enum class TlkStorageFormat {
    ClassicV30,
    JadeV40,
    DragonAgeV02,
};

std::string storageFormatName(TlkStorageFormat format);
std::string storageFormatToken(TlkStorageFormat format);
TlkStorageFormat parseStorageFormat(std::string_view value);

class NeoTLKError : public std::runtime_error {
public:
    explicit NeoTLKError(const std::string& message) : std::runtime_error(message) {}
};

struct ResRef {
    std::array<char, 16> bytes{};

    static ResRef fromString(std::string_view value);
    std::string soundString() const;
    std::string toString() const;

    bool operator==(const ResRef& other) const noexcept { return bytes == other.bytes; }
    bool operator!=(const ResRef& other) const noexcept { return !(*this == other); }
};

struct TalkString {
    UInt32 flags = 0;
    ResRef soundResref{};
    UInt32 volumeVariance = 0;
    UInt32 pitchVariance = 0;
    UInt32 offsetToString = 0;
    UInt32 stringSize = 0;
    float soundLength = 0.0f;
    UInt32 soundId = 0xffffffffu; // Jade Empire TLK V4.0 numeric sound id.

    UInt32 strRef = 0;
    std::string text; // UTF-8 in memory for every supported on-disk encoding.
    TextEncoding textEncoding = TextEncoding::Utf8;
    bool custom = false;

    std::string soundString() const;
    void cloneFrom(const TalkString& other);
};

class TalkTable {
public:
    TalkTable();
    explicit TalkTable(const std::string& filename);
    ~TalkTable();

    TalkTable(const TalkTable&) = delete;
    TalkTable& operator=(const TalkTable&) = delete;
    TalkTable(TalkTable&&) noexcept;
    TalkTable& operator=(TalkTable&&) noexcept;

    const std::array<char, 4>& fileId() const noexcept { return fileType_; }
    const std::array<char, 4>& version() const noexcept { return fileVersion_; }
    UInt32 language() const noexcept { return languageId_; }
    void setLanguage(UInt32 languageId);
    void setVersion30();
    void setVersion40();
    bool isVersion40() const noexcept;
    bool isDragonAgeV02() const noexcept { return storageFormat_ == TlkStorageFormat::DragonAgeV02; }
    bool hasSparseStrRefs() const noexcept { return isDragonAgeV02(); }
    bool supportsSoundMetadata() const noexcept { return !isDragonAgeV02(); }
    bool supportsLanguageId() const noexcept { return !isDragonAgeV02(); }
    bool deleteReindexesStrRefs() const noexcept { return !hasSparseStrRefs(); }
    TlkStorageFormat storageFormat() const noexcept { return storageFormat_; }
    TextEncoding preferredTextEncoding() const noexcept { return preferredTextEncoding_; }
    std::string textEncodingSummary() const;
    UInt32 count() const;
    UInt32 minStrRef() const noexcept;
    UInt32 maxStrRef() const noexcept;
    UInt32 nextAvailableStrRef() const;
    bool containsStrRef(UInt32 strRef) const noexcept;
    UInt32 stringEntriesOffset() const noexcept { return stringEntriesOffset_; }
    bool hasOpenFile() const noexcept { return fileOpen_; }
    bool modified() const noexcept { return modified_; }
    const std::string& filename() const noexcept { return filename_; }
    bool hasSaveTarget() const noexcept { return hasSaveTarget_; }
    const std::string& saveTargetFilename() const noexcept { return saveTargetFilename_; }
    const std::vector<TalkString>& entries() const noexcept { return entries_; }
    TalkString& entryAtStrRef(UInt32 strRef);
    const TalkString& entryAtStrRef(UInt32 strRef) const;

    void addEntry(TalkString& entry);
    void addEntry(TalkString&& entry);
    void addEntryAtStrRef(UInt32 strRef, TalkString entry);
    TalkString& ensureEntryAtStrRef(UInt32 strRef);
    void replaceEntry(TalkString& entry);
    void replaceEntry(UInt32 strRef, const TalkString& replacement);
    // Atomically replace the semantic entry set while preserving the native
    // file family and any format-specific backing data (notably GFF4 TLKs).
    void replaceAllEntries(std::vector<TalkString> entries);
    void deleteEntry(UInt32 strRef);
    void newFile();
    void load(const std::string& filename);
    void save(const std::string& filename = std::string());
    void reset();

    UInt32 appendFrom(const TalkTable& other);
    UInt32 padToStrRef(UInt32 targetStrRef);

private:
    TlkStorageFormat storageFormat_ = TlkStorageFormat::ClassicV30;
    TextEncoding preferredTextEncoding_ = TextEncoding::Windows1252;
    std::array<char, 4> fileType_{};
    std::array<char, 4> fileVersion_{};
    UInt32 languageId_ = 0;
    UInt32 stringCount_ = 0;
    UInt32 stringEntriesOffset_ = 0;

    std::vector<TalkString> entries_;
    std::string filename_;
    std::string saveTargetFilename_;

    bool fileOpen_ = false;
    bool modified_ = false;
    bool hasSaveTarget_ = false;
    bool saveTargetSnapshotValid_ = false;
    std::uintmax_t saveTargetSize_ = 0;
    std::filesystem::file_time_type saveTargetWriteTime_{};
    std::uint64_t saveTargetContentHash_ = 0;

    std::unique_ptr<neogff::GffFile> dragonAgeBacking_;
    std::unique_ptr<neogff::GffStruct> dragonAgeEntryPrototype_;
    std::vector<std::unique_ptr<neogff::GffStruct>> dragonAgeReservedStructs_;
    UInt32 dragonAgeListLabelId_ = 0;
    UInt32 dragonAgeIdLabelId_ = 0;
    UInt32 dragonAgeTextLabelId_ = 0;

    void synchronizeCount();
    void loadDragonAgeV02(const std::filesystem::path& filename,
                          const std::string& displayFilename,
                          std::uintmax_t sourceSize,
                          const std::filesystem::file_time_type& sourceWriteTime,
                          std::uint64_t sourceContentHash);
    void prepareDragonAgeV02ForSave();
};

std::string fourCharToString(const std::array<char, 4>& value);

} // namespace neotlk
