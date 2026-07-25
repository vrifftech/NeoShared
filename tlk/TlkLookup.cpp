#include "neotlk/TlkLookup.hpp"
#include "neotlk/TlkFile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace neotlk {
namespace {

struct Gff4FieldTemplate {
    std::uint32_t label = 0;
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    std::uint32_t offset = 0;
};

struct Gff4StructTemplate {
    std::uint32_t typeidValue = 0;
    std::uint32_t size = 0;
    std::vector<Gff4FieldTemplate> fields;
};

void requireRange(const std::vector<std::uint8_t>& data, std::uint64_t offset, std::uint64_t size, const char* context) {
    if (offset > data.size() || size > data.size() || data.size() - static_cast<std::size_t>(offset) < size) {
        throw std::runtime_error(std::string("Malformed GFF TLK file: ") + context + " points outside the file.");
    }
}

std::uint32_t readU32At(const std::vector<std::uint8_t>& data, std::size_t offset, const char* context) {
    requireRange(data, offset, 4u, context);
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(data[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(data[offset + 3u]) << 24u);
}

std::int32_t readI32At(const std::vector<std::uint8_t>& data, std::size_t offset, const char* context) {
    return static_cast<std::int32_t>(readU32At(data, offset, context));
}

std::uint16_t readU16At(const std::vector<std::uint8_t>& data, std::size_t offset, const char* context) {
    requireRange(data, offset, 2u, context);
    return static_cast<std::uint16_t>(data[offset]) |
           (static_cast<std::uint16_t>(data[offset + 1u]) << 8u);
}

bool bytesEqual(const std::vector<std::uint8_t>& data, std::size_t offset, const char* text, std::size_t size) {
    if (offset > data.size() || data.size() - offset < size) return false;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[offset + i] != static_cast<std::uint8_t>(text[i])) return false;
    }
    return true;
}

void appendUtf8Codepoint(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6u) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12u) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18u) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string decodeDa2TlkString(std::uint32_t bitOffset,
                               const std::vector<std::int32_t>& tree,
                               const std::vector<std::uint32_t>& bitstream) {
    if (bitOffset == 0u) return {};
    if (tree.empty() || (tree.size() % 2u) != 0u || bitstream.empty()) {
        throw std::runtime_error("Malformed TLK V0.5 Huffman tables.");
    }

    const std::uint32_t nodeCount = static_cast<std::uint32_t>(tree.size() / 2u);
    const std::uint32_t rootNode = nodeCount - 1u;
    const std::uint64_t maxBits = static_cast<std::uint64_t>(bitstream.size()) * 32u;
    std::uint64_t bit = bitOffset;
    std::string out;

    for (std::size_t chars = 0; chars < 1048576u; ++chars) {
        std::uint32_t node = rootNode;
        for (std::size_t depth = 0; depth < 1024u; ++depth) {
            if (bit >= maxBits) {
                throw std::runtime_error("Malformed TLK V0.5 string: bit offset exceeded the encoded data.");
            }
            const std::uint32_t word = bitstream[static_cast<std::size_t>(bit / 32u)];
            const std::uint32_t branch = (word >> static_cast<std::uint32_t>(bit % 32u)) & 1u;
            ++bit;
            const std::int32_t child = tree[static_cast<std::size_t>(node) * 2u + branch];
            if (child < 0) {
                const std::uint32_t cp = static_cast<std::uint32_t>(-child - 1);
                if (cp == 0u) return out;
                appendUtf8Codepoint(out, cp);
                break;
            }
            if (static_cast<std::uint32_t>(child) >= nodeCount) {
                throw std::runtime_error("Malformed TLK V0.5 string: Huffman node index is invalid.");
            }
            node = static_cast<std::uint32_t>(child);
        }
    }

    throw std::runtime_error("Malformed TLK V0.5 string: terminator was not found.");
}

