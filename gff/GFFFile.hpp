#pragma once

#include "Common.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace neogff {

constexpr UInt32 FIELD_TYPE_BYTE          = 0;
constexpr UInt32 FIELD_TYPE_CHAR          = 1;
constexpr UInt32 FIELD_TYPE_WORD          = 2;
constexpr UInt32 FIELD_TYPE_SHORT         = 3;
constexpr UInt32 FIELD_TYPE_DWORD         = 4;
constexpr UInt32 FIELD_TYPE_INT           = 5;
constexpr UInt32 FIELD_TYPE_DWORD64       = 6;
constexpr UInt32 FIELD_TYPE_INT64         = 7;
constexpr UInt32 FIELD_TYPE_FLOAT         = 8;
constexpr UInt32 FIELD_TYPE_DOUBLE        = 9;
constexpr UInt32 FIELD_TYPE_CEXOSTRING    = 10;
constexpr UInt32 FIELD_TYPE_RESREF        = 11;
constexpr UInt32 FIELD_TYPE_CEXOLOCSTRING = 12;
constexpr UInt32 FIELD_TYPE_VOID          = 13;
constexpr UInt32 FIELD_TYPE_STRUCT        = 14;
constexpr UInt32 FIELD_TYPE_LIST          = 15;
constexpr UInt32 FIELD_TYPE_ORIENTATION   = 16;
constexpr UInt32 FIELD_TYPE_POSITION      = 17;
constexpr UInt32 FIELD_TYPE_JADE_STRREF   = 18;

using Fixed16Chars = std::array<char, 16>;
using Fixed8Bytes = std::array<std::uint8_t, 8>;
using CharBuffer = std::vector<char>;
using ByteBuffer = std::vector<std::uint8_t>;

class GffError : public std::runtime_error {
public:
    explicit GffError(const std::string& message) : std::runtime_error(message) {}
};

class GffField {
public:
    UInt32 fieldtype = 0xFFFFFFFFu;
    bool hasGff4LabelId = false;
    UInt32 gff4LabelId = 0;
    std::uint16_t gff4BaseType = 0;
    std::uint16_t gff4Flags = 0;

    GffField();
    explicit GffField(const std::string& label);
    virtual ~GffField() = default;

    void SetLabel(std::string label);
    void SetLabelRaw(const Fixed16Chars& label);
    std::string GetLabel() const;
    Fixed16Chars GetLabelRaw() const;

    std::string fieldlabel() const { return GetLabel(); }
    void fieldlabel(const std::string& label) { SetLabel(label); }
    Fixed16Chars fieldlabelraw() const { return GetLabelRaw(); }
    void fieldlabelraw(const Fixed16Chars& label) { SetLabelRaw(label); }

    std::string GetString() const;
    std::string text() const { return GetString(); }
    std::unique_ptr<GffField> Clone() const;

protected:
    Fixed16Chars label_{};
};

class GffStruct : public GffField {
public:
    UInt32 typeid_ = 0;
    UInt32 gff4TemplateIndex = 0;

    GffStruct();
    explicit GffStruct(const std::string& label);
    ~GffStruct() override = default;

    UInt32 typeidValue() const { return typeid_; }
    void typeidValue(UInt32 value) { typeid_ = value; }
    std::size_t count() const { return fields_.size(); }

    GffField* GetFieldByLabel(const std::string& label);
    const GffField* GetFieldByLabel(const std::string& label) const;
    GffField* GetFieldByLabel(const std::string& label, std::size_t occurrence);
    const GffField* GetFieldByLabel(const std::string& label, std::size_t occurrence) const;
    GffField* GetField(int index);
    const GffField* GetField(int index) const;
    GffField* GetField(std::size_t index);
    const GffField* GetField(std::size_t index) const;
    GffField* fields(int index) { return GetField(index); }
    const GffField* fields(int index) const { return GetField(index); }
    GffField* fields(std::size_t index) { return GetField(index); }
    const GffField* fields(std::size_t index) const { return GetField(index); }

