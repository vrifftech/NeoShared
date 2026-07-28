#include "GFFFile.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <vector>
#include <unordered_map>
#include <iomanip>

namespace neogff {

namespace {

constexpr std::size_t GFF_HEADER_SIZE = 56;

std::vector<std::string> splitNonEmpty(const std::string& text, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(delimiter, start);
        const std::size_t stop = (end == std::string::npos) ? text.size() : end;
        if (stop > start) {
            out.emplace_back(text.substr(start, stop - start));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::string fixedToString(const char* data, std::size_t size) {
    return std::string(data, data + size);
}

std::string fixedToDisplayString(const char* data, std::size_t size) {
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(data[i]);
    }
    return out;
}

template <typename T>
T readLittleEndian(std::istream& in) {
    static_assert(std::is_trivially_copyable<T>::value, "POD required");
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!in) {
        throw GffError("Unexpected end of file while reading GFF data.");
    }
    return value;
}

template <typename T>
void writeLittleEndian(std::ostream& out, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "POD required");
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!out) {
        throw GffError("Unable to write GFF data.");
    }
}

void copyStringToFixed(std::string value, char* dest, std::size_t size) {
    if (value.size() > size) {
        value.resize(size);
    }
    std::fill(dest, dest + size, '\0');
    std::copy(value.begin(), value.end(), dest);
}

std::string charsToString(const CharBuffer& chars) {
    return std::string(chars.begin(), chars.end());
}

CharBuffer stringToChars(const std::string& s) {
    return CharBuffer(s.begin(), s.end());
}

template <typename T>
T parseUnsignedBounded(const std::string& text, T maximum, const std::string& context) {
    static_assert(std::is_unsigned<T>::value, "unsigned type required");
    try {
        const std::uint32_t value = ParseUInt32Decimal(text);
        if (value > static_cast<std::uint32_t>(maximum)) {
            throw std::out_of_range("value too large");
        }
        return static_cast<T>(value);
    } catch (const std::exception&) {
        throw GffError("'" + text + "' is not a valid " + context + " value");
    }
}

template <typename T>
T parseSignedBounded(const std::string& text, T minimum, T maximum, const std::string& context) {
    static_assert(std::is_signed<T>::value, "signed type required");
    try {
        const std::int64_t value = ParseInt64Decimal(text);
        if (value < static_cast<std::int64_t>(minimum) || value > static_cast<std::int64_t>(maximum)) {
            throw std::out_of_range("value out of range");
        }
        return static_cast<T>(value);
    } catch (const std::exception&) {
        throw GffError("'" + text + "' is not a valid " + context + " value");
    }
}

template <typename T>
std::unique_ptr<T> cloneBase(const T& src) {
    auto out = std::make_unique<T>();
    out->fieldtype = src.fieldtype;
    out->SetLabel(src.GetLabel());
    return out;
}

template <typename T>
T& checkedGffCast(GffField& field) {
    auto* casted = dynamic_cast<T*>(&field);
    if (casted == nullptr) {
        throw GffError("Invalid class typecast");
    }
    return *casted;
}

template <typename T>
const T& checkedGffCast(const GffField& field) {
    const auto* casted = dynamic_cast<const T*>(&field);
    if (casted == nullptr) {
        throw GffError("Invalid class typecast");
    }
    return *casted;
}

UInt32 checkedUInt32Add(UInt32 lhs, UInt32 rhs, const char* context) {
    if (std::numeric_limits<UInt32>::max() - lhs < rhs) {
        throw GffError(std::string("GFF 32-bit offset/count overflow while ") + context + ".");
    }
    return static_cast<UInt32>(lhs + rhs);
}

UInt32 checkedUInt32Mul(UInt32 lhs, UInt32 rhs, const char* context) {
    if (lhs != 0 && rhs > std::numeric_limits<UInt32>::max() / lhs) {
        throw GffError(std::string("GFF 32-bit offset/count overflow while ") + context + ".");
    }
    return static_cast<UInt32>(lhs * rhs);
}

UInt32 checkedUInt32Sub(UInt32 lhs, UInt32 rhs, const char* context) {
    if (lhs < rhs) {
        throw GffError(std::string("GFF 32-bit offset/count underflow while ") + context + ".");
    }
    return static_cast<UInt32>(lhs - rhs);
}

UInt32 checkedSizeToUInt32(std::size_t value, const char* context) {
    if (value > static_cast<std::size_t>(std::numeric_limits<UInt32>::max())) {
        throw GffError(std::string("GFF size exceeds 32-bit limit while ") + context + ".");
    }
    return static_cast<UInt32>(value);
}

UInt32 checkedCountToUInt32(std::size_t value, const char* context) {
    return checkedSizeToUInt32(value, context);
}

std::int32_t checkedSizeToInt32(std::size_t value, const char* context) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw GffError(std::string("GFF string size exceeds signed 32-bit limit while ") + context + ".");
    }
    return static_cast<std::int32_t>(value);
}

std::uint64_t checkedUInt64Add(std::uint64_t lhs, std::uint64_t rhs, const char* context) {
    if (std::numeric_limits<std::uint64_t>::max() - lhs < rhs) {
        throw GffError(std::string("GFF 64-bit offset/count overflow while ") + context + ".");
    }
    return lhs + rhs;
}

std::uint64_t checkedUInt64Mul(std::uint64_t lhs, std::uint64_t rhs, const char* context) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw GffError(std::string("GFF 64-bit offset/count overflow while ") + context + ".");
    }
    return lhs * rhs;
}

void requireSectionRange(UInt32 sectionCount, UInt32 relativeOffset, std::uint64_t size, const char* context) {
    const std::uint64_t end = checkedUInt64Add(relativeOffset, size, context);
    if (end > sectionCount) {
        throw GffError(std::string("GFF ") + context + " points outside its section.");
    }
}

UInt32 checkedTableOffset(UInt32 base, UInt32 index, UInt32 itemSize, UInt32 count, const char* context) {
    if (index >= count) {
        throw GffError(std::string("GFF ") + context + " index points outside its table.");
    }
    const std::uint64_t relative = checkedUInt64Mul(index, itemSize, context);
    const std::uint64_t absolute = checkedUInt64Add(base, relative, context);
    if (absolute > std::numeric_limits<UInt32>::max()) {
        throw GffError(std::string("GFF ") + context + " offset exceeds 32-bit file format limits.");
    }
    return static_cast<UInt32>(absolute);
}

std::uint64_t bytesToUInt64LE(const Fixed8Bytes& bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

Fixed8Bytes uint64ToBytesLE(std::uint64_t value) {
    Fixed8Bytes bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
    }
    return bytes;
}

std::string formatDisplayChar(char ch) {
    const auto uc = static_cast<unsigned char>(ch);
    switch (ch) {
    case '\0': return "\\0";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default:
        break;
    }
    if (std::isprint(uc)) {
        return std::string(1, ch);
    }
    std::ostringstream out;
    out << "\\x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(uc);
    return out.str();
}


UInt32 readU32LEAt(const std::vector<std::uint8_t>& data, std::size_t offset, const char* context) {
    if (data.size() < 4u || offset > data.size() - 4u) {
        throw GffError(std::string("Malformed GFF V4 file while reading ") + context + ".");
    }
    return static_cast<UInt32>(data[offset]) |
           (static_cast<UInt32>(data[offset + 1]) << 8u) |
           (static_cast<UInt32>(data[offset + 2]) << 16u) |
           (static_cast<UInt32>(data[offset + 3]) << 24u);
}


void ensureGff4Range(const std::vector<std::uint8_t>& data, std::uint64_t offset, std::uint64_t size, const char* context) {
    if (offset > data.size() || size > data.size() || data.size() - static_cast<std::size_t>(offset) < size) {
        throw GffError(std::string("Malformed GFF V4 file: ") + context + " points outside the file.");
    }
}

std::uint16_t readU16LEAt(const std::string& data, std::size_t offset, const char* context) {
    if (data.size() < 2u || offset > data.size() - 2u) {
        throw GffError(std::string("Malformed GFF V4 file while reading ") + context + ".");
    }
    const auto b0 = static_cast<unsigned char>(data[offset]);
    const auto b1 = static_cast<unsigned char>(data[offset + 1u]);
    return static_cast<std::uint16_t>(b0) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b1) << 8u);
}

UInt32 readU32LEAt(const std::string& data, std::size_t offset, const char* context) {
    if (data.size() < 4u || offset > data.size() - 4u) {
        throw GffError(std::string("Malformed GFF V4 file while reading ") + context + ".");
    }
    return static_cast<UInt32>(static_cast<unsigned char>(data[offset])) |
           (static_cast<UInt32>(static_cast<unsigned char>(data[offset + 1u])) << 8u) |
           (static_cast<UInt32>(static_cast<unsigned char>(data[offset + 2u])) << 16u) |
           (static_cast<UInt32>(static_cast<unsigned char>(data[offset + 3u])) << 24u);
}

std::uint64_t readU64LEAt(const std::string& data, std::size_t offset, const char* context) {
    const std::uint64_t lo = readU32LEAt(data, offset, context);
    const std::uint64_t hi = readU32LEAt(data, offset + 4u, context);
    return lo | (hi << 32u);
}

void ensureGff4Range(const std::string& data, std::uint64_t offset, std::uint64_t size, const char* context) {
    if (offset > data.size() || size > data.size() || data.size() - static_cast<std::size_t>(offset) < size) {
        throw GffError(std::string("Malformed GFF V4 file: ") + context + " points outside the file.");
    }
}

std::string formatGff4Hex(UInt32 value) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string gff4LabelToString(UInt32 label) {
    switch (label) {
    case 10000u: return "Name";
    case 10001u: return "Hash";
    case 10002u: return "Columns";
    case 10003u: return "Rows";
    case 10999u: return "ColumnType";
    case 19004u: return "StringID";
    case 19005u: return "BitOffset";
    case 19006u: return "Strings";
    case 19007u: return "HuffmanTree";
    case 19008u: return "HuffmanData";
    default:
        if (label >= 10005u && label < 200000u) {
            return "Column" + std::to_string(label - 10005u);
        }
        return "gff4_" + formatGff4Hex(label);
    }
}

bool parseHex32(const std::string& text, UInt32& value) {
    std::string s = text;
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
    if (s.empty() || s.size() > 8u) return false;
    UInt32 out = 0;
    for (char ch : s) {
        out <<= 4u;
        if (ch >= '0' && ch <= '9') out |= static_cast<UInt32>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') out |= static_cast<UInt32>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') out |= static_cast<UInt32>(ch - 'A' + 10);
        else return false;
    }
    value = out;
    return true;
}

UInt32 gff4LabelFromString(const std::string& label, UInt32 fallback = 0) {
    std::string key = label;
    key.erase(key.begin(), std::find_if(key.begin(), key.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    key.erase(std::find_if(key.rbegin(), key.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), key.end());
    const std::string lower = ToLowerAscii(key);
    if (lower == "name") return 10000u;
    if (lower == "hash") return 10001u;
    if (lower == "columns" || lower == "colm") return 10002u;
    if (lower == "rows") return 10003u;
    if (lower == "columntype" || lower == "type") return 10999u;
    if (lower == "stringid" || lower == "talk_string_id" || lower == "tlkstringid") return 19004u;
    if (lower == "bitoffset" || lower == "stringbitoffset") return 19005u;
    if (lower == "strings" || lower == "hstr") return 19006u;
    if (lower == "huffmantree" || lower == "tree") return 19007u;
    if (lower == "huffmandata" || lower == "encodedstrings" || lower == "bitstream") return 19008u;
    if (lower.rfind("column", 0) == 0 && lower.size() > 6u) {
        const std::string suffix = lower.substr(6u);
        if (IsUnsignedDecimal(suffix)) {
            return checkedUInt32Add(10005u, ParseUInt32Decimal(suffix), "computing GFF V4 column field label");
        }
    }
    UInt32 parsed = 0;
    if (lower.rfind("gff4_", 0) == 0 && parseHex32(lower.substr(5u), parsed)) return parsed;
    if (lower.rfind("hash_", 0) == 0 && parseHex32(lower.substr(5u), parsed)) return parsed;
    if (parseHex32(lower, parsed)) return parsed;
    return fallback;
}

void appendUtf8FromCodepoint(std::string& out, UInt32 cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6u) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12u) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18u) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}


std::string readGff4Utf16String(const std::string& data, UInt32 dataOffset, UInt32 relativeOffset) {
    if (relativeOffset == 0xFFFFFFFFu) return {};
    const std::uint64_t absolute = static_cast<std::uint64_t>(dataOffset) + relativeOffset;
    ensureGff4Range(data, absolute, 4u, "ECString header");
    const UInt32 codeUnits = readU32LEAt(data, static_cast<std::size_t>(absolute), "ECString length");
    const std::uint64_t textOffset = absolute + 4u;
    const std::uint64_t byteCount = static_cast<std::uint64_t>(codeUnits) * 2u;
    ensureGff4Range(data, textOffset, byteCount, "ECString payload");
    std::string out;
    for (UInt32 i = 0; i < codeUnits; ++i) {
        const std::uint16_t unit = readU16LEAt(data, static_cast<std::size_t>(textOffset + static_cast<std::uint64_t>(i) * 2u), "ECString text");
        if (unit == 0) break;
        if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1u < codeUnits) {
            const std::uint16_t next = readU16LEAt(data, static_cast<std::size_t>(textOffset + static_cast<std::uint64_t>(i + 1u) * 2u), "ECString surrogate");
            if (next >= 0xDC00u && next <= 0xDFFFu) {
                const UInt32 cp = 0x10000u + (((static_cast<UInt32>(unit) - 0xD800u) << 10u) | (static_cast<UInt32>(next) - 0xDC00u));
                appendUtf8FromCodepoint(out, cp);
                ++i;
                continue;
            }
        }
        appendUtf8FromCodepoint(out, unit);
    }
    return out;
}

std::vector<std::uint16_t> utf8ToUtf16Units(const std::string& text) {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        UInt32 cp = 0xFFFDu;
        std::size_t advance = 1u;
        if (ch < 0x80u) {
            cp = ch;
        } else if ((ch & 0xE0u) == 0xC0u && i + 1u < text.size()) {
            cp = ((ch & 0x1Fu) << 6u) | (static_cast<unsigned char>(text[i + 1u]) & 0x3Fu);
            advance = 2u;
        } else if ((ch & 0xF0u) == 0xE0u && i + 2u < text.size()) {
            cp = ((ch & 0x0Fu) << 12u) |
                 ((static_cast<unsigned char>(text[i + 1u]) & 0x3Fu) << 6u) |
                 (static_cast<unsigned char>(text[i + 2u]) & 0x3Fu);
            advance = 3u;
        } else if ((ch & 0xF8u) == 0xF0u && i + 3u < text.size()) {
            cp = ((ch & 0x07u) << 18u) |
                 ((static_cast<unsigned char>(text[i + 1u]) & 0x3Fu) << 12u) |
                 ((static_cast<unsigned char>(text[i + 2u]) & 0x3Fu) << 6u) |
                 (static_cast<unsigned char>(text[i + 3u]) & 0x3Fu);
            advance = 4u;
        }
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<std::uint16_t>(cp));
        } else {
            cp -= 0x10000u;
            out.push_back(static_cast<std::uint16_t>(0xD800u + ((cp >> 10u) & 0x3FFu)));
            out.push_back(static_cast<std::uint16_t>(0xDC00u + (cp & 0x3FFu)));
        }
        i += advance;
    }
    out.push_back(0u);
    return out;
}

UInt32 gff4PrimitiveStorageSize(std::uint16_t type) {
    switch (type) {
    case 0: return 1;
    case 1: return 1;
    case 2: return 2;
    case 3: return 2;
    case 4: return 4;
    case 5: return 4;
    case 6: return 8;
    case 7: return 8;
    case 8: return 4;
    case 9: return 8;
    case 10: return 12;
    case 12: return 16;
    case 13: return 16;
    case 14: return 4;  // ECString reference.
    case 15: return 16;
    case 16: return 64;
    case 17: return 8;
    default: return 4;
    }
}

constexpr UInt32 kGff4ExpandedPrimitiveListItemLimit = 4096u;
constexpr UInt32 kGff4ExpandedPrimitiveListByteLimit = 65536u;

bool shouldCompactGff4PrimitiveList(UInt32 count, UInt32 itemSize) {
    return count > kGff4ExpandedPrimitiveListItemLimit ||
           checkedUInt64Mul(count, itemSize, "checking GFF V4 primitive-list size") > kGff4ExpandedPrimitiveListByteLimit;
}