std::unordered_map<std::uint32_t, std::string> loadGff4TlkV05(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to open TLK file: " + file.string());
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (data.size() < 28u ||
        !(bytesEqual(data, 0u, "GFF V4.0", 8u) || bytesEqual(data, 0u, "GFF V4.1", 8u)) ||
        !bytesEqual(data, 8u, "PC  ", 4u) ||
        !bytesEqual(data, 12u, "TLK ", 4u)) {
        return {};
    }
    if (!bytesEqual(data, 16u, "V0.5", 4u)) {
        throw std::runtime_error("Unsupported GFF TLK version. the shared GFF core currently resolves Dragon Age II TLK V0.5 files.");
    }
    if (!bytesEqual(data, 0u, "GFF V4.0", 8u)) {
        throw std::runtime_error("Unsupported GFF TLK container version. Expected GFF V4.0.");
    }

    const std::uint32_t structCount = readU32At(data, 20u, "GFF4 TLK struct count");
    const std::uint32_t dataOffset = readU32At(data, 24u, "GFF4 TLK data offset");
    if (structCount == 0u || structCount > 100000u) {
        throw std::runtime_error("Malformed GFF TLK file: unreasonable struct-template count.");
    }
    requireRange(data, 28u, static_cast<std::uint64_t>(structCount) * 16u, "GFF4 TLK struct-template table");
    requireRange(data, dataOffset, 1u, "GFF4 TLK data block");

    std::vector<Gff4StructTemplate> structs;
    structs.reserve(structCount);
    for (std::uint32_t i = 0; i < structCount; ++i) {
        const std::size_t off = 28u + static_cast<std::size_t>(i) * 16u;
        Gff4StructTemplate tmpl;
        tmpl.typeidValue = readU32At(data, off, "GFF4 TLK struct type");
        const std::uint32_t fieldCount = readU32At(data, off + 4u, "GFF4 TLK field count");
        const std::uint32_t fieldOffset = readU32At(data, off + 8u, "GFF4 TLK field offset");
        tmpl.size = readU32At(data, off + 12u, "GFF4 TLK struct size");
        if (fieldCount > 100000u) {
            throw std::runtime_error("Malformed GFF TLK file: unreasonable field-template count.");
        }
        requireRange(data, fieldOffset, static_cast<std::uint64_t>(fieldCount) * 12u, "GFF4 TLK field-template table");
        tmpl.fields.reserve(fieldCount);
        for (std::uint32_t j = 0; j < fieldCount; ++j) {
            const std::size_t fieldBase = static_cast<std::size_t>(fieldOffset) + static_cast<std::size_t>(j) * 12u;
            Gff4FieldTemplate field;
            field.label = readU32At(data, fieldBase, "GFF4 TLK field label");
            field.type = readU16At(data, fieldBase + 4u, "GFF4 TLK field type");
            field.flags = readU16At(data, fieldBase + 6u, "GFF4 TLK field flags");
            field.offset = readU32At(data, fieldBase + 8u, "GFF4 TLK field data offset");
            tmpl.fields.push_back(field);
        }
        structs.push_back(std::move(tmpl));
    }

    if (structs.empty() || structs[0].fields.size() < 3u) {
        throw std::runtime_error("Malformed TLK V0.5 root structure.");
    }

    const Gff4FieldTemplate* stringListField = nullptr;
    const Gff4FieldTemplate* treeField = nullptr;
    const Gff4FieldTemplate* dataField = nullptr;
    for (const Gff4FieldTemplate& field : structs[0].fields) {
        if (field.label == 19006u) stringListField = &field;
        else if (field.label == 19007u) treeField = &field;
        else if (field.label == 19008u) dataField = &field;
    }
    if (!stringListField || !treeField || !dataField) {
        stringListField = &structs[0].fields[0];
        treeField = &structs[0].fields[1];
        dataField = &structs[0].fields[2];
    }

    if ((stringListField->flags & 0xC000u) != 0xC000u || stringListField->type >= structs.size()) {
        throw std::runtime_error("Malformed TLK V0.5 string list field.");
    }
    if ((treeField->flags & 0x8000u) == 0u || treeField->type != 5u ||
        (dataField->flags & 0x8000u) == 0u || dataField->type != 4u) {
        throw std::runtime_error("Malformed TLK V0.5 Huffman fields.");
    }

    auto readRelativeListOffset = [&](const Gff4FieldTemplate& field) -> std::uint32_t {
        const std::uint32_t absolute = dataOffset + field.offset;
        requireRange(data, absolute, 4u, "GFF4 TLK root list reference");
        return readU32At(data, absolute, "GFF4 TLK root list reference");
    };

    const std::uint32_t stringListOffset = dataOffset + readRelativeListOffset(*stringListField);
    const std::uint32_t treeListOffset = dataOffset + readRelativeListOffset(*treeField);
    const std::uint32_t dataListOffset = dataOffset + readRelativeListOffset(*dataField);

    const Gff4StructTemplate& stringItemTemplate = structs[stringListField->type];
    if (stringItemTemplate.size < 8u || stringItemTemplate.fields.size() < 2u) {
        throw std::runtime_error("Malformed TLK V0.5 string-entry template.");
    }
    const Gff4FieldTemplate* stringIdField = nullptr;
    const Gff4FieldTemplate* bitOffsetField = nullptr;
    for (const Gff4FieldTemplate& field : stringItemTemplate.fields) {
        if (field.label == 19004u) stringIdField = &field;
        else if (field.label == 19005u) bitOffsetField = &field;
    }
    if (!stringIdField || !bitOffsetField) {
        stringIdField = &stringItemTemplate.fields[0];
        bitOffsetField = &stringItemTemplate.fields[1];
    }
    if (stringIdField->type != 4u || bitOffsetField->type != 4u) {
        throw std::runtime_error("Malformed TLK V0.5 string-entry fields.");
    }

    requireRange(data, stringListOffset, 4u, "TLK V0.5 string list header");
    const std::uint32_t stringEntryCount = readU32At(data, stringListOffset, "TLK V0.5 string entry count");
    requireRange(data, static_cast<std::uint64_t>(stringListOffset) + 4u,
                 static_cast<std::uint64_t>(stringEntryCount) * stringItemTemplate.size,
                 "TLK V0.5 string entries");

    requireRange(data, treeListOffset, 4u, "TLK V0.5 Huffman tree header");
    const std::uint32_t treeCount = readU32At(data, treeListOffset, "TLK V0.5 Huffman tree value count");
    if (treeCount == 0u || (treeCount % 2u) != 0u) {
        throw std::runtime_error("Malformed TLK V0.5 Huffman tree value count.");
    }
    requireRange(data, static_cast<std::uint64_t>(treeListOffset) + 4u,
                 static_cast<std::uint64_t>(treeCount) * 4u,
                 "TLK V0.5 Huffman tree values");
    std::vector<std::int32_t> tree;
    tree.reserve(treeCount);
    for (std::uint32_t i = 0; i < treeCount; ++i) {
        tree.push_back(readI32At(data, static_cast<std::size_t>(treeListOffset) + 4u + static_cast<std::size_t>(i) * 4u, "TLK V0.5 Huffman tree value"));
    }

    requireRange(data, dataListOffset, 4u, "TLK V0.5 Huffman data header");
    const std::uint32_t dataCount = readU32At(data, dataListOffset, "TLK V0.5 Huffman data word count");
    requireRange(data, static_cast<std::uint64_t>(dataListOffset) + 4u,
                 static_cast<std::uint64_t>(dataCount) * 4u,
                 "TLK V0.5 Huffman data words");
    std::vector<std::uint32_t> bitstream;
    bitstream.reserve(dataCount);
    for (std::uint32_t i = 0; i < dataCount; ++i) {
        bitstream.push_back(readU32At(data, static_cast<std::size_t>(dataListOffset) + 4u + static_cast<std::size_t>(i) * 4u, "TLK V0.5 Huffman data word"));
    }

    std::unordered_map<std::uint32_t, std::string> strings;
    strings.reserve(stringEntryCount);
    for (std::uint32_t i = 0; i < stringEntryCount; ++i) {
        const std::size_t entryOffset = static_cast<std::size_t>(stringListOffset) + 4u + static_cast<std::size_t>(i) * stringItemTemplate.size;
        const std::uint32_t strref = readU32At(data, entryOffset + stringIdField->offset, "TLK V0.5 string id");
        const std::uint32_t bitOffset = readU32At(data, entryOffset + bitOffsetField->offset, "TLK V0.5 string bit offset");
        if (bitOffset == 0u) continue;
        strings.emplace(strref, decodeDa2TlkString(bitOffset, tree, bitstream));
    }
    return strings;
}

} // namespace