    void AddField(std::unique_ptr<GffField> field);
    void AddField(GffField* field);
    void DeleteField(const std::string& label);
    void DeleteField(const std::string& label, std::size_t occurrence);

    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;

    std::vector<std::unique_ptr<GffField>>& allFields() { return fields_; }
    const std::vector<std::unique_ptr<GffField>>& allFields() const { return fields_; }

private:
    std::vector<std::unique_ptr<GffField>> fields_;
};

class GffList : public GffField {
public:
    GffList();
    explicit GffList(const std::string& label);
    ~GffList() override = default;

    bool gff4CompactPrimitiveList = false;
    UInt32 gff4PrimitiveListCount = 0;
    UInt32 gff4PrimitiveItemSize = 0;
    ByteBuffer gff4PrimitiveListData;

    std::size_t count() const { return structs_.size(); }
    GffStruct* GetStruct(int index);
    const GffStruct* GetStruct(int index) const;
    GffStruct* GetStruct(std::size_t index);
    const GffStruct* GetStruct(std::size_t index) const;
    GffStruct* structs(int index) { return GetStruct(index); }
    const GffStruct* structs(int index) const { return GetStruct(index); }
    GffStruct* structs(std::size_t index) { return GetStruct(index); }
    const GffStruct* structs(std::size_t index) const { return GetStruct(index); }
    void AddStruct(std::unique_ptr<GffStruct> structure);
    void AddStruct(GffStruct* structure);
    void DeleteStruct(UInt32 index);

    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;

    std::vector<std::unique_ptr<GffStruct>>& allStructs() { return structs_; }
    const std::vector<std::unique_ptr<GffStruct>>& allStructs() const { return structs_; }

private:
    std::vector<std::unique_ptr<GffStruct>> structs_;
};