void appendU32LE(std::vector<std::uint8_t>& out, UInt32 value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

void writeU32LETo(std::vector<std::uint8_t>& out, std::size_t offset, UInt32 value) {
    if (offset > out.size() || out.size() - offset < 4u) throw GffError("Internal GFF V4 writer offset error.");
    out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    out[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
    out[offset + 2u] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
    out[offset + 3u] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
}

void writeBytesTo(std::vector<std::uint8_t>& out, std::size_t offset, const void* data, std::size_t size) {
    if (offset > out.size() || out.size() - offset < size) throw GffError("Internal GFF V4 writer range error.");
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::copy(bytes, bytes + size, out.begin() + static_cast<std::ptrdiff_t>(offset));
}

UInt32 checkedVectorOffset(std::size_t value, const char* context) {
    if (value > static_cast<std::size_t>(std::numeric_limits<UInt32>::max())) {
        throw GffError(std::string("GFF V4 data block exceeds 32-bit offset range while ") + context + ".");
    }
    return static_cast<UInt32>(value);
}

GffLocalizedSubstring& requirePhysicalExoLocSubstring(GffLocalizedStringField& loc, UInt32 index, const char* action) {
    if (static_cast<std::size_t>(index) >= loc.substrings.size()) {
        throw GffError(std::string("Access violation ") + action + " CExoLocString substring text.");
    }
    return loc.substrings[static_cast<std::size_t>(index)];
}

const GffLocalizedSubstring& requirePhysicalExoLocSubstring(const GffLocalizedStringField& loc, UInt32 index, const char* action) {
    if (static_cast<std::size_t>(index) >= loc.substrings.size()) {
        throw GffError(std::string("Access violation ") + action + " CExoLocString substring text.");
    }
    return loc.substrings[static_cast<std::size_t>(index)];
}

} // namespace

class GffFile::BinaryReader {
public:
    explicit BinaryReader(const std::filesystem::path& path)
        : data(ReadRegularFileBytes(path)), stream(data, std::ios::in | std::ios::binary) {
        if (!stream) {
            throw GffError("Unable to open GFF file for reading: " + path.string());
        }
    }

    void seek(UInt32 offset) {
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream) {
            throw GffError("Unable to seek while reading GFF file.");
        }
    }

    UInt32 position() {
        return static_cast<UInt32>(stream.tellg());
    }

    template <typename T>
    T read() {
        return readLittleEndian<T>(stream);
    }

    void readBytes(char* dest, std::size_t size) {
        if (size == 0) {
            return;
        }
        stream.read(dest, static_cast<std::streamsize>(size));
        if (!stream) {
            throw GffError("Unexpected end of file while reading GFF byte block.");
        }
    }

    std::string data;
    std::istringstream stream;
};

class GffFile::BinaryWriter {
public:
    explicit BinaryWriter(const std::filesystem::path& path)
        : file(path) {}

    void seek(UInt32 offset) {
        file.seek(offset);
    }

    UInt32 position() {
        const auto pos = file.position();
        if (pos > std::numeric_limits<UInt32>::max()) {
            throw GffError("GFF output grew beyond a 32-bit offset.");
        }
        return static_cast<UInt32>(pos);
    }

    template <typename T>
    void write(const T& value) {
        static_assert(std::is_trivially_copyable<T>::value, "POD required");
        file.writePod(value);
    }

    void writeBytes(const char* data, std::size_t size) {
        file.writeBytes(data, size);
    }

    void writeZero(std::size_t size) {
        static const std::array<char, 16> zeros{};
        while (size > 0) {
            const std::size_t chunk = std::min<std::size_t>(size, zeros.size());
            writeBytes(zeros.data(), chunk);
            size -= chunk;
        }
    }

    void close() {
        file.close();
    }

    SafeOutputFile file;
};

GffFile::~GffFile() = default;

// GffField

GffField::GffField() {
    label_.fill('\0');
}

GffField::GffField(const std::string& label) {
    label_.fill('\0');
    SetLabel(label);
}

void GffField::SetLabel(std::string label) {
    copyStringToFixed(std::move(label), label_.data(), label_.size());
}

void GffField::SetLabelRaw(const Fixed16Chars& label) {
    label_ = label;
}

std::string GffField::GetLabel() const {
    std::string out;
    for (char c : label_) {
        if (c != '\0') {
            out.push_back(c);
        }
    }
    return out;
}

Fixed16Chars GffField::GetLabelRaw() const {
    return label_;
}

std::string GffField::GetString() const {
    // Dispatch by the stored GFF field type so callers can render a field through the base class.
    switch (fieldtype) {
    case FIELD_TYPE_BYTE:
        return std::to_string(checkedGffCast<GffByteField>(*this).value);
    case FIELD_TYPE_CHAR:
        return formatDisplayChar(checkedGffCast<GffCharField>(*this).value);
    case FIELD_TYPE_WORD:
        return std::to_string(checkedGffCast<GffWordField>(*this).value);
    case FIELD_TYPE_SHORT:
        return std::to_string(checkedGffCast<GffShortField>(*this).value);
    case FIELD_TYPE_DWORD:
        return std::to_string(checkedGffCast<GffUInt32Field>(*this).value);
    case FIELD_TYPE_INT:
        return std::to_string(checkedGffCast<GffIntField>(*this).value);
    case FIELD_TYPE_DWORD64:
        return std::to_string(checkedGffCast<GffUInt64Field>(*this).value);
    case FIELD_TYPE_INT64:
        return std::to_string(checkedGffCast<GffInt64Field>(*this).value);
    case FIELD_TYPE_FLOAT:
        return FormatNumber(checkedGffCast<GffFloatField>(*this).value);
    case FIELD_TYPE_DOUBLE:
        return FormatNumber(checkedGffCast<GffDoubleField>(*this).value);
    case FIELD_TYPE_CEXOSTRING:
        return charsToString(checkedGffCast<GffExoStringField>(*this).textData);
    case FIELD_TYPE_RESREF:
        return charsToString(checkedGffCast<GffResRefField>(*this).textData);
    case FIELD_TYPE_CEXOLOCSTRING: {
        const auto& loc = checkedGffCast<GffLocalizedStringField>(*this);
        std::string out = "LocString[strref=";
        out += (loc.strref == 0xFFFFFFFFu) ? "-1" : std::to_string(loc.strref);
        out += ", substrings=" + std::to_string(loc.stringcount) + ", strings=";
        for (UInt32 i = 0; i < loc.stringcount; ++i) {
            out += "(" + requirePhysicalExoLocSubstring(loc, i, "reading").GetString() + ")";
        }
        out += "]";
        return out;
    }
    case FIELD_TYPE_VOID:
        return "(Raw Binary data, size=" + std::to_string(checkedGffCast<GffVoidField>(*this).bytesize) + ")";
    case FIELD_TYPE_STRUCT:
        return "[STRUCT type=" + std::to_string(checkedGffCast<GffStruct>(*this).typeid_) + "]";
    case FIELD_TYPE_LIST: {
        const auto& list = checkedGffCast<GffList>(*this);
        if (list.gff4CompactPrimitiveList) {
            return "[PRIMITIVE LIST items=" + std::to_string(list.gff4PrimitiveListCount) +
                   ", bytes=" + std::to_string(list.gff4PrimitiveListData.size()) + "]";
        }
        return "[LIST]";
    }
    case FIELD_TYPE_ORIENTATION:
        return FormatGffVector4Text(checkedGffCast<GffOrientationField>(*this).value);
    case FIELD_TYPE_POSITION:
        return FormatGffVector3Text(checkedGffCast<GffPositionField>(*this).value);
    case FIELD_TYPE_JADE_STRREF: {
        const auto& text = checkedGffCast<GffJadeStringRefField>(*this);
        return std::to_string(text.stringType) + "|" + (text.strref == 0xFFFFFFFFu ? std::string("-1") : std::to_string(text.strref));
    }
    default:
        return {};
    }
}