void TlkLookup::clear() {
    loaded_ = false;
    filename_.clear();
    languageId_ = 0;
    strings_.clear();
    sparseStrings_.clear();
}

void TlkLookup::load(const std::filesystem::path& file) {
    std::ifstream probeInput(file, std::ios::binary);
    if (!probeInput) throw std::runtime_error("Unable to open TLK file: " + file.string());

    std::array<char, 20> probe{};
    probeInput.read(probe.data(), static_cast<std::streamsize>(probe.size()));
    const std::size_t probeSize = static_cast<std::size_t>(probeInput.gcount());
    const bool isGff4TlkV05 =
        probeSize == probe.size() &&
        (std::string(probe.data(), probe.data() + 8) == "GFF V4.0" ||
         std::string(probe.data(), probe.data() + 8) == "GFF V4.1") &&
        std::string(probe.data() + 8, probe.data() + 12) == "PC  " &&
        std::string(probe.data() + 12, probe.data() + 16) == "TLK " &&
        std::string(probe.data() + 16, probe.data() + 20) == "V0.5";

    clear();
    if (isGff4TlkV05) {
        sparseStrings_ = loadGff4TlkV05(file);
        filename_ = file;
        loaded_ = true;
        return;
    }

    TalkTable table(file.string());
    languageId_ = table.supportsLanguageId() ? table.language() : 0u;
    if (table.hasSparseStrRefs()) {
        sparseStrings_.reserve(table.entries().size());
        for (const TalkString& entry : table.entries()) {
            sparseStrings_[entry.strRef] = entry.text;
        }
    } else {
        strings_.resize(table.entries().size());
        for (const TalkString& entry : table.entries()) {
            if (entry.strRef >= strings_.size()) {
                throw std::runtime_error("TLK entry index exceeds the declared string table size.");
            }
            strings_[static_cast<std::size_t>(entry.strRef)] = entry.text;
        }
    }

    filename_ = file;
    loaded_ = true;
}

std::optional<std::string> TlkLookup::resolve(std::uint32_t strref) const {
    if (!loaded_ || strref == 0xFFFFFFFFu) return std::nullopt;
    if (!strings_.empty()) {
        if (strref >= strings_.size()) return std::nullopt;
        return strings_[static_cast<std::size_t>(strref)];
    }
    const auto found = sparseStrings_.find(strref);
    if (found == sparseStrings_.end()) return std::nullopt;
    return found->second;
}

} // namespace neotlk