class GffByteField : public GffField {
public:
    std::uint8_t value = 0;
    GffByteField();
    GffByteField(const std::string& label, std::uint8_t data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffCharField : public GffField {
public:
    char value = '\0';
    GffCharField();
    GffCharField(const std::string& label, char data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffWordField : public GffField {
public:
    std::uint16_t value = 0;
    GffWordField();
    GffWordField(const std::string& label, std::uint16_t data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffShortField : public GffField {
public:
    std::int16_t value = 0;
    GffShortField();
    GffShortField(const std::string& label, std::int16_t data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffUInt32Field : public GffField {
public:
    UInt32 value = 0;
    GffUInt32Field();
    GffUInt32Field(const std::string& label, UInt32 data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffIntField : public GffField {
public:
    std::int32_t value = 0;
    GffIntField();
    GffIntField(const std::string& label, std::int32_t data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffUInt64Field : public GffField {
public:
    std::uint64_t value = 0;
    GffUInt64Field();
    GffUInt64Field(const std::string& label, std::uint64_t data);
    GffUInt64Field(const std::string& label, const Fixed8Bytes& data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffInt64Field : public GffField {
public:
    std::int64_t value = 0;
    GffInt64Field();
    GffInt64Field(const std::string& label, std::int64_t data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffFloatField : public GffField {
public:
    float value = 0.0f;
    GffFloatField();
    GffFloatField(const std::string& label, float data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffDoubleField : public GffField {
public:
    double value = 0.0;
    GffDoubleField();
    GffDoubleField(const std::string& label, double data);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffExoStringField : public GffField {
public:
    UInt32 size = 0;
    CharBuffer textData;

    GffExoStringField();
    GffExoStringField(const std::string& label, const std::string& data);
    void SetString(const std::string& text);
    std::string GetString() const;
    std::string textstring() const { return GetString(); }
    void textstring(const std::string& text) { SetString(text); }
    std::unique_ptr<GffField> Clone() const;
};

class GffResRefField : public GffField {
public:
    std::uint8_t size = 0;
    CharBuffer textData;

    GffResRefField();
    GffResRefField(const std::string& label, const std::string& data);
    void SetString(const std::string& text);
    std::string GetString() const;
    std::string textstring() const { return GetString(); }
    void textstring(const std::string& text) { SetString(text); }
    std::unique_ptr<GffField> Clone() const;
};

class GffLocalizedSubstring {
public:
    std::int32_t stringid = 0;
    std::int32_t stringlength = 0;
    CharBuffer textData;

    GffLocalizedSubstring() = default;
    GffLocalizedSubstring(std::int32_t id, const std::string& text);
    void SetString(const std::string& text);
    std::string GetString() const;
    std::string textstring() const { return GetString(); }
    void textstring(const std::string& text) { SetString(text); }
};

class GffLocalizedStringField : public GffField {
public:
    UInt32 bytesize = 8;
    UInt32 strref = 0xFFFFFFFFu;
    UInt32 stringcount = 0;
    std::vector<GffLocalizedSubstring> substrings;

    GffLocalizedStringField();
    GffLocalizedStringField(const std::string& label, UInt32 strrefValue);

    void AddString(int langId, const std::string& text);
    void SetString(int index, const std::string& text);
    void DeleteString(int index);
    void DeleteStringByID(int langId);
    void SetStringByID(int langId, const std::string& text);
    std::string GetStringById(int langId) const;
    std::string GetString(int index) const;
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;

private:
    void recalcByteSize();
    void recalcByteSizeUsingStringCount();
};

class GffVoidField : public GffField {
public:
    UInt32 bytesize = 0;
    ByteBuffer data;

    GffVoidField();
    GffVoidField(const std::string& label, const ByteBuffer& bytes);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffJadeStringRefField : public GffField {
public:
    UInt32 stringType = 0;
    UInt32 strref = 0xFFFFFFFFu;
    GffJadeStringRefField();
    GffJadeStringRefField(const std::string& label, UInt32 typeValue, UInt32 strrefValue);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffOrientationField : public GffField {
public:
    std::array<float, 4> value{};
    GffOrientationField();
    GffOrientationField(const std::string& label, float d1, float d2, float d3, float d4);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffPositionField : public GffField {
public:
    std::array<float, 3> value{};
    GffPositionField();
    GffPositionField(const std::string& label, float x, float y, float z);
    std::string GetString() const;
    std::unique_ptr<GffField> Clone() const;
};

class GffFile {
public:
    GffFile();
    ~GffFile();

    void NewFile(const std::string& type, const std::filesystem::path& filename = {});
    void LoadFile(const std::filesystem::path& filename);
    void SaveFile(const std::filesystem::path& filename = {});

    GffField* GetFieldByLabel(const std::string& fieldPath);
    const GffField* GetFieldByLabel(const std::string& fieldPath) const;
    GffField* GetFirstRootField();
    GffField* GetNextRootField();

    void AddField(std::unique_ptr<GffField> field, const std::string& path);
    void AddField(GffField* field, const std::string& path);
    void DeleteField(const std::string& fieldPath);
    bool ChangeFieldValue(std::string path, const std::string& value);

    std::string filetype() const;
    void filetype(const std::string& type);
    std::string version() const;
    void version(const std::string& version);
    const std::filesystem::path& filename() const { return filename_; }
    bool loaded() const noexcept { return isLoaded_; }
    bool dirty() const noexcept { return isDirty_; }
    void dirty(bool value) noexcept { isDirty_ = value; }
    bool isGff4() const noexcept { return isGff4_; }
    GffStruct* root() { return rootStruct_.get(); }
    const GffStruct* root() const { return rootStruct_.get(); }

private:
    struct Header {
        FourChar filetype{};
        FourChar fileversion{};
        UInt32 structoffset = 0;
        UInt32 structcount = 0;
        UInt32 fieldoffset = 0;
        UInt32 fieldcount = 0;
        UInt32 labeloffset = 0;
        UInt32 labelcount = 0;
        UInt32 fielddataoffset = 0;
        UInt32 fielddatacount = 0;
        UInt32 fieldindexoffset = 0;
        UInt32 fieldindexcount = 0;
        UInt32 listindexoffset = 0;
        UInt32 listindexcount = 0;
    };

    struct Gff4FieldTemplate {
        UInt32 label = 0;
        std::uint16_t type = 0;
        std::uint16_t flags = 0;
        UInt32 offset = 0;
    };

    struct Gff4StructTemplate {
        UInt32 typeidValue = 0;
        std::vector<Gff4FieldTemplate> fields;
        UInt32 size = 0;
    };

    struct SaveData {
        UInt32 StructCount = 0;
        UInt32 FieldCount = 0;
        UInt32 FieldIndexCount = 0;
        UInt32 ListIndexCount = 0;
        UInt32 ListCount = 0;
        UInt32 DataBlockSize = 0;
        std::vector<Fixed16Chars> FieldLabels;

        UInt32 CurrStructIndex = 0;
        UInt32 CurrStructOffset = 0;
        UInt32 CurrFieldIndex = 0;
        UInt32 CurrFieldOffset = 0;
        UInt32 CurrFieldDataOffset = 0;
        UInt32 CurrFieldIndexOffset = 0;
        UInt32 CurrListIndexOffset = 0;

        void AddLabel(const Fixed16Chars& label);
    };

    void ResetAll();
    void SetType(std::string type);
    void SetVersion(std::string versionText);
    std::string GetType() const;
    std::string GetVersion() const;

    void ValidateCanonicalLayout(std::uint64_t fileSize) const;
    void LoadGff4File(const std::filesystem::path& filename);
    void SaveGff4File(const std::filesystem::path& outFilename);
    std::unique_ptr<GffStruct> LoadGff4Struct(UInt32 templateIndex, UInt32 dataOffset, const std::string& label);
    std::unique_ptr<GffField> LoadGff4Field(const Gff4FieldTemplate& fieldTemplate, UInt32 structDataOffset);
    std::unique_ptr<GffStruct> LoadFileStruct(UInt32 offset);
    std::unique_ptr<GffField> LoadFileField(UInt32 offset);
    std::unique_ptr<GffField> LoadComplexField(UInt32 type, UInt32 dataOrOffset);

    UInt32 SaveGetLabelIndex(const Fixed16Chars& label) const;
    UInt32 SaveProcessList(const GffList& list);
    UInt32 SaveProcessComplexFieldData(const GffField& field);
    UInt32 SaveProcessField(const GffField& field);
    UInt32 SaveProcessFieldSlot(const GffField* field, const std::string& structureLabel);
    UInt32 SaveProcessStruct(const GffStruct& structure);
    UInt32 SaveProcessStructSlot(const GffStruct* structure);
    bool IsComplexField(const GffField& field) const;
    UInt32 GetFieldDataSize(const GffField& field) const;
    void SaveParseList(const GffList& list);
    void SaveParseStruct(const GffStruct& structure);
    void SaveProcessLabels();

    FourChar filetypeRaw_{};
    FourChar fileversionRaw_{};
    std::filesystem::path filename_;
    std::unique_ptr<GffStruct> rootStruct_;
    std::size_t currField_ = 0;
    bool isLoaded_ = false;
    bool isDirty_ = false;
    bool isGff4_ = false;
    FourChar gff4PlatformRaw_{{'P', 'C', ' ', ' '}};
    std::vector<Gff4StructTemplate> gff4Templates_;
    UInt32 gff4DataOffset_ = 0;

    mutable Header header_{};
    mutable SaveData saveData_{};

    class BinaryReader;
    class BinaryWriter;
    std::unique_ptr<BinaryReader> reader_;
    std::unique_ptr<BinaryWriter> writer_;
};

} // namespace neogff