std::unique_ptr<GffField> GffField::Clone() const {
    std::unique_ptr<GffField> out;

    switch (fieldtype) {
    case FIELD_TYPE_BYTE: {
        auto clone = std::make_unique<GffByteField>();
        clone->value = checkedGffCast<GffByteField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_CHAR: {
        auto clone = std::make_unique<GffCharField>();
        clone->value = checkedGffCast<GffCharField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_WORD: {
        auto clone = std::make_unique<GffWordField>();
        clone->value = checkedGffCast<GffWordField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_SHORT: {
        auto clone = std::make_unique<GffShortField>();
        clone->value = checkedGffCast<GffShortField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_DWORD: {
        auto clone = std::make_unique<GffUInt32Field>();
        clone->value = checkedGffCast<GffUInt32Field>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_INT: {
        auto clone = std::make_unique<GffIntField>();
        clone->value = checkedGffCast<GffIntField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_DWORD64: {
        auto clone = std::make_unique<GffUInt64Field>();
        clone->value = checkedGffCast<GffUInt64Field>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_INT64: {
        auto clone = std::make_unique<GffInt64Field>();
        clone->value = checkedGffCast<GffInt64Field>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_FLOAT: {
        auto clone = std::make_unique<GffFloatField>();
        clone->value = checkedGffCast<GffFloatField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_DOUBLE: {
        auto clone = std::make_unique<GffDoubleField>();
        clone->value = checkedGffCast<GffDoubleField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_CEXOSTRING: {
        auto clone = std::make_unique<GffExoStringField>();
        clone->SetString(charsToString(checkedGffCast<GffExoStringField>(*this).textData));
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_RESREF: {
        auto clone = std::make_unique<GffResRefField>();
        clone->SetString(charsToString(checkedGffCast<GffResRefField>(*this).textData));
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_CEXOLOCSTRING: {
        const auto& loc = checkedGffCast<GffLocalizedStringField>(*this);
        auto clone = std::make_unique<GffLocalizedStringField>();
        clone->strref = loc.strref;
        for (const auto& sub : loc.substrings) {
            clone->AddString(sub.stringid, sub.GetString());
        }
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_VOID: {
        const auto& voidField = checkedGffCast<GffVoidField>(*this);
        auto clone = std::make_unique<GffVoidField>();
        clone->bytesize = voidField.bytesize;
        clone->data = voidField.data;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_STRUCT: {
        const auto& structure = checkedGffCast<GffStruct>(*this);
        auto clone = std::make_unique<GffStruct>();
        clone->typeid_ = structure.typeid_;
        clone->gff4TemplateIndex = structure.gff4TemplateIndex;
        for (std::size_t i = 0; i < structure.count(); ++i) {
            const GffField* child = structure.GetField(i);
            if (!child) {
                throw GffError("Invalid null field encountered when cloning struct \"" + structure.GetLabel() + "\"!");
            }
            auto childClone = child->Clone();
            if (!childClone) {
                throw GffError("Unable to clone field \"" + child->GetLabel() + "\" in struct \"" + structure.GetLabel() + "\"!");
            }
            clone->AddField(std::move(childClone));
        }
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_LIST: {
        const auto& list = checkedGffCast<GffList>(*this);
        auto clone = std::make_unique<GffList>();
        clone->hasGff4LabelId = list.hasGff4LabelId;
        clone->gff4LabelId = list.gff4LabelId;
        clone->gff4BaseType = list.gff4BaseType;
        clone->gff4Flags = list.gff4Flags;
        clone->gff4CompactPrimitiveList = list.gff4CompactPrimitiveList;
        clone->gff4PrimitiveListCount = list.gff4PrimitiveListCount;
        clone->gff4PrimitiveItemSize = list.gff4PrimitiveItemSize;
        clone->gff4PrimitiveListData = list.gff4PrimitiveListData;
        for (std::size_t i = 0; i < list.count(); ++i) {
            const GffStruct* sourceStruct = list.GetStruct(i);
            if (!sourceStruct) {
                throw GffError("Invalid null STRUCT encountered when cloning List \"" + list.GetLabel() + "\"!");
            }
            auto structClone = sourceStruct->Clone();
            if (!structClone) {
                throw GffError("Unable to clone struct " + std::to_string(i) + " in List \"" + list.GetLabel() + "\"!");
            }
            auto* asStruct = dynamic_cast<GffStruct*>(structClone.get());
            if (!asStruct) {
                throw GffError("Unable to clone struct " + std::to_string(i) + " in List \"" + list.GetLabel() + "\"!");
            }
            clone->AddStruct(std::unique_ptr<GffStruct>(static_cast<GffStruct*>(structClone.release())));
        }
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_ORIENTATION: {
        auto clone = std::make_unique<GffOrientationField>();
        clone->value = checkedGffCast<GffOrientationField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_POSITION: {
        auto clone = std::make_unique<GffPositionField>();
        clone->value = checkedGffCast<GffPositionField>(*this).value;
        out = std::move(clone);
        break;
    }
    case FIELD_TYPE_JADE_STRREF: {
        const auto& source = checkedGffCast<GffJadeStringRefField>(*this);
        auto clone = std::make_unique<GffJadeStringRefField>();
        clone->stringType = source.stringType;
        clone->strref = source.strref;
        out = std::move(clone);
        break;
    }
    default:
        return nullptr;
    }

    if (out) {
        out->SetLabel(GetLabel());
        out->fieldtype = fieldtype;
        out->hasGff4LabelId = hasGff4LabelId;
        out->gff4LabelId = gff4LabelId;
        out->gff4BaseType = gff4BaseType;
        out->gff4Flags = gff4Flags;
    }
    return out;
}

// GffStruct

GffStruct::GffStruct() : GffField() {
    fieldtype = FIELD_TYPE_STRUCT;
}

GffStruct::GffStruct(const std::string& label) : GffField(label) {
    fieldtype = FIELD_TYPE_STRUCT;
}

GffField* GffStruct::GetFieldByLabel(const std::string& label) {
    return GetFieldByLabel(label, 0u);
}

const GffField* GffStruct::GetFieldByLabel(const std::string& label) const {
    return GetFieldByLabel(label, 0u);
}

GffField* GffStruct::GetFieldByLabel(const std::string& label, std::size_t occurrence) {
    std::size_t matched = 0;
    for (auto& field : fields_) {
        if (!field) {
            throw GffError("Invalid null field encountered when searching Struct " + GetLabel() + "!");
        }
        if (field->GetLabel() == label) {
            if (matched == occurrence) return field.get();
            ++matched;
        }
    }
    return nullptr;
}

const GffField* GffStruct::GetFieldByLabel(const std::string& label, std::size_t occurrence) const {
    std::size_t matched = 0;
    for (const auto& field : fields_) {
        if (!field) {
            throw GffError("Invalid null field encountered when searching Struct " + GetLabel() + "!");
        }
        if (field->GetLabel() == label) {
            if (matched == occurrence) return field.get();
            ++matched;
        }
    }
    return nullptr;
}

GffField* GffStruct::GetField(int index) {
    if (index < 0) {
        throw GffError("Out of bounds index encountered when attempting to retrieve field from struct " + GetLabel() + "!");
    }
    return GetField(static_cast<std::size_t>(index));
}

const GffField* GffStruct::GetField(int index) const {
    if (index < 0) {
        throw GffError("Out of bounds index encountered when attempting to retrieve field from struct " + GetLabel() + "!");
    }
    return GetField(static_cast<std::size_t>(index));
}

GffField* GffStruct::GetField(std::size_t index) {
    if (index >= fields_.size()) {
        throw GffError("Out of bounds index encountered when attempting to retrieve field from struct " + GetLabel() + "!");
    }
    return fields_[index].get();
}

const GffField* GffStruct::GetField(std::size_t index) const {
    if (index >= fields_.size()) {
        throw GffError("Out of bounds index encountered when attempting to retrieve field from struct " + GetLabel() + "!");
    }
    return fields_[index].get();
}

void GffStruct::AddField(std::unique_ptr<GffField> field) {
    if (!field) {
        throw GffError("Invalid null field encountered when adding field to Struct " + GetLabel() + "!");
    }
    // Jade Empire SAV resources can contain repeated labels within one
    // struct. Preserve each occurrence and its original order.
    fields_.push_back(std::move(field));
}

void GffStruct::AddField(GffField* field) {
    AddField(std::unique_ptr<GffField>(field));
}

void GffStruct::DeleteField(const std::string& label) {
    DeleteField(label, 0u);
}

void GffStruct::DeleteField(const std::string& label, std::size_t occurrence) {
    std::size_t matched = 0;
    auto it = fields_.begin();
    for (; it != fields_.end(); ++it) {
        if (!*it) {
            throw GffError("Invalid null field encountered when deleting field from Struct " + GetLabel() + "!");
        }
        if ((*it)->GetLabel() == label) {
            if (matched == occurrence) break;
            ++matched;
        }
    }
    if (it == fields_.end()) {
        throw GffError("Unable to delete field occurrence " + std::to_string(occurrence + 1u) +
                       " with label " + label + " from Struct " + GetLabel() + "!");
    }
    fields_.erase(it);
}

std::string GffStruct::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffStruct::Clone() const { return GffField::Clone(); }

// GffList

GffList::GffList() : GffField() {
    fieldtype = FIELD_TYPE_LIST;
}

GffList::GffList(const std::string& label) : GffField(label) {
    fieldtype = FIELD_TYPE_LIST;
}

GffStruct* GffList::GetStruct(int index) {
    if (index < 0) {
        throw GffError("Out of bounds index " + std::to_string(index) + " when trying to get STRUCT from LIST " + GetLabel() + "!");
    }
    return GetStruct(static_cast<std::size_t>(index));
}

const GffStruct* GffList::GetStruct(int index) const {
    if (index < 0) {
        throw GffError("Out of bounds index " + std::to_string(index) + " when trying to get STRUCT from LIST " + GetLabel() + "!");
    }
    return GetStruct(static_cast<std::size_t>(index));
}

GffStruct* GffList::GetStruct(std::size_t index) {
    if (index >= structs_.size()) {
        throw GffError("Out of bounds index " + std::to_string(index) + " when trying to get STRUCT from LIST " + GetLabel() + "!");
    }
    return structs_[index].get();
}

const GffStruct* GffList::GetStruct(std::size_t index) const {
    if (index >= structs_.size()) {
        throw GffError("Out of bounds index " + std::to_string(index) + " when trying to get STRUCT from LIST " + GetLabel() + "!");
    }
    return structs_[index].get();
}

void GffList::AddStruct(std::unique_ptr<GffStruct> structure) {
    if (!structure) {
        throw GffError("Invalid null STRUCT encountered when adding to LIST " + GetLabel() + "!");
    }
    if (!structure->GetLabel().empty()) {
        structure->SetLabel("");
    }
    structs_.push_back(std::move(structure));
}

void GffList::AddStruct(GffStruct* structure) {
    AddStruct(std::unique_ptr<GffStruct>(structure));
}

void GffList::DeleteStruct(UInt32 index) {
    if (index >= structs_.size()) {
        throw GffError("Out of bounds index " + std::to_string(index) + " when trying to delete STRUCT from LIST " + GetLabel() + "!");
    }
    structs_.erase(structs_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::string GffList::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffList::Clone() const { return GffField::Clone(); }

// Simple and complex field wrappers

GffByteField::GffByteField() { fieldtype = FIELD_TYPE_BYTE; }
GffByteField::GffByteField(const std::string& label, std::uint8_t data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_BYTE; }
std::string GffByteField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffByteField::Clone() const { return GffField::Clone(); }

GffCharField::GffCharField() { fieldtype = FIELD_TYPE_CHAR; }
GffCharField::GffCharField(const std::string& label, char data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_CHAR; }
std::string GffCharField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffCharField::Clone() const { return GffField::Clone(); }

GffWordField::GffWordField() { fieldtype = FIELD_TYPE_WORD; }
GffWordField::GffWordField(const std::string& label, std::uint16_t data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_WORD; }
std::string GffWordField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffWordField::Clone() const { return GffField::Clone(); }

GffShortField::GffShortField() { fieldtype = FIELD_TYPE_SHORT; }
GffShortField::GffShortField(const std::string& label, std::int16_t data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_SHORT; }
std::string GffShortField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffShortField::Clone() const { return GffField::Clone(); }

GffUInt32Field::GffUInt32Field() { fieldtype = FIELD_TYPE_DWORD; }
GffUInt32Field::GffUInt32Field(const std::string& label, UInt32 data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_DWORD; }
std::string GffUInt32Field::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffUInt32Field::Clone() const { return GffField::Clone(); }

GffIntField::GffIntField() { fieldtype = FIELD_TYPE_INT; }
GffIntField::GffIntField(const std::string& label, std::int32_t data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_INT; }
std::string GffIntField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffIntField::Clone() const { return GffField::Clone(); }

GffUInt64Field::GffUInt64Field() { fieldtype = FIELD_TYPE_DWORD64; }
GffUInt64Field::GffUInt64Field(const std::string& label, std::uint64_t data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_DWORD64; }
GffUInt64Field::GffUInt64Field(const std::string& label, const Fixed8Bytes& data) : GffField(label), value(bytesToUInt64LE(data)) { fieldtype = FIELD_TYPE_DWORD64; }
std::string GffUInt64Field::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffUInt64Field::Clone() const { return GffField::Clone(); }

GffInt64Field::GffInt64Field() { fieldtype = FIELD_TYPE_INT64; }
GffInt64Field::GffInt64Field(const std::string& label, std::int64_t data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_INT64; }
std::string GffInt64Field::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffInt64Field::Clone() const { return GffField::Clone(); }

GffFloatField::GffFloatField() { fieldtype = FIELD_TYPE_FLOAT; }
GffFloatField::GffFloatField(const std::string& label, float data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_FLOAT; }
std::string GffFloatField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffFloatField::Clone() const { return GffField::Clone(); }

GffDoubleField::GffDoubleField() { fieldtype = FIELD_TYPE_DOUBLE; }
GffDoubleField::GffDoubleField(const std::string& label, double data) : GffField(label), value(data) { fieldtype = FIELD_TYPE_DOUBLE; }
std::string GffDoubleField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffDoubleField::Clone() const { return GffField::Clone(); }

GffExoStringField::GffExoStringField() { fieldtype = FIELD_TYPE_CEXOSTRING; }
GffExoStringField::GffExoStringField(const std::string& label, const std::string& data) : GffField(label) { fieldtype = FIELD_TYPE_CEXOSTRING; SetString(data); }
void GffExoStringField::SetString(const std::string& text) { size = checkedSizeToUInt32(text.size(), "setting CExoString text"); textData = stringToChars(text); }
std::string GffExoStringField::GetString() const { return charsToString(textData); }
std::unique_ptr<GffField> GffExoStringField::Clone() const { return GffField::Clone(); }

GffResRefField::GffResRefField() { fieldtype = FIELD_TYPE_RESREF; }
GffResRefField::GffResRefField(const std::string& label, const std::string& data) : GffField(label) { fieldtype = FIELD_TYPE_RESREF; SetString(data); }
void GffResRefField::SetString(const std::string& text) {
    std::string stored = text;
    if (stored.size() > std::numeric_limits<std::uint8_t>::max()) {
        stored.resize(std::numeric_limits<std::uint8_t>::max());
    }
    size = static_cast<std::uint8_t>(stored.size());
    textData = stringToChars(stored);
}
std::string GffResRefField::GetString() const { return charsToString(textData); }
std::unique_ptr<GffField> GffResRefField::Clone() const { return GffField::Clone(); }

GffLocalizedSubstring::GffLocalizedSubstring(std::int32_t id, const std::string& text) : stringid(id) { SetString(text); }
void GffLocalizedSubstring::SetString(const std::string& text) { stringlength = checkedSizeToInt32(text.size(), "setting CExoLocString substring text"); textData = stringToChars(text); }
std::string GffLocalizedSubstring::GetString() const { return charsToString(textData); }

GffLocalizedStringField::GffLocalizedStringField() { fieldtype = FIELD_TYPE_CEXOLOCSTRING; }
GffLocalizedStringField::GffLocalizedStringField(const std::string& label, UInt32 strrefValue) : GffField(label), strref(strrefValue) { fieldtype = FIELD_TYPE_CEXOLOCSTRING; }

void GffLocalizedStringField::recalcByteSize() {
    bytesize = 8;
    for (const auto& s : substrings) {
        bytesize = checkedUInt32Add(bytesize, checkedUInt32Add(static_cast<UInt32>(s.stringlength), 8u, "recalculating CExoLocString substring size"), "recalculating CExoLocString byte size");
    }
}

void GffLocalizedStringField::recalcByteSizeUsingStringCount() {
    bytesize = 8;
    for (UInt32 i = 0; i < stringcount; ++i) {
        bytesize = checkedUInt32Add(bytesize, checkedUInt32Add(static_cast<UInt32>(requirePhysicalExoLocSubstring(*this, i, "recalculating").stringlength), 8u, "recalculating CExoLocString substring size"), "recalculating CExoLocString byte size");
    }
}

void GffLocalizedStringField::AddString(int langId, const std::string& text) {
    if (langId < 0) {
        throw GffError("Invalid Language ID specified when adding CExoLocString substring!");
    }
    for (auto& s : substrings) {
        if (s.stringid == langId) {
            s.SetString(text);
            recalcByteSize();
            return;
        }
    }
    substrings.emplace_back(langId, text);
    ++stringcount;
    recalcByteSize();
}

void GffLocalizedStringField::SetString(int index, const std::string& text) {
    if (index < 0 || static_cast<UInt32>(index) >= stringcount) {
        throw GffError("Invalid substring index when setting CExoLocString text!");
    }
    requirePhysicalExoLocSubstring(*this, static_cast<UInt32>(index), "setting").SetString(text);
    recalcByteSizeUsingStringCount();
}

void GffLocalizedStringField::DeleteString(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= substrings.size()) {
        throw GffError("Invalid substring index when deleting CExoLocString substring!");
    }
    substrings.erase(substrings.begin() + index);
    --stringcount;
    recalcByteSize();
}

void GffLocalizedStringField::DeleteStringByID(int langId) {
    for (std::size_t i = 0; i < substrings.size(); ++i) {
        if (substrings[i].stringid == langId) {
            DeleteString(static_cast<int>(i));
            return;
        }
    }
}

void GffLocalizedStringField::SetStringByID(int langId, const std::string& text) {
    AddString(langId, text);
}

std::string GffLocalizedStringField::GetStringById(int langId) const {
    for (const auto& s : substrings) {
        if (s.stringid == langId) {
            return s.GetString();
        }
    }
    return {};
}

std::string GffLocalizedStringField::GetString(int index) const {
    if (index < 0 || static_cast<UInt32>(index) >= stringcount) {
        throw GffError("Invalid substring index when getting CExoLocString text!");
    }
    return requirePhysicalExoLocSubstring(*this, static_cast<UInt32>(index), "reading").GetString();
}

std::string GffLocalizedStringField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffLocalizedStringField::Clone() const { return GffField::Clone(); }

GffVoidField::GffVoidField() { fieldtype = FIELD_TYPE_VOID; }
GffVoidField::GffVoidField(const std::string& label, const ByteBuffer& bytes) : GffField(label), bytesize(checkedSizeToUInt32(bytes.size(), "constructing CVoid data")), data(bytes) { fieldtype = FIELD_TYPE_VOID; }
std::string GffVoidField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffVoidField::Clone() const { return GffField::Clone(); }

GffJadeStringRefField::GffJadeStringRefField() { fieldtype = FIELD_TYPE_JADE_STRREF; }
GffJadeStringRefField::GffJadeStringRefField(const std::string& label, UInt32 typeValue, UInt32 strrefValue) : GffField(label), stringType(typeValue), strref(strrefValue) { fieldtype = FIELD_TYPE_JADE_STRREF; }
std::string GffJadeStringRefField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffJadeStringRefField::Clone() const { return GffField::Clone(); }

GffOrientationField::GffOrientationField() { fieldtype = FIELD_TYPE_ORIENTATION; }
GffOrientationField::GffOrientationField(const std::string& label, float d1, float d2, float d3, float d4) : GffField(label), value{d1, d2, d3, d4} { fieldtype = FIELD_TYPE_ORIENTATION; }
std::string GffOrientationField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffOrientationField::Clone() const { return GffField::Clone(); }

GffPositionField::GffPositionField() { fieldtype = FIELD_TYPE_POSITION; }
GffPositionField::GffPositionField(const std::string& label, float x, float y, float z) : GffField(label), value{x, y, z} { fieldtype = FIELD_TYPE_POSITION; }
std::string GffPositionField::GetString() const { return GffField::GetString(); }
std::unique_ptr<GffField> GffPositionField::Clone() const { return GffField::Clone(); }

// GffFile

GffFile::GffFile() {
    filetypeRaw_.fill('\0');
    fileversionRaw_.fill('\0');
}

void GffFile::ResetAll() {
    isLoaded_ = false;
    isDirty_ = false;
    filename_.clear();
    rootStruct_.reset();
    filetypeRaw_.fill('\0');
    fileversionRaw_.fill('\0');
    reader_.reset();
    writer_.reset();
    header_ = Header{};
    saveData_ = SaveData{};
    isGff4_ = false;
    gff4PlatformRaw_ = FourChar{{'P', 'C', ' ', ' '}};
    gff4Templates_.clear();
    gff4DataOffset_ = 0;
}

void GffFile::SetType(std::string type) {
    isDirty_ = true;
    filetypeRaw_.fill('\0');
    const auto n = std::min(filetypeRaw_.size(), type.size());
    std::copy_n(type.begin(), n, filetypeRaw_.begin());
}

void GffFile::SetVersion(std::string versionText) {
    isDirty_ = true;
    fileversionRaw_.fill('\0');
    const auto n = std::min(fileversionRaw_.size(), versionText.size());
    std::copy_n(versionText.begin(), n, fileversionRaw_.begin());
}

std::string GffFile::GetType() const {
    return fixedToDisplayString(filetypeRaw_.data(), filetypeRaw_.size());
}

std::string GffFile::GetVersion() const {
    return fixedToDisplayString(fileversionRaw_.data(), fileversionRaw_.size());
}

std::string GffFile::filetype() const { return GetType(); }
void GffFile::filetype(const std::string& type) { SetType(type); }
std::string GffFile::version() const { return GetVersion(); }
void GffFile::version(const std::string& versionText) { SetVersion(versionText); }

void GffFile::NewFile(const std::string& type, const std::filesystem::path& filename) {
    ResetAll();
    filename_ = filename;
    SetType(type);
    SetVersion("V3.2");
    rootStruct_ = std::make_unique<GffStruct>();
    rootStruct_->typeid_ = 0xFFFFFFFFu;
    isLoaded_ = true;
    isDirty_ = false;
}

void GffFile::ValidateCanonicalLayout(std::uint64_t fileSize) const {
    auto checkedEnd = [](std::uint64_t offset, std::uint64_t size, const char* section) {
        if (std::numeric_limits<std::uint64_t>::max() - offset < size) {
            throw GffError(std::string("GFF section overflow in ") + section + ".");
        }
        return offset + size;
    };

    std::uint64_t expected = GFF_HEADER_SIZE;
    auto requireOffset = [&](UInt32 actual, const char* section) {
        if (static_cast<std::uint64_t>(actual) != expected) {
            throw GffError(std::string("Noncanonical GFF layout: ") + section + " does not start at the expected offset.");
        }
    };

    requireOffset(header_.structoffset, "Struct Array");
    expected = checkedEnd(expected, static_cast<std::uint64_t>(header_.structcount) * 12u, "Struct Array");

    requireOffset(header_.fieldoffset, "Field Array");
    expected = checkedEnd(expected, static_cast<std::uint64_t>(header_.fieldcount) * 12u, "Field Array");

    requireOffset(header_.labeloffset, "Label Array");
    expected = checkedEnd(expected, static_cast<std::uint64_t>(header_.labelcount) * 16u, "Label Array");

    requireOffset(header_.fielddataoffset, "Field Data");
    expected = checkedEnd(expected, header_.fielddatacount, "Field Data");

    requireOffset(header_.fieldindexoffset, "Field Indices");
    expected = checkedEnd(expected, header_.fieldindexcount, "Field Indices");

    requireOffset(header_.listindexoffset, "List Indices");
    expected = checkedEnd(expected, header_.listindexcount, "List Indices");

    if (expected != fileSize) {
        throw GffError("Noncanonical GFF layout: trailing bytes or truncated section data are present.");
    }
}

void GffFile::LoadFile(const std::filesystem::path& filename) {
    std::error_code fileEc;
    const bool loadableFile = std::filesystem::is_regular_file(filename, fileEc);
    if (fileEc || !loadableFile) {
        throw GffError("Specified file " + filename.string() + " could not be found to be opened!");
    }

    // Load transactionally. A bad or truncated file should not erase the current
    // in-memory GFF object, because callers may use this backend object for
    // unsaved edits or a currently displayed file.
    const FourChar previousFileType = filetypeRaw_;
    const FourChar previousFileVersion = fileversionRaw_;
    const std::filesystem::path previousFilename = filename_;
    auto previousRootStruct = std::move(rootStruct_);
    const std::size_t previousCurrField = currField_;
    const bool previousLoaded = isLoaded_;
    const bool previousDirty = isDirty_;
    const Header previousHeader = header_;
    const SaveData previousSaveData = saveData_;
    const bool previousIsGff4 = isGff4_;
    const FourChar previousGff4Platform = gff4PlatformRaw_;
    const auto previousGff4Templates = gff4Templates_;
    const UInt32 previousGff4DataOffset = gff4DataOffset_;
    auto previousReader = std::move(reader_);
    auto previousWriter = std::move(writer_);

    auto restorePrevious = [&]() {
        filetypeRaw_ = previousFileType;
        fileversionRaw_ = previousFileVersion;
        filename_ = previousFilename;
        rootStruct_ = std::move(previousRootStruct);
        currField_ = previousCurrField;
        isLoaded_ = previousLoaded;
        isDirty_ = previousDirty;
        header_ = previousHeader;
        saveData_ = previousSaveData;
        isGff4_ = previousIsGff4;
        gff4PlatformRaw_ = previousGff4Platform;
        gff4Templates_ = previousGff4Templates;
        gff4DataOffset_ = previousGff4DataOffset;
        reader_ = std::move(previousReader);
        writer_ = std::move(previousWriter);
    };

    try {
        ResetAll();
        currField_ = previousCurrField;
        reader_ = std::make_unique<BinaryReader>(filename);

        reader_->readBytes(header_.filetype.data(), 4);
        reader_->readBytes(header_.fileversion.data(), 4);

        if (fixedToString(header_.filetype.data(), 4) == "GFF " &&
            (fixedToString(header_.fileversion.data(), 4) == "V4.0" || fixedToString(header_.fileversion.data(), 4) == "V4.1")) {
            reader_.reset();
            LoadGff4File(filename);
            return;
        }

        const std::string classicVersion = fixedToString(header_.fileversion.data(), 4);
        if (classicVersion != "V3.2" && classicVersion != "V3.3") {
            throw GffError("Unsupported or non-GFF resource. The shared GFF core supports classic GFF V3.2/V3.3 resources and Dragon Age GFF V4.0/V4.1 containers.");
        }

        filename_ = filename;
        filetypeRaw_ = header_.filetype;
        fileversionRaw_ = header_.fileversion;

        header_.structoffset = reader_->read<UInt32>();
        header_.structcount = reader_->read<UInt32>();
        header_.fieldoffset = reader_->read<UInt32>();
        header_.fieldcount = reader_->read<UInt32>();
        header_.labeloffset = reader_->read<UInt32>();
        header_.labelcount = reader_->read<UInt32>();
        header_.fielddataoffset = reader_->read<UInt32>();
        header_.fielddatacount = reader_->read<UInt32>();
        header_.fieldindexoffset = reader_->read<UInt32>();
        header_.fieldindexcount = reader_->read<UInt32>();
        header_.listindexoffset = reader_->read<UInt32>();
        header_.listindexcount = reader_->read<UInt32>();

        ValidateCanonicalLayout(reader_->data.size());

        rootStruct_ = LoadFileStruct(header_.structoffset);
        isLoaded_ = true;
        isDirty_ = false;
        reader_.reset();
    } catch (...) {
        reader_.reset();
        restorePrevious();
        throw;
    }
}

std::unique_ptr<GffField> GffFile::LoadComplexField(UInt32 type, UInt32 dataOrOffset) {
    auto seekFieldData = [&](std::uint64_t requiredSize, const char* context) {
        requireSectionRange(header_.fielddatacount, dataOrOffset, requiredSize, context);
        reader_->seek(checkedUInt32Add(header_.fielddataoffset, dataOrOffset, "resolving complex field data offset"));
    };

    switch (type) {
    case FIELD_TYPE_DWORD64: {
        seekFieldData(8, "DWORD64 field data");
        auto field = std::make_unique<GffUInt64Field>();
        Fixed8Bytes bytes{};
        reader_->readBytes(reinterpret_cast<char*>(bytes.data()), bytes.size());
        field->value = bytesToUInt64LE(bytes);
        return field;
    }
    case FIELD_TYPE_INT64: {
        seekFieldData(8, "INT64 field data");
        auto field = std::make_unique<GffInt64Field>();
        field->value = reader_->read<std::int64_t>();
        return field;
    }
    case FIELD_TYPE_DOUBLE: {
        seekFieldData(8, "DOUBLE field data");
        auto field = std::make_unique<GffDoubleField>();
        field->value = reader_->read<double>();
        return field;
    }
    case FIELD_TYPE_CEXOSTRING: {
        seekFieldData(4, "CExoString field header");
        auto field = std::make_unique<GffExoStringField>();
        field->size = reader_->read<UInt32>();
        requireSectionRange(header_.fielddatacount, dataOrOffset, checkedUInt64Add(4, field->size, "CExoString field data"), "CExoString field data");
        field->textData.resize(field->size);
        if (field->size > 0) {
            reader_->readBytes(field->textData.data(), field->size);
        }
        return field;
    }
    case FIELD_TYPE_RESREF: {
        seekFieldData(1, "CResRef field header");
        auto field = std::make_unique<GffResRefField>();
        field->size = reader_->read<std::uint8_t>();
        requireSectionRange(header_.fielddatacount, dataOrOffset, checkedUInt64Add(1, field->size, "CResRef field data"), "CResRef field data");
        field->textData.resize(field->size);
        if (field->size > 0) {
            reader_->readBytes(field->textData.data(), field->size);
        }
        return field;
    }
    case FIELD_TYPE_CEXOLOCSTRING: {
        seekFieldData(12, "CExoLocString field header");
        auto field = std::make_unique<GffLocalizedStringField>();
        field->bytesize = reader_->read<UInt32>();
        if (field->bytesize < 8) {
            throw GffError("Error loading CExoLocString field, byte size is too small!");
        }
        requireSectionRange(header_.fielddatacount, dataOrOffset, checkedUInt64Add(4, field->bytesize, "CExoLocString field data"), "CExoLocString field data");
        field->strref = reader_->read<UInt32>();
        field->stringcount = reader_->read<UInt32>();
        field->substrings.clear();

        std::uint64_t consumed = 8; // strref + string count, excluding the leading bytesize field.
        for (UInt32 i = 0; i < field->stringcount; ++i) {
            if (checkedUInt64Add(consumed, 8, "CExoLocString substring header") > field->bytesize) {
                throw GffError("Error loading CExoLocString substring, data extends past the field payload!");
            }
            GffLocalizedSubstring sub;
            sub.stringid = reader_->read<std::int32_t>();
            sub.stringlength = reader_->read<std::int32_t>();
            if (sub.stringlength < 0) {
                throw GffError("Error loading CExoLocString substring, negative string length encountered!");
            }
            consumed = checkedUInt64Add(consumed, 8, "CExoLocString substring header");
            consumed = checkedUInt64Add(consumed, static_cast<std::uint64_t>(sub.stringlength), "CExoLocString substring text");
            if (consumed > field->bytesize) {
                throw GffError("Error loading CExoLocString substring, data extends past the field payload!");
            }
            if (sub.stringlength > 0) {
                sub.textData.resize(static_cast<std::size_t>(sub.stringlength));
                reader_->readBytes(sub.textData.data(), static_cast<std::size_t>(sub.stringlength));
            }
            field->substrings.push_back(std::move(sub));
        }
        return field;
    }
    case FIELD_TYPE_VOID: {
        seekFieldData(4, "VOID field header");
        auto field = std::make_unique<GffVoidField>();
        field->bytesize = reader_->read<UInt32>();
        requireSectionRange(header_.fielddatacount, dataOrOffset, checkedUInt64Add(4, field->bytesize, "VOID field data"), "VOID field data");
        field->data.resize(field->bytesize);
        if (field->bytesize > 0) {
            reader_->readBytes(reinterpret_cast<char*>(field->data.data()), field->bytesize);
        }
        return field;
    }
    case FIELD_TYPE_ORIENTATION: {
        seekFieldData(16, "Orientation field data");
        auto field = std::make_unique<GffOrientationField>();
        for (float& value : field->value) {
            value = reader_->read<float>();
        }
        return field;
    }
    case FIELD_TYPE_POSITION: {
        seekFieldData(12, "Position field data");
        auto field = std::make_unique<GffPositionField>();
        for (float& value : field->value) {
            value = reader_->read<float>();
        }
        return field;
    }
    case FIELD_TYPE_JADE_STRREF: {
        seekFieldData(8, "Jade string reference field data");
        auto field = std::make_unique<GffJadeStringRefField>();
        field->stringType = reader_->read<UInt32>();
        field->strref = reader_->read<UInt32>();
        return field;
    }
    default:
        throw GffError("Invalid field type encountered when reading field " + std::to_string(type) + " data!");
    }
}

std::unique_ptr<GffField> GffFile::LoadFileField(UInt32 offset) {
    reader_->seek(offset);
    const UInt32 type = reader_->read<UInt32>();
    const UInt32 labelIndex = reader_->read<UInt32>();

    std::unique_ptr<GffField> field;

    switch (type) {
    case FIELD_TYPE_BYTE: {
        auto f = std::make_unique<GffByteField>();
        f->value = reader_->read<std::uint8_t>();
        field = std::move(f);
        break;
    }
    case FIELD_TYPE_CHAR: {
        auto f = std::make_unique<GffCharField>();
        f->value = reader_->read<char>();
        field = std::move(f);
        break;
    }
    case FIELD_TYPE_WORD: {
        auto f = std::make_unique<GffWordField>();
        f->value = reader_->read<std::uint16_t>();
        field = std::move(f);
        break;
    }
    case FIELD_TYPE_SHORT: {
        auto f = std::make_unique<GffShortField>();
        f->value = reader_->read<std::int16_t>();
        field = std::move(f);
        break;
    }
    case FIELD_TYPE_DWORD: {
        auto f = std::make_unique<GffUInt32Field>();
        f->value = reader_->read<UInt32>();
        field = std::move(f);
        break;
    }
    case FIELD_TYPE_INT: {
        auto f = std::make_unique<GffIntField>();
        f->value = reader_->read<std::int32_t>();
        field = std::move(f);
        break;
    }
    case FIELD_TYPE_FLOAT: {
        auto f = std::make_unique<GffFloatField>();
        f->value = reader_->read<float>();
        field = std::move(f);
        break;
    }
    case FIELD_TYPE_DWORD64:
    case FIELD_TYPE_INT64:
    case FIELD_TYPE_DOUBLE:
    case FIELD_TYPE_CEXOSTRING:
    case FIELD_TYPE_RESREF:
    case FIELD_TYPE_CEXOLOCSTRING:
    case FIELD_TYPE_VOID:
    case FIELD_TYPE_ORIENTATION:
    case FIELD_TYPE_POSITION:
    case FIELD_TYPE_JADE_STRREF: {
        const UInt32 dataOrOffset = reader_->read<UInt32>();
        field = LoadComplexField(type, dataOrOffset);
        break;
    }
    case FIELD_TYPE_STRUCT: {
        const UInt32 structIndex = reader_->read<UInt32>();
        field = LoadFileStruct(checkedTableOffset(header_.structoffset, structIndex, 12u, header_.structcount, "nested STRUCT"));
        break;
    }
    case FIELD_TYPE_LIST: {
        const UInt32 dataOrOffset = reader_->read<UInt32>();
        requireSectionRange(header_.listindexcount, dataOrOffset, 4, "LIST index header");
        const UInt32 listOffset = checkedUInt32Add(header_.listindexoffset, dataOrOffset, "resolving LIST index offset");
        reader_->seek(listOffset);
        const UInt32 listCount = reader_->read<UInt32>();
        const std::uint64_t bytesNeeded = checkedUInt64Add(4, checkedUInt64Mul(listCount, 4, "LIST item index data"), "LIST item index data");
        requireSectionRange(header_.listindexcount, dataOrOffset, bytesNeeded, "LIST item index data");
        auto list = std::make_unique<GffList>();
        for (UInt32 i = 0; i < listCount; ++i) {
            const UInt32 itemOffset = checkedUInt32Add(listOffset, checkedUInt32Add(4, checkedUInt32Mul(i, 4u, "resolving LIST item index offset"), "resolving LIST item index offset"), "resolving LIST item index offset");
            reader_->seek(itemOffset);
            const UInt32 structIndex = reader_->read<UInt32>();
            list->AddStruct(LoadFileStruct(checkedTableOffset(header_.structoffset, structIndex, 12u, header_.structcount, "LIST STRUCT")));
        }
        field = std::move(list);
        break;
    }
    default:
        throw GffError("Invalid GFF field type " + std::to_string(type) + " encountered while loading.");
    }

    if (!field) {
        throw GffError("Invalid null GFF field encountered while loading.");
    }
    if (labelIndex >= header_.labelcount) {
        throw GffError("GFF field label index points outside the label table.");
    }

    Fixed16Chars label{};
    reader_->seek(checkedTableOffset(header_.labeloffset, labelIndex, 16u, header_.labelcount, "label"));
    reader_->readBytes(label.data(), label.size());
    field->SetLabelRaw(label);
    field->fieldtype = type;
    return field;
}

std::unique_ptr<GffStruct> GffFile::LoadFileStruct(UInt32 offset) {
    reader_->seek(offset);
    const UInt32 type = reader_->read<UInt32>();
    const UInt32 dataOrOffset = reader_->read<UInt32>();
    const UInt32 fieldCount = reader_->read<UInt32>();

    auto structure = std::make_unique<GffStruct>();
    structure->typeid_ = type;

    if (fieldCount == 1) {
        structure->AddField(LoadFileField(checkedTableOffset(header_.fieldoffset, dataOrOffset, 12u, header_.fieldcount, "single struct field")));
    } else if (fieldCount > 1) {
        const std::uint64_t bytesNeeded = checkedUInt64Mul(fieldCount, 4, "struct field indexes");
        requireSectionRange(header_.fieldindexcount, dataOrOffset, bytesNeeded, "struct field indexes");
        for (UInt32 i = 0; i < fieldCount; ++i) {
            const UInt32 fieldIndexOffset = checkedUInt32Add(checkedUInt32Add(header_.fieldindexoffset, dataOrOffset, "resolving field index offset"), checkedUInt32Mul(i, 4u, "resolving field index offset"), "resolving field index offset");
            reader_->seek(fieldIndexOffset);
            const UInt32 fieldIndex = reader_->read<UInt32>();
            structure->AddField(LoadFileField(checkedTableOffset(header_.fieldoffset, fieldIndex, 12u, header_.fieldcount, "struct field")));
        }
    }

    structure->fieldtype = FIELD_TYPE_STRUCT;
    return structure;
}

void GffFile::LoadGff4File(const std::filesystem::path& filename) {
    ResetAll();
    const std::string raw = ReadRegularFileBytes(filename);
    std::vector<std::uint8_t> data(raw.begin(), raw.end());
    if (data.size() < 28u) {
        throw GffError("Specified file is not a valid GFF V4 file.");
    }
    const std::string magic(reinterpret_cast<const char*>(data.data()), 8u);
    if (magic != "GFF V4.0" && magic != "GFF V4.1") {
        throw GffError("Specified file is not a valid GFF V4.0/V4.1 file.");
    }
    const std::string platform(reinterpret_cast<const char*>(data.data() + 8u), 4u);
    if (platform != "PC  ") {
        throw GffError("Only little-endian PC GFF V4 files are supported.");
    }
    std::copy_n(reinterpret_cast<const char*>(data.data() + 8u), 4u, gff4PlatformRaw_.begin());
    std::copy_n(reinterpret_cast<const char*>(data.data() + 12u), 4u, filetypeRaw_.begin());
    std::copy_n(reinterpret_cast<const char*>(data.data() + 16u), 4u, fileversionRaw_.begin());

    const bool isV41 = magic == "GFF V4.1";
    const UInt32 structCount = readU32LEAt(data, 20u, "GFF V4 struct count");
    if (structCount == 0u || structCount > 100000u) {
        throw GffError("Malformed GFF V4 file: unreasonable struct-template count.");
    }

    std::size_t structArrayOffset = 28u;
    if (isV41) {
        if (data.size() < 36u) throw GffError("Malformed GFF V4.1 header.");
        const UInt32 stringCount = readU32LEAt(data, 24u, "GFF V4.1 global string count");
        const UInt32 stringOffset = readU32LEAt(data, 28u, "GFF V4.1 global string offset");
        gff4DataOffset_ = readU32LEAt(data, 32u, "GFF V4.1 data offset");
        if (stringCount != 0u || stringOffset != 0xFFFFFFFFu) {
            throw GffError("GFF V4.1 global string tables are not supported in this shared GFF pass.");
        }
        structArrayOffset = 36u;
    } else {
        gff4DataOffset_ = readU32LEAt(data, 24u, "GFF V4 data offset");
    }
    if (gff4DataOffset_ >= data.size()) {
        throw GffError("Malformed GFF V4 file: data block offset points outside the file.");
    }

    const std::uint64_t structBytes = static_cast<std::uint64_t>(structCount) * 16u;
    ensureGff4Range(data, structArrayOffset, structBytes, "struct-template table");
    gff4Templates_.clear();
    gff4Templates_.reserve(structCount);
    for (UInt32 i = 0; i < structCount; ++i) {
        const std::size_t off = structArrayOffset + static_cast<std::size_t>(i) * 16u;
        Gff4StructTemplate tmpl;
        tmpl.typeidValue = readU32LEAt(data, off, "GFF V4 struct type");
        const UInt32 fieldCount = readU32LEAt(data, off + 4u, "GFF V4 struct field count");
        const UInt32 fieldOffset = readU32LEAt(data, off + 8u, "GFF V4 struct field offset");
        tmpl.size = readU32LEAt(data, off + 12u, "GFF V4 struct size");
        if (fieldCount > 100000u) {
            throw GffError("Malformed GFF V4 file: unreasonable field count in struct-template.");
        }
        ensureGff4Range(data, fieldOffset, static_cast<std::uint64_t>(fieldCount) * 12u, "field-template table");
        tmpl.fields.reserve(fieldCount);
        for (UInt32 f = 0; f < fieldCount; ++f) {
            const std::size_t fieldPos = static_cast<std::size_t>(fieldOffset) + static_cast<std::size_t>(f) * 12u;
            Gff4FieldTemplate field;
            field.label = readU32LEAt(data, fieldPos, "GFF V4 field label");
            const UInt32 typeAndFlags = readU32LEAt(data, fieldPos + 4u, "GFF V4 field type/flags");
            field.type = static_cast<std::uint16_t>(typeAndFlags & 0xFFFFu);
            field.flags = static_cast<std::uint16_t>((typeAndFlags >> 16u) & 0xFFFFu);
            field.offset = readU32LEAt(data, fieldPos + 8u, "GFF V4 field offset");
            tmpl.fields.push_back(field);
        }
        gff4Templates_.push_back(std::move(tmpl));
    }

    reader_ = std::make_unique<BinaryReader>(filename);
    rootStruct_ = LoadGff4Struct(0u, gff4DataOffset_, "");
    reader_.reset();
    filename_ = filename;
    isGff4_ = true;
    isLoaded_ = true;
    isDirty_ = false;
}

std::unique_ptr<GffStruct> GffFile::LoadGff4Struct(UInt32 templateIndex, UInt32 dataOffset, const std::string& label) {
    if (templateIndex >= gff4Templates_.size()) {
        throw GffError("Malformed GFF V4 file: struct field references an invalid struct-template.");
    }
    const auto& tmpl = gff4Templates_[templateIndex];
    const std::uint64_t structEnd = static_cast<std::uint64_t>(dataOffset) + tmpl.size;
    if (structEnd > reader_->data.size()) {
        throw GffError("Malformed GFF V4 file: struct instance extends outside the file.");
    }
    auto structure = std::make_unique<GffStruct>(label);
    structure->typeid_ = tmpl.typeidValue;
    structure->gff4TemplateIndex = templateIndex;
    structure->fieldtype = FIELD_TYPE_STRUCT;
    std::unordered_map<std::string, UInt32> labelCounts;
    for (const auto& fieldTemplate : tmpl.fields) {
        auto field = LoadGff4Field(fieldTemplate, dataOffset);
        const std::string baseLabel = field->GetLabel();
        UInt32& seen = labelCounts[baseLabel];
        ++seen;
        if (seen > 1u) {
            field->SetLabel(baseLabel + "#" + std::to_string(seen));
        }
        structure->AddField(std::move(field));
    }
    return structure;
}

std::unique_ptr<GffField> GffFile::LoadGff4Field(const Gff4FieldTemplate& fieldTemplate, UInt32 structDataOffset) {
    const UInt32 fieldDataOffset = checkedUInt32Add(structDataOffset, fieldTemplate.offset, "resolving GFF V4 field data offset");
    ensureGff4Range(reader_->data, fieldDataOffset, gff4PrimitiveStorageSize(fieldTemplate.type), "field data");

    const bool isList = (fieldTemplate.flags & 0x8000u) != 0u;
    const bool isStruct = (fieldTemplate.flags & 0x4000u) != 0u;
    const bool isReference = (fieldTemplate.flags & 0x2000u) != 0u;
    const std::string label = gff4LabelToString(fieldTemplate.label);

    std::unique_ptr<GffField> out;
    if (isList) {
        auto list = std::make_unique<GffList>(label);
        list->gff4BaseType = fieldTemplate.type;
        list->gff4Flags = fieldTemplate.flags;
        const UInt32 relativeListOffset = readU32LEAt(reader_->data, fieldDataOffset, "GFF V4 list reference");
        if (relativeListOffset != 0xFFFFFFFFu) {
            const UInt32 listOffset = checkedUInt32Add(gff4DataOffset_, relativeListOffset, "resolving GFF V4 list offset");
            ensureGff4Range(reader_->data, listOffset, 4u, "list header");
            const UInt32 count = readU32LEAt(reader_->data, listOffset, "GFF V4 list count");
            if (isStruct && count > 1000000u) throw GffError("Malformed GFF V4 file: unreasonable struct-list count.");
            if (isStruct) {
                if (fieldTemplate.type >= gff4Templates_.size()) throw GffError("Malformed GFF V4 file: list references invalid struct-template.");
                const auto& itemTemplate = gff4Templates_[fieldTemplate.type];
                ensureGff4Range(reader_->data, static_cast<std::uint64_t>(listOffset) + 4u, static_cast<std::uint64_t>(count) * itemTemplate.size, "struct-list items");
                for (UInt32 i = 0; i < count; ++i) {
                    const UInt32 itemOffset = checkedUInt32Add(listOffset, checkedUInt32Add(4u, checkedUInt32Mul(i, itemTemplate.size, "resolving GFF V4 list item"), "resolving GFF V4 list item"), "resolving GFF V4 list item");
                    list->AddStruct(LoadGff4Struct(fieldTemplate.type, itemOffset, ""));
                }
            } else {
                const UInt32 itemSize = gff4PrimitiveStorageSize(fieldTemplate.type);
                const std::uint64_t itemBytes64 = checkedUInt64Mul(count, itemSize, "checking GFF V4 primitive-list items");
                ensureGff4Range(reader_->data, static_cast<std::uint64_t>(listOffset) + 4u, itemBytes64, "primitive-list items");
                if (shouldCompactGff4PrimitiveList(count, itemSize)) {
                    const auto begin = reader_->data.begin() + static_cast<std::ptrdiff_t>(listOffset + 4u);
                    const auto end = begin + static_cast<std::ptrdiff_t>(itemBytes64);
                    list->gff4CompactPrimitiveList = true;
                    list->gff4PrimitiveListCount = count;
                    list->gff4PrimitiveItemSize = itemSize;
                    list->gff4PrimitiveListData.assign(begin, end);
                } else {
                    for (UInt32 i = 0; i < count; ++i) {
                        auto itemStruct = std::make_unique<GffStruct>("");
                        itemStruct->typeid_ = 0xFFFFFFFFu;
                        Gff4FieldTemplate synthetic;
                        synthetic.label = 0u;
                        synthetic.type = fieldTemplate.type;
                        synthetic.flags = fieldTemplate.flags & static_cast<std::uint16_t>(~0x8000u);
                        synthetic.offset = 0u;
                        const UInt32 itemOffset = checkedUInt32Add(listOffset, checkedUInt32Add(4u, checkedUInt32Mul(i, itemSize, "resolving GFF V4 primitive list item"), "resolving GFF V4 primitive list item"), "resolving GFF V4 primitive list item");
                        auto valueField = LoadGff4Field(synthetic, itemOffset);
                        valueField->SetLabel("value");
                        itemStruct->AddField(std::move(valueField));
                        list->AddStruct(std::move(itemStruct));
                    }
                }
            }
        }
        out = std::move(list);
    } else if (isStruct) {
        UInt32 nestedOffset = fieldDataOffset;
        if (isReference) {
            const UInt32 relative = readU32LEAt(reader_->data, fieldDataOffset, "GFF V4 struct reference");
            if (relative == 0xFFFFFFFFu) {
                auto structure = std::make_unique<GffStruct>(label);
                structure->typeid_ = 0xFFFFFFFFu;
                out = std::move(structure);
            } else {
                nestedOffset = checkedUInt32Add(gff4DataOffset_, relative, "resolving GFF V4 referenced struct");
                out = LoadGff4Struct(fieldTemplate.type, nestedOffset, label);
            }
        } else {
            out = LoadGff4Struct(fieldTemplate.type, nestedOffset, label);
        }
    } else {
        switch (fieldTemplate.type) {
        case 0: {
            auto f = std::make_unique<GffByteField>(label, reader_->data[fieldDataOffset]);
            out = std::move(f);
            break;
        }
        case 1: {
            auto f = std::make_unique<GffCharField>(label, static_cast<char>(reader_->data[fieldDataOffset]));
            out = std::move(f);
            break;
        }
        case 2: {
            auto f = std::make_unique<GffWordField>(label, readU16LEAt(reader_->data, fieldDataOffset, "GFF V4 UINT16"));
            out = std::move(f);
            break;
        }
        case 3: {
            auto f = std::make_unique<GffShortField>(label, static_cast<std::int16_t>(readU16LEAt(reader_->data, fieldDataOffset, "GFF V4 INT16")));
            out = std::move(f);
            break;
        }
        case 4: {
            auto f = std::make_unique<GffUInt32Field>(label, readU32LEAt(reader_->data, fieldDataOffset, "GFF V4 UINT32"));
            out = std::move(f);
            break;
        }
        case 5: {
            auto f = std::make_unique<GffIntField>(label, static_cast<std::int32_t>(readU32LEAt(reader_->data, fieldDataOffset, "GFF V4 INT32")));
            out = std::move(f);
            break;
        }
        case 6: {
            auto f = std::make_unique<GffUInt64Field>(label, readU64LEAt(reader_->data, fieldDataOffset, "GFF V4 UINT64"));
            out = std::move(f);
            break;
        }
        case 7: {
            auto f = std::make_unique<GffInt64Field>(label, static_cast<std::int64_t>(readU64LEAt(reader_->data, fieldDataOffset, "GFF V4 INT64")));
            out = std::move(f);
            break;
        }
        case 8: {
            UInt32 bits = readU32LEAt(reader_->data, fieldDataOffset, "GFF V4 FLOAT32");
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            auto f = std::make_unique<GffFloatField>(label, value);
            out = std::move(f);
            break;
        }
        case 9: {
            std::uint64_t bits = readU64LEAt(reader_->data, fieldDataOffset, "GFF V4 FLOAT64");
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            auto f = std::make_unique<GffDoubleField>(label, value);
            out = std::move(f);
            break;
        }
        case 10: {
            auto f = std::make_unique<GffPositionField>(label, 0.0f, 0.0f, 0.0f);
            for (std::size_t i = 0; i < 3u; ++i) {
                UInt32 bits = readU32LEAt(reader_->data, fieldDataOffset + static_cast<UInt32>(i * 4u), "GFF V4 Vector3f");
                std::memcpy(&f->value[i], &bits, sizeof(float));
            }
            out = std::move(f);
            break;
        }
        case 12:
        case 13:
        case 15: {
            auto f = std::make_unique<GffOrientationField>(label, 0.0f, 0.0f, 0.0f, 0.0f);
            for (std::size_t i = 0; i < 4u; ++i) {
                UInt32 bits = readU32LEAt(reader_->data, fieldDataOffset + static_cast<UInt32>(i * 4u), "GFF V4 Vector4f/Quaternionf/Color4f");
                std::memcpy(&f->value[i], &bits, sizeof(float));
            }
            out = std::move(f);
            break;
        }
        case 14: {
            const UInt32 relative = readU32LEAt(reader_->data, fieldDataOffset, "GFF V4 ECString reference");
            auto f = std::make_unique<GffExoStringField>(label, readGff4Utf16String(reader_->data, gff4DataOffset_, relative));
            out = std::move(f);
            break;
        }
        case 16: {
            const auto begin = reader_->data.begin() + static_cast<std::ptrdiff_t>(fieldDataOffset);
            ByteBuffer bytes(begin, begin + 64u);
            auto f = std::make_unique<GffVoidField>(label, bytes);
            out = std::move(f);
            break;
        }
        case 17: {
            const UInt32 strref = readU32LEAt(reader_->data, fieldDataOffset, "GFF V4 TlkString strref");
            const UInt32 stringType = readU32LEAt(reader_->data, fieldDataOffset + 4u, "GFF V4 TlkString type");
            auto f = std::make_unique<GffJadeStringRefField>(label, stringType, strref);
            out = std::move(f);
            break;
        }
        default: {
            const UInt32 size = gff4PrimitiveStorageSize(fieldTemplate.type);
            const auto begin = reader_->data.begin() + static_cast<std::ptrdiff_t>(fieldDataOffset);
            ByteBuffer bytes(begin, begin + static_cast<std::ptrdiff_t>(size));
            auto f = std::make_unique<GffVoidField>(label, bytes);
            out = std::move(f);
            break;
        }
        }
    }

    if (!out) throw GffError("Unable to materialize GFF V4 field.");
    out->hasGff4LabelId = true;
    out->gff4LabelId = fieldTemplate.label;
    out->gff4BaseType = fieldTemplate.type;
    out->gff4Flags = fieldTemplate.flags;
    return out;
}

namespace {

struct FieldPathSelector {
    std::string label;
    std::size_t occurrence = 0;
};

FieldPathSelector fieldPathSelector(const GffStruct& structure, const std::string& token) {
    // Preserve backwards compatibility for literal labels, even when a label
    // happens to end in the occurrence-suffix syntax.
    if (structure.GetFieldByLabel(token) != nullptr) return FieldPathSelector{token, 0u};

    const std::size_t marker = token.rfind("[#");
    if (marker == std::string::npos || token.size() < marker + 4u || token.back() != ']') {
        return FieldPathSelector{token, 0u};
    }
    const std::string ordinalText = token.substr(marker + 2u, token.size() - marker - 3u);
    if (!IsUnsignedDecimal(ordinalText)) return FieldPathSelector{token, 0u};

    UInt32 ordinal = 0;
    try {
        ordinal = ParseUInt32Decimal(ordinalText);
    } catch (const std::exception&) {
        return FieldPathSelector{token, 0u};
    }
    if (ordinal == 0u) return FieldPathSelector{token, 0u};
    return FieldPathSelector{token.substr(0, marker), static_cast<std::size_t>(ordinal - 1u)};
}

GffField* fieldForPathToken(GffStruct& structure, const std::string& token) {
    const FieldPathSelector selector = fieldPathSelector(structure, token);
    return structure.GetFieldByLabel(selector.label, selector.occurrence);
}

const GffField* fieldForPathToken(const GffStruct& structure, const std::string& token) {
    const FieldPathSelector selector = fieldPathSelector(structure, token);
    return structure.GetFieldByLabel(selector.label, selector.occurrence);
}

GffField* parseStructPath(GffStruct* structure, const std::vector<std::string>& tokens, std::size_t tokenIndex, const std::string& fieldPath);

GffField* parseListPath(GffList* list, const std::vector<std::string>& tokens, std::size_t tokenIndex, const std::string& fieldPath) {
    if (tokenIndex >= tokens.size()) {
        return nullptr;
    }
    const std::string& token = tokens[tokenIndex];
    if (!IsUnsignedDecimal(token)) {
        throw GffError("Unable to retrieve struct at path " + fieldPath + ", " + token + " is not a valid list index!");
    }
    UInt32 parsedIndex = 0;
    try {
        parsedIndex = ParseUInt32Decimal(token);
    } catch (const std::exception&) {
        throw GffError("Unable to retrieve struct at path " + fieldPath + ", " + token + " is not a valid list index!");
    }
    const std::size_t index = static_cast<std::size_t>(parsedIndex);
    if (index >= list->count()) {
        return nullptr;
    }
    if (tokenIndex + 1 == tokens.size()) {
        return list->GetStruct(index);
    }
    return parseStructPath(list->GetStruct(index), tokens, tokenIndex + 1, fieldPath);
}

GffField* parseStructPath(GffStruct* structure, const std::vector<std::string>& tokens, std::size_t tokenIndex, const std::string& fieldPath) {
    if (tokenIndex >= tokens.size()) {
        return nullptr;
    }
    if (structure == nullptr) {
        throw GffError("Access violation reading null GFF struct while parsing path " + fieldPath + "!");
    }
    const std::string& token = tokens[tokenIndex];
    GffField* field = fieldForPathToken(*structure, token);
    if (field == nullptr) return nullptr;
    if (tokenIndex + 1 == tokens.size()) return field;
    if (field->fieldtype == FIELD_TYPE_STRUCT) {
        return parseStructPath(&checkedGffCast<GffStruct>(*field), tokens, tokenIndex + 1, fieldPath);
    }
    if (field->fieldtype == FIELD_TYPE_LIST) {
        return parseListPath(&checkedGffCast<GffList>(*field), tokens, tokenIndex + 1, fieldPath);
    }
    throw GffError("Unable to retrieve struct field at path " + fieldPath + ", the field " + field->GetLabel() + " is not a LIST or STRUCT!");
}

const GffField* parseStructPathConst(const GffStruct* structure, const std::vector<std::string>& tokens, std::size_t tokenIndex, const std::string& fieldPath);

const GffField* parseListPathConst(const GffList* list, const std::vector<std::string>& tokens, std::size_t tokenIndex, const std::string& fieldPath) {
    if (tokenIndex >= tokens.size()) {
        return nullptr;
    }
    const std::string& token = tokens[tokenIndex];
    if (!IsUnsignedDecimal(token)) {
        throw GffError("Unable to retrieve struct at path " + fieldPath + ", " + token + " is not a valid list index!");
    }
    UInt32 parsedIndex = 0;
    try {
        parsedIndex = ParseUInt32Decimal(token);
    } catch (const std::exception&) {
        throw GffError("Unable to retrieve struct at path " + fieldPath + ", " + token + " is not a valid list index!");
    }
    const std::size_t index = static_cast<std::size_t>(parsedIndex);
    if (index >= list->count()) {
        return nullptr;
    }
    if (tokenIndex + 1 == tokens.size()) {
        return list->GetStruct(index);
    }
    return parseStructPathConst(list->GetStruct(index), tokens, tokenIndex + 1, fieldPath);
}

const GffField* parseStructPathConst(const GffStruct* structure, const std::vector<std::string>& tokens, std::size_t tokenIndex, const std::string& fieldPath) {
    if (tokenIndex >= tokens.size()) {
        return nullptr;
    }
    if (structure == nullptr) {
        throw GffError("Access violation reading null GFF struct while parsing path " + fieldPath + "!");
    }
    const std::string& token = tokens[tokenIndex];
    const GffField* field = fieldForPathToken(*structure, token);
    if (field == nullptr) return nullptr;
    if (tokenIndex + 1 == tokens.size()) return field;
    if (field->fieldtype == FIELD_TYPE_STRUCT) {
        return parseStructPathConst(&checkedGffCast<GffStruct>(*field), tokens, tokenIndex + 1, fieldPath);
    }
    if (field->fieldtype == FIELD_TYPE_LIST) {
        return parseListPathConst(&checkedGffCast<GffList>(*field), tokens, tokenIndex + 1, fieldPath);
    }
    throw GffError("Unable to retrieve struct field at path " + fieldPath + ", the field " + field->GetLabel() + " is not a LIST or STRUCT!");
}

std::vector<std::string> tokenizePath(const std::string& path) {
    return splitNonEmpty(path, '\\');
}

} // namespace

GffField* GffFile::GetFirstRootField() {
    currField_ = 0;
    if (!rootStruct_) {
        throw GffError("Access violation reading GFF root struct.");
    }
    if (rootStruct_->count() == 0) {
        return nullptr;
    }
    return rootStruct_->GetField(currField_);
}

GffField* GffFile::GetNextRootField() {
    ++currField_;
    if (!rootStruct_) {
        throw GffError("Access violation reading GFF root struct.");
    }
    if (currField_ >= rootStruct_->count()) {
        return nullptr;
    }
    return rootStruct_->GetField(currField_);
}

GffField* GffFile::GetFieldByLabel(const std::string& fieldPath) {
    auto tokens = tokenizePath(fieldPath);
    if (tokens.empty()) {
        throw GffError("Invalid field path " + fieldPath + " encountered! Unable to load specified field.");
    }

    if (!rootStruct_) {
        throw GffError("Access violation reading GFF root struct.");
    }
    return parseStructPath(rootStruct_.get(), tokens, 0, fieldPath);
}

const GffField* GffFile::GetFieldByLabel(const std::string& fieldPath) const {
    auto tokens = tokenizePath(fieldPath);
    if (tokens.empty()) {
        throw GffError("Invalid field path " + fieldPath + " encountered! Unable to load specified field.");
    }
    if (!rootStruct_) {
        throw GffError("Access violation reading GFF root struct.");
    }
    return parseStructPathConst(rootStruct_.get(), tokens, 0, fieldPath);
}

void GffFile::AddField(std::unique_ptr<GffField> field, const std::string& path) {
    GffField* parent = path.empty() ? static_cast<GffField*>(rootStruct_.get()) : GetFieldByLabel(path);
    if (!parent) {
        throw GffError("Unable to add new field at path " + path + " since the specified parent field could not be found.");
    }

    if (parent->fieldtype == FIELD_TYPE_STRUCT) {
        GffStruct* asStruct = dynamic_cast<GffStruct*>(parent);
        if (asStruct == nullptr) {
            throw GffError("Invalid class typecast");
        }
        asStruct->AddField(std::move(field));
        isDirty_ = true;
    } else if (parent->fieldtype == FIELD_TYPE_LIST) {
        if (!field) {
            throw GffError("Access violation adding null field to LIST " + path + "!");
        }
        if (field->fieldtype != FIELD_TYPE_STRUCT) {
            const std::string label = field->GetLabel();
            throw GffError("Unable to add new field to the LIST " + path + " since the field " + label + " is not a STRUCT!");
        }
        GffStruct* asStruct = dynamic_cast<GffStruct*>(field.get());
        if (asStruct == nullptr) {
            throw GffError("Invalid class typecast");
        }
        checkedGffCast<GffList>(*parent).AddStruct(
            std::unique_ptr<GffStruct>(static_cast<GffStruct*>(field.release())));
        isDirty_ = true;
    } else {
        throw GffError("Unable to add new field at path " + path + " since the field " + parent->GetLabel() + " is not a LIST or STRUCT!");
    }
}

void GffFile::AddField(GffField* field, const std::string& path) {
    AddField(std::unique_ptr<GffField>(field), path);
}

void GffFile::DeleteField(const std::string& fieldPath) {
    const auto tokens = splitNonEmpty(fieldPath, '\\');
    if (tokens.empty()) {
        throw GffError("Unable to delete a field from an empty path!");
    }

    std::string field;
    std::string path;
    if (tokens.size() == 1) {
        field = tokens[0];
    } else {
        field = tokens.back();
        path.clear();
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (!path.empty()) path.push_back('\\');
            path += tokens[i];
        }
    }

    GffField* parent = path.empty() ? static_cast<GffField*>(rootStruct_.get()) : GetFieldByLabel(path);
    if (!parent) {
        throw GffError("Unable to delete the field " + field + " since the parent path " + path + " could not be found!");
    }

    if (parent->fieldtype == FIELD_TYPE_STRUCT) {
        GffStruct& parentStruct = checkedGffCast<GffStruct>(*parent);
        const FieldPathSelector selector = fieldPathSelector(parentStruct, field);
        parentStruct.DeleteField(selector.label, selector.occurrence);
        isDirty_ = true;
    } else if (parent->fieldtype == FIELD_TYPE_LIST) {
        if (!IsUnsignedDecimal(field)) {
            throw GffError("Unable to delete the field " + path + "\\" + field + " since no valid parent LIST index was specified!");
        }
        UInt32 parsedIndex = 0;
        try {
            parsedIndex = ParseUInt32Decimal(field);
        } catch (const std::exception&) {
            throw GffError("Unable to delete the field " + path + "\\" + field + " since no valid parent LIST index was specified!");
        }
        checkedGffCast<GffList>(*parent).DeleteStruct(parsedIndex);
        isDirty_ = true;
    }

}

bool GffFile::ChangeFieldValue(std::string path, const std::string& value) {
    std::string index;
    const auto open = path.find('(');
    const auto close = path.find(')');
    if (open != std::string::npos && close != std::string::npos) {
        // Field paths may contain a parenthesized suffix such as Name(strref) or Text(lang0).
        const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(close) -
                                     static_cast<std::ptrdiff_t>(open) + 1;
        if (count > 0) {
            index = path.substr(open, static_cast<std::size_t>(count));
        }
        path = path.substr(0, open);
    }

    GffField* field = GetFieldByLabel(path);
    if (!field) {
        return false;
    }

    try {
        switch (field->fieldtype) {
        case FIELD_TYPE_BYTE:
            if (!IsUnsignedDecimal(value)) return false;
            checkedGffCast<GffByteField>(*field).value = parseUnsignedBounded<std::uint8_t>(value, std::numeric_limits<std::uint8_t>::max(), "BYTE");
            break;
        case FIELD_TYPE_CHAR:
            if (value.empty()) return false;
            checkedGffCast<GffCharField>(*field).value = value[0];
            break;
        case FIELD_TYPE_WORD:
            if (!IsUnsignedDecimal(value)) return false;
            checkedGffCast<GffWordField>(*field).value = parseUnsignedBounded<std::uint16_t>(value, std::numeric_limits<std::uint16_t>::max(), "WORD");
            break;
        case FIELD_TYPE_SHORT:
            if (!IsSignedDecimal(value)) return false;
            checkedGffCast<GffShortField>(*field).value = parseSignedBounded<std::int16_t>(value, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), "SHORT");
            break;
        case FIELD_TYPE_DWORD:
            if (!IsUnsignedDecimal(value)) return false;
            checkedGffCast<GffUInt32Field>(*field).value = ParseUInt32Decimal(value);
            break;
        case FIELD_TYPE_INT:
            if (!IsSignedDecimal(value)) return false;
            checkedGffCast<GffIntField>(*field).value = ParseInt32Decimal(value);
            break;
        case FIELD_TYPE_DWORD64:
            if (!IsUnsignedDecimal(value)) return false;
            checkedGffCast<GffUInt64Field>(*field).value = ParseUInt64Decimal(value);
            break;
        case FIELD_TYPE_INT64:
            if (!IsSignedDecimal(value)) return false;
            checkedGffCast<GffInt64Field>(*field).value = ParseInt64Decimal(value);
            break;
        case FIELD_TYPE_FLOAT:
            if (!IsDecimalNumber(value)) return false;
            checkedGffCast<GffFloatField>(*field).value = ParseFloatDecimal(value);
            break;
        case FIELD_TYPE_DOUBLE:
            if (!IsDecimalNumber(value)) return false;
            checkedGffCast<GffDoubleField>(*field).value = ParseDoubleDecimal(value);
            break;
        case FIELD_TYPE_CEXOSTRING:
            checkedGffCast<GffExoStringField>(*field).SetString(value);
            break;
        case FIELD_TYPE_CEXOLOCSTRING: {
            auto* loc = &checkedGffCast<GffLocalizedStringField>(*field);
            if (index == "(strref)") {
                if (value == "-1") {
                    loc->strref = 0xFFFFFFFFu;
                } else {
                    if (!IsUnsignedDecimal(value)) return false;
                    loc->strref = ParseUInt32Decimal(value);
                }
            } else if (index.rfind("(lang", 0) == 0) {
                const auto end = index.find(')');
                const std::string lang = end == std::string::npos ? std::string{} : index.substr(5, end - 5);
                if (!IsUnsignedDecimal(lang)) return false;
                const auto parsedLang = ParseUInt32Decimal(lang);
                if (parsedLang > static_cast<UInt32>(std::numeric_limits<int>::max())) return false;
                loc->SetStringByID(static_cast<int>(parsedLang), value);
            } else {
                return false;
            }
            break;
        }
        case FIELD_TYPE_RESREF:
            checkedGffCast<GffResRefField>(*field).SetString(value);
            break;
        case FIELD_TYPE_ORIENTATION:
            checkedGffCast<GffOrientationField>(*field).value = ParseGffVector4Text(value);
            break;
        case FIELD_TYPE_POSITION:
            checkedGffCast<GffPositionField>(*field).value = ParseGffVector3Text(value);
            break;
        case FIELD_TYPE_JADE_STRREF: {
            const auto tokens = splitNonEmpty(value, '|');
            if (tokens.size() != 2 || !IsUnsignedDecimal(tokens[0])) return false;
            auto* text = &checkedGffCast<GffJadeStringRefField>(*field);
            text->stringType = ParseUInt32Decimal(tokens[0]);
            if (tokens[1] == "-1") {
                text->strref = 0xFFFFFFFFu;
            } else {
                if (!IsUnsignedDecimal(tokens[1])) return false;
                text->strref = ParseUInt32Decimal(tokens[1]);
            }
            break;
        }
        default:
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }

    isDirty_ = true;
    return true;
}

void GffFile::SaveData::AddLabel(const Fixed16Chars& label) {
    if (std::find(FieldLabels.begin(), FieldLabels.end(), label) != FieldLabels.end()) {
        return;
    }
    FieldLabels.push_back(label);
}

bool GffFile::IsComplexField(const GffField& field) const {
    return field.fieldtype == FIELD_TYPE_DWORD64 ||
           field.fieldtype == FIELD_TYPE_INT64 ||
           field.fieldtype == FIELD_TYPE_DOUBLE ||
           field.fieldtype == FIELD_TYPE_CEXOSTRING ||
           field.fieldtype == FIELD_TYPE_RESREF ||
           field.fieldtype == FIELD_TYPE_CEXOLOCSTRING ||
           field.fieldtype == FIELD_TYPE_VOID ||
           field.fieldtype == FIELD_TYPE_ORIENTATION ||
           field.fieldtype == FIELD_TYPE_POSITION ||
           field.fieldtype == FIELD_TYPE_JADE_STRREF;
}

UInt32 GffFile::GetFieldDataSize(const GffField& field) const {
    switch (field.fieldtype) {
    case FIELD_TYPE_BYTE:
    case FIELD_TYPE_CHAR:
        return 1;
    case FIELD_TYPE_WORD:
    case FIELD_TYPE_SHORT:
        return 2;
    case FIELD_TYPE_DWORD:
    case FIELD_TYPE_INT:
    case FIELD_TYPE_FLOAT:
        return 4;
    case FIELD_TYPE_DWORD64:
    case FIELD_TYPE_INT64:
    case FIELD_TYPE_DOUBLE:
    case FIELD_TYPE_JADE_STRREF:
        return 8;
    case FIELD_TYPE_CEXOSTRING:
        return checkedUInt32Add(4u, checkedGffCast<GffExoStringField>(field).size, "computing CExoString data size");
    case FIELD_TYPE_RESREF:
        return checkedUInt32Add(1u, checkedGffCast<GffResRefField>(field).size, "computing CResRef data size");
    case FIELD_TYPE_CEXOLOCSTRING:
        return checkedUInt32Add(4u, checkedGffCast<GffLocalizedStringField>(field).bytesize, "computing CExoLocString data size");
    case FIELD_TYPE_VOID:
        return checkedUInt32Add(4u, checkedGffCast<GffVoidField>(field).bytesize, "computing CVoid data size");
    case FIELD_TYPE_ORIENTATION:
        return 16;
    case FIELD_TYPE_POSITION:
        return 12;
    default:
        return 0;
    }
}

void GffFile::SaveParseStruct(const GffStruct& structure) {
    for (std::size_t i = 0; i < structure.count(); ++i) {
        const GffField* fieldPtr = structure.GetField(i);
        saveData_.FieldCount = checkedUInt32Add(saveData_.FieldCount, 1u, "counting fields for save");
        if (!fieldPtr) {
            throw GffError("Access violation reading null GFF field while parsing struct " + structure.GetLabel() + " for save!");
        }
        const GffField& field = *fieldPtr;

        saveData_.AddLabel(field.GetLabelRaw());

        if (field.fieldtype == FIELD_TYPE_STRUCT) {
            saveData_.StructCount = checkedUInt32Add(saveData_.StructCount, 1u, "counting structs for save");
            const auto& nested = checkedGffCast<GffStruct>(field);
            if (nested.count() > 1) {
                saveData_.FieldIndexCount = checkedUInt32Add(saveData_.FieldIndexCount, checkedCountToUInt32(nested.count(), "counting nested field indexes"), "counting nested field indexes");
            }
            SaveParseStruct(nested);
        } else if (field.fieldtype == FIELD_TYPE_LIST) {
            saveData_.ListCount = checkedUInt32Add(saveData_.ListCount, 1u, "counting lists for save");
            const auto& list = checkedGffCast<GffList>(field);
            if (list.count() > 0) {
                saveData_.ListIndexCount = checkedUInt32Add(saveData_.ListIndexCount, checkedCountToUInt32(list.count(), "counting list indexes"), "counting list indexes");
            }
            SaveParseList(list);
        } else if (IsComplexField(field)) {
            saveData_.DataBlockSize = checkedUInt32Add(saveData_.DataBlockSize, GetFieldDataSize(field), "counting field data bytes");
        }
    }
}

void GffFile::SaveParseList(const GffList& list) {
    for (std::size_t i = 0; i < list.count(); ++i) {
        const GffStruct* structurePtr = list.GetStruct(i);
        saveData_.StructCount = checkedUInt32Add(saveData_.StructCount, 1u, "counting structs for save");
        if (!structurePtr) {
            throw GffError("Access violation reading null GFF STRUCT while parsing LIST " + list.GetLabel() + " for save!");
        }
        const GffStruct& structure = *structurePtr;
        if (structure.count() > 1) {
            saveData_.FieldIndexCount = checkedUInt32Add(saveData_.FieldIndexCount, checkedCountToUInt32(structure.count(), "counting field indexes"), "counting field indexes");
        }
        SaveParseStruct(structure);
    }
}

UInt32 GffFile::SaveGetLabelIndex(const Fixed16Chars& label) const {
    for (std::size_t i = 0; i < saveData_.FieldLabels.size(); ++i) {
        if (saveData_.FieldLabels[i] == label) {
            return static_cast<UInt32>(i);
        }
    }
    return 0;
}

void GffFile::SaveProcessLabels() {
    writer_->seek(header_.labeloffset);
    for (const auto& label : saveData_.FieldLabels) {
        writer_->writeBytes(label.data(), label.size());
    }
}

void GffFile::SaveGff4File(const std::filesystem::path& outFilename) {
    const std::filesystem::path target = ResolveOutputTarget(outFilename.empty() ? filename_ : outFilename);
    if (!rootStruct_) throw GffError("Unable to save GFF V4: missing root struct.");
    if (gff4Templates_.empty()) throw GffError("Unable to save GFF V4: missing preserved struct templates.");

    auto findTemplateField = [&](UInt32 templateIndex, UInt32 label) -> const Gff4FieldTemplate* {
        if (templateIndex >= gff4Templates_.size()) return nullptr;
        for (const auto& f : gff4Templates_[templateIndex].fields) {
            if (f.label == label) return &f;
        }
        return nullptr;
    };

    auto fieldLabelId = [](const GffField& field) -> UInt32 {
        if (field.hasGff4LabelId) return field.gff4LabelId;
        return gff4LabelFromString(field.GetLabel(), 0xFFFFFFFFu);
    };

    std::function<void(const GffStruct&, UInt32)> validateStruct = [&](const GffStruct& structure, UInt32 templateIndex) {
        if (templateIndex >= gff4Templates_.size()) throw GffError("GFF V4 structural validation failed: invalid struct template.");
        for (std::size_t i = 0; i < structure.count(); ++i) {
            const GffField* field = structure.GetField(i);
            if (!field) throw GffError("GFF V4 structural validation failed: null field.");
            const UInt32 labelId = fieldLabelId(*field);
            const Gff4FieldTemplate* tmpl = findTemplateField(templateIndex, labelId);
            if (!tmpl) {
                throw GffError("GFF V4 structural add/delete/rename is not supported by the shared GFF core yet; save scalar edits only or use Neo2DA for GDA tables.");
            }
            const bool isList = (tmpl->flags & 0x8000u) != 0u;
            const bool isStruct = (tmpl->flags & 0x4000u) != 0u;
            if (isList) {
                if (field->fieldtype != FIELD_TYPE_LIST) throw GffError("GFF V4 structural validation failed: list field type changed.");
                if (isStruct && tmpl->type < gff4Templates_.size()) {
                    const auto& list = checkedGffCast<GffList>(*field);
                    for (std::size_t n = 0; n < list.count(); ++n) {
                        const GffStruct* child = list.GetStruct(n);
                        if (child) validateStruct(*child, tmpl->type);
                    }
                }
            } else if (isStruct) {
                if (field->fieldtype != FIELD_TYPE_STRUCT) throw GffError("GFF V4 structural validation failed: struct field type changed.");
                validateStruct(checkedGffCast<GffStruct>(*field), tmpl->type);
            }
        }
    };
    validateStruct(*rootStruct_, 0u);

    std::vector<std::uint8_t> dataBlock;
    auto ensureData = [&](UInt32 offset, UInt32 size) {
        const std::uint64_t end = static_cast<std::uint64_t>(offset) + size;
        if (end > std::numeric_limits<UInt32>::max()) throw GffError("GFF V4 data block exceeds 32-bit limits.");
        if (dataBlock.size() < end) dataBlock.resize(static_cast<std::size_t>(end), 0xFFu);
    };

    auto appendPadding = [&]() {
        while ((dataBlock.size() % 4u) != 0u) dataBlock.push_back(0u);
    };

    auto appendGff4String = [&](const std::string& text) -> UInt32 {
        appendPadding();
        const UInt32 offset = checkedVectorOffset(dataBlock.size(), "appending ECString");
        const auto units = utf8ToUtf16Units(text);
        appendU32LE(dataBlock, checkedCountToUInt32(units.size(), "writing ECString length"));
        for (std::uint16_t unit : units) {
            dataBlock.push_back(static_cast<std::uint8_t>(unit & 0xFFu));
            dataBlock.push_back(static_cast<std::uint8_t>((unit >> 8u) & 0xFFu));
        }
        return offset;
    };

    auto getFieldByGff4Template = [&](const GffStruct& structure, UInt32 label, std::size_t templateFieldIndex) -> const GffField* {
        if (templateFieldIndex < structure.count()) {
            const GffField* positional = structure.GetField(templateFieldIndex);
            if (positional && fieldLabelId(*positional) == label) return positional;
        }
        for (std::size_t i = 0; i < structure.count(); ++i) {
            const GffField* field = structure.GetField(i);
            if (!field) continue;
            if (fieldLabelId(*field) == label) return field;
        }
        return nullptr;
    };

    auto asString = [](const GffField& field) -> std::string {
        if (const auto* f = dynamic_cast<const GffExoStringField*>(&field)) return f->GetString();
        if (const auto* f = dynamic_cast<const GffResRefField*>(&field)) return f->GetString();
        return field.GetString();
    };

    auto writeScalar = [&](const GffField& field, const Gff4FieldTemplate& tmpl, UInt32 absoluteDataOffset) {
        ensureData(absoluteDataOffset, gff4PrimitiveStorageSize(tmpl.type));
        switch (tmpl.type) {
        case 0: {
            std::uint8_t value = 0;
            if (const auto* f = dynamic_cast<const GffByteField*>(&field)) value = f->value;
            else value = static_cast<std::uint8_t>(ParseUInt32Decimal(field.GetString()) & 0xFFu);
            dataBlock[absoluteDataOffset] = value;
            break;
        }
        case 1: {
            char value = '\0';
            if (const auto* f = dynamic_cast<const GffCharField*>(&field)) value = f->value;
            else if (!field.GetString().empty()) value = field.GetString()[0];
            dataBlock[absoluteDataOffset] = static_cast<std::uint8_t>(value);
            break;
        }
        case 2: {
            std::uint16_t value = 0;
            if (const auto* f = dynamic_cast<const GffWordField*>(&field)) value = f->value;
            else value = static_cast<std::uint16_t>(ParseUInt32Decimal(field.GetString()) & 0xFFFFu);
            dataBlock[absoluteDataOffset] = static_cast<std::uint8_t>(value & 0xFFu);
            dataBlock[absoluteDataOffset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
            break;
        }
        case 3: {
            std::int16_t value = 0;
            if (const auto* f = dynamic_cast<const GffShortField*>(&field)) value = f->value;
            else value = static_cast<std::int16_t>(ParseInt32Decimal(field.GetString()));
            const auto raw = static_cast<std::uint16_t>(value);
            dataBlock[absoluteDataOffset] = static_cast<std::uint8_t>(raw & 0xFFu);
            dataBlock[absoluteDataOffset + 1u] = static_cast<std::uint8_t>((raw >> 8u) & 0xFFu);
            break;
        }
        case 4: {
            UInt32 value = 0;
            if (const auto* f = dynamic_cast<const GffUInt32Field*>(&field)) value = f->value;
            else value = ParseUInt32Decimal(field.GetString());
            writeU32LETo(dataBlock, absoluteDataOffset, value);
            break;
        }
        case 5: {
            std::int32_t value = 0;
            if (const auto* f = dynamic_cast<const GffIntField*>(&field)) value = f->value;
            else value = ParseInt32Decimal(field.GetString());
            writeU32LETo(dataBlock, absoluteDataOffset, static_cast<UInt32>(value));
            break;
        }
        case 6: {
            std::uint64_t value = 0;
            if (const auto* f = dynamic_cast<const GffUInt64Field*>(&field)) value = f->value;
            else value = ParseUInt64Decimal(field.GetString());
            for (std::size_t i = 0; i < 8u; ++i) dataBlock[absoluteDataOffset + i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xFFu);
            break;
        }
        case 7: {
            std::int64_t value = 0;
            if (const auto* f = dynamic_cast<const GffInt64Field*>(&field)) value = f->value;
            else value = ParseInt64Decimal(field.GetString());
            const auto raw = static_cast<std::uint64_t>(value);
            for (std::size_t i = 0; i < 8u; ++i) dataBlock[absoluteDataOffset + i] = static_cast<std::uint8_t>((raw >> (i * 8u)) & 0xFFu);
            break;
        }
        case 8: {
            float value = 0.0f;
            if (const auto* f = dynamic_cast<const GffFloatField*>(&field)) value = f->value;
            else value = ParseFloatDecimal(field.GetString());
            UInt32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            writeU32LETo(dataBlock, absoluteDataOffset, bits);
            break;
        }
        case 9: {
            double value = 0.0;
            if (const auto* f = dynamic_cast<const GffDoubleField*>(&field)) value = f->value;
            else value = ParseDoubleDecimal(field.GetString());
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            for (std::size_t i = 0; i < 8u; ++i) dataBlock[absoluteDataOffset + i] = static_cast<std::uint8_t>((bits >> (i * 8u)) & 0xFFu);
            break;
        }
        case 10: {
            const auto& f = checkedGffCast<GffPositionField>(field);
            for (std::size_t i = 0; i < 3u; ++i) {
                UInt32 bits = 0;
                std::memcpy(&bits, &f.value[i], sizeof(bits));
                writeU32LETo(dataBlock, absoluteDataOffset + static_cast<UInt32>(i * 4u), bits);
            }
            break;
        }
        case 12:
        case 13:
        case 15: {
            const auto& f = checkedGffCast<GffOrientationField>(field);
            for (std::size_t i = 0; i < 4u; ++i) {
                UInt32 bits = 0;
                std::memcpy(&bits, &f.value[i], sizeof(bits));
                writeU32LETo(dataBlock, absoluteDataOffset + static_cast<UInt32>(i * 4u), bits);
            }
            break;
        }
        case 14: {
            const UInt32 relative = appendGff4String(asString(field));
            writeU32LETo(dataBlock, absoluteDataOffset, relative);
            break;
        }
        case 16: {
            const auto& f = checkedGffCast<GffVoidField>(field);
            const std::size_t n = std::min<std::size_t>(64u, f.data.size());
            if (n > 0) writeBytesTo(dataBlock, absoluteDataOffset, f.data.data(), n);
            break;
        }
        case 17: {
            const auto& f = checkedGffCast<GffJadeStringRefField>(field);
            writeU32LETo(dataBlock, absoluteDataOffset, f.strref);
            writeU32LETo(dataBlock, absoluteDataOffset + 4u, f.stringType);
            break;
        }
        default: {
            const auto& f = checkedGffCast<GffVoidField>(field);
            const UInt32 n = std::min<UInt32>(gff4PrimitiveStorageSize(tmpl.type), f.bytesize);
            if (n > 0) writeBytesTo(dataBlock, absoluteDataOffset, f.data.data(), n);
            break;
        }
        }
    };

    std::function<void(const GffStruct&, UInt32, UInt32)> writeStruct;
    writeStruct = [&](const GffStruct& structure, UInt32 templateIndex, UInt32 dataOffset) {
        if (templateIndex >= gff4Templates_.size()) throw GffError("GFF V4 writer invalid struct template.");
        const auto& tmpl = gff4Templates_[templateIndex];
        ensureData(dataOffset, tmpl.size);
        for (UInt32 i = 0; i < tmpl.size; ++i) dataBlock[dataOffset + i] = 0xFFu;
        for (std::size_t fieldIndex = 0; fieldIndex < tmpl.fields.size(); ++fieldIndex) {
            const auto& fieldTemplate = tmpl.fields[fieldIndex];
            const GffField* field = getFieldByGff4Template(structure, fieldTemplate.label, fieldIndex);
            const UInt32 fieldData = checkedUInt32Add(dataOffset, fieldTemplate.offset, "writing GFF V4 field");
            const bool isList = (fieldTemplate.flags & 0x8000u) != 0u;
            const bool isStruct = (fieldTemplate.flags & 0x4000u) != 0u;
            const bool isReference = (fieldTemplate.flags & 0x2000u) != 0u;
            ensureData(fieldData, 4u);
            if (!field) {
                if (isList || isReference || fieldTemplate.type == 14u) writeU32LETo(dataBlock, fieldData, 0xFFFFFFFFu);
                continue;
            }
            if (isList) {
                const auto& list = checkedGffCast<GffList>(*field);
                appendPadding();
                const UInt32 listRelative = checkedVectorOffset(dataBlock.size(), "writing GFF V4 list");
                writeU32LETo(dataBlock, fieldData, listRelative);
                const UInt32 outputListCount = list.gff4CompactPrimitiveList ?
                    list.gff4PrimitiveListCount : checkedCountToUInt32(list.count(), "writing GFF V4 list count");
                appendU32LE(dataBlock, outputListCount);
                if (isStruct) {
                    if (fieldTemplate.type >= gff4Templates_.size()) throw GffError("GFF V4 writer invalid list struct template.");
                    const UInt32 itemTemplateIndex = fieldTemplate.type;
                    const UInt32 itemSize = gff4Templates_[itemTemplateIndex].size;
                    const UInt32 firstItem = checkedVectorOffset(dataBlock.size(), "writing GFF V4 list items");
                    ensureData(firstItem, checkedUInt32Mul(checkedCountToUInt32(list.count(), "writing list items"), itemSize, "writing list items"));
                    for (std::size_t i = 0; i < list.count(); ++i) {
                        const GffStruct* item = list.GetStruct(i);
                        if (!item) throw GffError("GFF V4 writer encountered a null list item.");
                        writeStruct(*item, itemTemplateIndex, checkedUInt32Add(firstItem, checkedUInt32Mul(checkedCountToUInt32(i, "writing list item index"), itemSize, "writing list item"), "writing list item"));
                    }
                } else {
                    const UInt32 itemSize = gff4PrimitiveStorageSize(fieldTemplate.type);
                    if (list.gff4CompactPrimitiveList) {
                        if (list.gff4PrimitiveItemSize != 0u && list.gff4PrimitiveItemSize != itemSize) {
                            throw GffError("GFF V4 writer compact primitive-list item size changed.");
                        }
                        if (checkedUInt64Mul(list.gff4PrimitiveListCount, itemSize, "writing compact primitive list") != list.gff4PrimitiveListData.size()) {
                            throw GffError("GFF V4 writer compact primitive-list byte count does not match item count.");
                        }
                        const UInt32 firstItem = checkedVectorOffset(dataBlock.size(), "writing compact GFF V4 primitive-list items");
                        ensureData(firstItem, checkedSizeToUInt32(list.gff4PrimitiveListData.size(), "writing compact primitive-list items"));
                        if (!list.gff4PrimitiveListData.empty()) {
                            writeBytesTo(dataBlock, firstItem, list.gff4PrimitiveListData.data(), list.gff4PrimitiveListData.size());
                        }
                    } else {
                        const UInt32 firstItem = checkedVectorOffset(dataBlock.size(), "writing GFF V4 primitive list items");
                        ensureData(firstItem, checkedUInt32Mul(checkedCountToUInt32(list.count(), "writing primitive list items"), itemSize, "writing primitive list items"));
                        for (std::size_t i = 0; i < list.count(); ++i) {
                            const GffStruct* item = list.GetStruct(i);
                            if (!item || item->count() == 0) continue;
                            const GffField* value = item->GetField(0);
                            if (!value) continue;
                            Gff4FieldTemplate synthetic = fieldTemplate;
                            synthetic.flags &= static_cast<std::uint16_t>(~0x8000u);
                            synthetic.offset = 0u;
                            writeScalar(*value, synthetic, checkedUInt32Add(firstItem, checkedUInt32Mul(checkedCountToUInt32(i, "writing primitive list item index"), itemSize, "writing primitive list item"), "writing primitive list item"));
                        }
                    }
                }
            } else if (isStruct) {
                const auto& nested = checkedGffCast<GffStruct>(*field);
                if (isReference) {
                    appendPadding();
                    const UInt32 nestedRelative = checkedVectorOffset(dataBlock.size(), "writing referenced GFF V4 struct");
                    writeU32LETo(dataBlock, fieldData, nestedRelative);
                    writeStruct(nested, fieldTemplate.type, nestedRelative);
                } else {
                    writeStruct(nested, fieldTemplate.type, fieldData);
                }
            } else {
                writeScalar(*field, fieldTemplate, fieldData);
            }
        }
    };

    writeStruct(*rootStruct_, 0u, 0u);

    UInt32 fieldCountTotal = 0;
    for (const auto& tmpl : gff4Templates_) {
        fieldCountTotal = checkedUInt32Add(fieldCountTotal, checkedCountToUInt32(tmpl.fields.size(), "counting GFF V4 fields"), "counting GFF V4 fields");
    }
    const UInt32 structCount = checkedCountToUInt32(gff4Templates_.size(), "writing GFF V4 struct count");
    const UInt32 structArrayOffset = 28u;
    const UInt32 fieldArrayOffset = checkedUInt32Add(structArrayOffset, checkedUInt32Mul(structCount, 16u, "computing GFF V4 field-array offset"), "computing GFF V4 field-array offset");
    const UInt32 dataOffset = checkedUInt32Add(fieldArrayOffset, checkedUInt32Mul(fieldCountTotal, 12u, "computing GFF V4 data offset"), "computing GFF V4 data offset");

    std::vector<std::uint8_t> output;
    output.reserve(static_cast<std::size_t>(dataOffset) + dataBlock.size());
    auto appendString = [&](const char* text, std::size_t n) {
        output.insert(output.end(), reinterpret_cast<const std::uint8_t*>(text), reinterpret_cast<const std::uint8_t*>(text) + n);
    };
    appendString("GFF V4.0", 8u);
    output.insert(output.end(), gff4PlatformRaw_.begin(), gff4PlatformRaw_.end());
    output.insert(output.end(), filetypeRaw_.begin(), filetypeRaw_.end());
    output.insert(output.end(), fileversionRaw_.begin(), fileversionRaw_.end());
    appendU32LE(output, structCount);
    appendU32LE(output, dataOffset);

    UInt32 currentFieldOffset = fieldArrayOffset;
    for (const auto& tmpl : gff4Templates_) {
        appendU32LE(output, tmpl.typeidValue);
        appendU32LE(output, checkedCountToUInt32(tmpl.fields.size(), "writing GFF V4 struct field count"));
        appendU32LE(output, currentFieldOffset);
        appendU32LE(output, tmpl.size);
        currentFieldOffset = checkedUInt32Add(currentFieldOffset, checkedUInt32Mul(checkedCountToUInt32(tmpl.fields.size(), "advancing GFF V4 field offset"), 12u, "advancing GFF V4 field offset"), "advancing GFF V4 field offset");
    }
    for (const auto& tmpl : gff4Templates_) {
        for (const auto& field : tmpl.fields) {
            appendU32LE(output, field.label);
            appendU32LE(output, static_cast<UInt32>(field.type) | (static_cast<UInt32>(field.flags) << 16u));
            appendU32LE(output, field.offset);
        }
    }
    if (output.size() != dataOffset) throw GffError("Internal GFF V4 writer header size mismatch.");
    output.insert(output.end(), dataBlock.begin(), dataBlock.end());

    const std::filesystem::path temp = MakeSiblingTempPath(target, "gff4");
    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) throw GffError("Unable to open temporary GFF V4 file for writing: " + temp.string());
        out.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(output.size()));
        if (!out) throw GffError("Unable to write temporary GFF V4 file: " + temp.string());
        out.close();
        FlushFileToDisk(temp);
        ReplaceFileWithTemp(temp, target);
        isDirty_ = false;
    } catch (...) {
        RemoveFileNoThrow(temp);
        throw;
    }
}

void GffFile::SaveFile(const std::filesystem::path& outFilename) {
    if (!isLoaded_) {
        throw GffError("Unable to save, no file has been loaded that can be saved!");
    }
    if (isGff4_) {
        SaveGff4File(outFilename);
        return;
    }

    const std::filesystem::path target = ResolveOutputTarget(outFilename.empty() ? filename_ : outFilename);
    const Header previousHeader = header_;
    const SaveData previousSaveData = saveData_;
    std::filesystem::path temp;

    header_ = Header{};
    saveData_ = SaveData{};

    try {
        saveData_.StructCount = checkedUInt32Add(saveData_.StructCount, 1u, "counting structs for save");
        if (!rootStruct_) {
            throw GffError("Access violation reading GFF root struct while saving file.");
        }
        if (rootStruct_->count() > 1) {
            saveData_.FieldIndexCount = checkedUInt32Add(saveData_.FieldIndexCount, checkedCountToUInt32(rootStruct_->count(), "counting root field indexes"), "counting root field indexes");
        }
        SaveParseStruct(*rootStruct_);

        header_.filetype = filetypeRaw_;
        header_.fileversion = fileversionRaw_;
        header_.structoffset = static_cast<UInt32>(GFF_HEADER_SIZE);
        header_.structcount = saveData_.StructCount;
        header_.fieldoffset = checkedUInt32Add(header_.structoffset, checkedUInt32Mul(header_.structcount, 12u, "computing Field Array offset"), "computing Field Array offset");
        header_.fieldcount = saveData_.FieldCount;
        header_.labeloffset = checkedUInt32Add(header_.fieldoffset, checkedUInt32Mul(header_.fieldcount, 12u, "computing Label Array offset"), "computing Label Array offset");
        header_.labelcount = checkedCountToUInt32(saveData_.FieldLabels.size(), "computing label count");
        header_.fielddataoffset = checkedUInt32Add(header_.labeloffset, checkedUInt32Mul(header_.labelcount, 16u, "computing Field Data offset"), "computing Field Data offset");
        header_.fielddatacount = saveData_.DataBlockSize;
        header_.fieldindexoffset = checkedUInt32Add(header_.fielddataoffset, header_.fielddatacount, "computing Field Indices offset");
        header_.fieldindexcount = checkedUInt32Mul(saveData_.FieldIndexCount, 4u, "computing Field Indices byte count");
        header_.listindexoffset = checkedUInt32Add(header_.fieldindexoffset, header_.fieldindexcount, "computing List Indices offset");
        header_.listindexcount = checkedUInt32Add(checkedUInt32Mul(saveData_.ListIndexCount, 4u, "computing List Indices struct-index byte count"), checkedUInt32Mul(saveData_.ListCount, 4u, "computing List Indices count byte count"), "computing List Indices byte count");

        temp = MakeSiblingTempPath(target, "gff");
        writer_ = std::make_unique<BinaryWriter>(temp);

        writer_->writeBytes(header_.filetype.data(), 4);
        writer_->writeBytes(header_.fileversion.data(), 4);
        writer_->write(header_.structoffset);
        writer_->write(header_.structcount);
        writer_->write(header_.fieldoffset);
        writer_->write(header_.fieldcount);
        writer_->write(header_.labeloffset);
        writer_->write(header_.labelcount);
        writer_->write(header_.fielddataoffset);
        writer_->write(header_.fielddatacount);
        writer_->write(header_.fieldindexoffset);
        writer_->write(header_.fieldindexcount);
        writer_->write(header_.listindexoffset);
        writer_->write(header_.listindexcount);

        saveData_.CurrStructOffset = header_.structoffset;
        saveData_.CurrFieldOffset = header_.fieldoffset;
        saveData_.CurrFieldDataOffset = header_.fielddataoffset;
        saveData_.CurrFieldIndexOffset = header_.fieldindexoffset;
        saveData_.CurrListIndexOffset = header_.listindexoffset;

        SaveProcessLabels();
        SaveProcessStruct(*rootStruct_);

        writer_->close();
        writer_.reset();
        FlushFileToDisk(temp);
        ReplaceFileWithTemp(temp, target);

        isDirty_ = false;
    } catch (...) {
        writer_.reset();
        RemoveFileNoThrow(temp);
        header_ = previousHeader;
        saveData_ = previousSaveData;
        throw;
    }
}

UInt32 GffFile::SaveProcessStruct(const GffStruct& structure) {
    writer_->seek(saveData_.CurrStructOffset);
    const UInt32 resultIndex = saveData_.CurrStructIndex;
    saveData_.CurrStructIndex = checkedUInt32Add(saveData_.CurrStructIndex, 1u, "advancing save struct index");
    saveData_.CurrStructOffset = checkedUInt32Add(saveData_.CurrStructOffset, 12u, "advancing save struct offset");

    writer_->write(structure.typeid_);
    const UInt32 dataOrOffsetPosition = writer_->position();
    UInt32 placeholder = 0xFFFFFFFFu;
    writer_->write(placeholder);
    writer_->write(checkedCountToUInt32(structure.count(), "writing struct field count"));

    if (structure.count() > 1) {
        const UInt32 fieldIndexOffset = saveData_.CurrFieldIndexOffset - header_.fieldindexoffset;
        UInt32 nextIndexPosition = saveData_.CurrFieldIndexOffset;
        saveData_.CurrFieldIndexOffset = checkedUInt32Add(saveData_.CurrFieldIndexOffset, checkedUInt32Mul(checkedCountToUInt32(structure.count(), "reserving field index table"), 4u, "reserving field index table"), "reserving field index table");

        for (std::size_t i = 0; i < structure.count(); ++i) {
            const GffField* child = structure.GetField(i);
            const UInt32 fieldIndex = SaveProcessFieldSlot(child, structure.GetLabel());
            writer_->seek(nextIndexPosition);
            writer_->write(fieldIndex);
            nextIndexPosition = checkedUInt32Add(nextIndexPosition, 4u, "advancing GFF index write position");
        }

        writer_->seek(dataOrOffsetPosition);
        writer_->write(fieldIndexOffset);
    } else if (structure.count() == 1) {
        const GffField* child = structure.GetField(0);
        const UInt32 fieldIndex = SaveProcessFieldSlot(child, structure.GetLabel());
        writer_->seek(dataOrOffsetPosition);
        writer_->write(fieldIndex);
    }

    return resultIndex;
}

UInt32 GffFile::SaveProcessStructSlot(const GffStruct* structure) {
    if (structure == nullptr) {
        throw GffError("Invalid null GFF STRUCT while saving LIST!");
    }
    return SaveProcessStruct(*structure);
}

UInt32 GffFile::SaveProcessFieldSlot(const GffField* field, const std::string& structureLabel) {
    if (field == nullptr) {
        throw GffError("Invalid null GFF field while saving struct " + structureLabel + "!");
    }
    return SaveProcessField(*field);
}

UInt32 GffFile::SaveProcessField(const GffField& field) {
    writer_->seek(saveData_.CurrFieldOffset);
    const UInt32 resultIndex = saveData_.CurrFieldIndex;
    saveData_.CurrFieldIndex = checkedUInt32Add(saveData_.CurrFieldIndex, 1u, "advancing save field index");
    saveData_.CurrFieldOffset = checkedUInt32Add(saveData_.CurrFieldOffset, 12u, "advancing save field offset");

    writer_->write(field.fieldtype);
    const UInt32 labelIndex = SaveGetLabelIndex(field.GetLabelRaw());
    writer_->write(labelIndex);
    const UInt32 dataOrOffsetPosition = writer_->position();

    switch (field.fieldtype) {
    case FIELD_TYPE_BYTE:
        writer_->write(checkedGffCast<GffByteField>(field).value);
        writer_->writeZero(3);
        break;
    case FIELD_TYPE_CHAR:
        writer_->write(checkedGffCast<GffCharField>(field).value);
        writer_->writeZero(3);
        break;
    case FIELD_TYPE_WORD:
        writer_->write(checkedGffCast<GffWordField>(field).value);
        writer_->writeZero(2);
        break;
    case FIELD_TYPE_SHORT:
        writer_->write(checkedGffCast<GffShortField>(field).value);
        writer_->writeZero(2);
        break;
    case FIELD_TYPE_DWORD:
        writer_->write(checkedGffCast<GffUInt32Field>(field).value);
        break;
    case FIELD_TYPE_INT:
        writer_->write(checkedGffCast<GffIntField>(field).value);
        break;
    case FIELD_TYPE_FLOAT:
        writer_->write(checkedGffCast<GffFloatField>(field).value);
        break;
    case FIELD_TYPE_DWORD64:
    case FIELD_TYPE_INT64:
    case FIELD_TYPE_DOUBLE:
    case FIELD_TYPE_CEXOSTRING:
    case FIELD_TYPE_RESREF:
    case FIELD_TYPE_CEXOLOCSTRING:
    case FIELD_TYPE_VOID:
    case FIELD_TYPE_ORIENTATION:
    case FIELD_TYPE_POSITION:
    case FIELD_TYPE_JADE_STRREF: {
        const UInt32 dataOffset = SaveProcessComplexFieldData(field);
        writer_->seek(dataOrOffsetPosition);
        writer_->write(dataOffset);
        break;
    }
    case FIELD_TYPE_STRUCT: {
        const UInt32 structIndex = SaveProcessStruct(checkedGffCast<GffStruct>(field));
        writer_->seek(dataOrOffsetPosition);
        writer_->write(structIndex);
        break;
    }
    case FIELD_TYPE_LIST: {
        const UInt32 listOffset = SaveProcessList(checkedGffCast<GffList>(field));
        writer_->seek(dataOrOffsetPosition);
        writer_->write(listOffset);
        break;
    }
    default:
        throw GffError("Invalid GFF field type " + std::to_string(field.fieldtype) + " encountered while saving.");
    }

    return resultIndex;
}

UInt32 GffFile::SaveProcessComplexFieldData(const GffField& field) {
    writer_->seek(saveData_.CurrFieldDataOffset);
    const UInt32 resultOffset = checkedUInt32Sub(saveData_.CurrFieldDataOffset, header_.fielddataoffset, "computing field data relative offset");
    saveData_.CurrFieldDataOffset = checkedUInt32Add(saveData_.CurrFieldDataOffset, GetFieldDataSize(field), "advancing field data write offset");

    switch (field.fieldtype) {
    case FIELD_TYPE_DWORD64: {
        const auto bytes = uint64ToBytesLE(checkedGffCast<GffUInt64Field>(field).value);
        writer_->writeBytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        break;
    }
    case FIELD_TYPE_INT64:
        writer_->write(checkedGffCast<GffInt64Field>(field).value);
        break;
    case FIELD_TYPE_DOUBLE:
        writer_->write(checkedGffCast<GffDoubleField>(field).value);
        break;
    case FIELD_TYPE_JADE_STRREF: {
        const auto& f = checkedGffCast<GffJadeStringRefField>(field);
        writer_->write(f.stringType);
        writer_->write(f.strref);
        break;
    }
    case FIELD_TYPE_CEXOSTRING: {
        const auto& f = checkedGffCast<GffExoStringField>(field);
        writer_->write(f.size);
        for (UInt32 i = 0; i < f.size; ++i) {
            writer_->write(f.textData.at(static_cast<std::size_t>(i)));
        }
        break;
    }
    case FIELD_TYPE_RESREF: {
        const auto& f = checkedGffCast<GffResRefField>(field);
        writer_->write(f.size);
        for (UInt32 i = 0; i < f.size; ++i) {
            writer_->write(f.textData.at(static_cast<std::size_t>(i)));
        }
        break;
    }
    case FIELD_TYPE_CEXOLOCSTRING: {
        const auto& f = checkedGffCast<GffLocalizedStringField>(field);
        writer_->write(f.bytesize);
        writer_->write(f.strref);
        writer_->write(f.stringcount);
        for (const auto& sub : f.substrings) {
            writer_->write(sub.stringid);
            writer_->write(sub.stringlength);
            for (std::int32_t i = 0; i < sub.stringlength; ++i) {
                writer_->write(sub.textData.at(static_cast<std::size_t>(i)));
            }
        }
        break;
    }
    case FIELD_TYPE_VOID: {
        const auto& f = checkedGffCast<GffVoidField>(field);
        writer_->write(f.bytesize);
        for (UInt32 i = 0; i < f.bytesize; ++i) {
            writer_->write(f.data.at(static_cast<std::size_t>(i)));
        }
        break;
    }
    case FIELD_TYPE_ORIENTATION: {
        const auto& f = checkedGffCast<GffOrientationField>(field);
        for (float value : f.value) {
            writer_->write(value);
        }
        break;
    }
    case FIELD_TYPE_POSITION: {
        const auto& f = checkedGffCast<GffPositionField>(field);
        for (float value : f.value) {
            writer_->write(value);
        }
        break;
    }
    default:
        break;
    }
    return resultOffset;
}

UInt32 GffFile::SaveProcessList(const GffList& list) {
    writer_->seek(saveData_.CurrListIndexOffset);
    const UInt32 resultOffset = checkedUInt32Sub(saveData_.CurrListIndexOffset, header_.listindexoffset, "computing list relative offset");
    saveData_.CurrListIndexOffset = checkedUInt32Add(saveData_.CurrListIndexOffset, checkedUInt32Add(4u, checkedUInt32Mul(checkedCountToUInt32(list.count(), "reserving list index table"), 4u, "reserving list index table"), "reserving list index table"), "reserving list index table");

    writer_->write(checkedCountToUInt32(list.count(), "writing list struct count"));
    UInt32 nextIndexPosition = writer_->position();
    for (std::size_t i = 0; i < list.count(); ++i) {
        const UInt32 structIndex = SaveProcessStructSlot(list.GetStruct(i));
        writer_->seek(nextIndexPosition);
        writer_->write(structIndex);
        nextIndexPosition = checkedUInt32Add(nextIndexPosition, 4u, "advancing GFF index write position");
    }
    return resultOffset;
}

} // namespace neogff
