#include "GffXml.hpp"

#include "GffTypeNames.hpp"

#include "SimpleXml.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace neogff {
namespace {

std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string rtrimSpacesAndNuls(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) value.pop_back();
    return value;
}

std::string normalizeGffTypeForImport(std::string type) {
    type = trim(type);
    if (type.empty()) throw std::invalid_argument("GFF XML root is missing type attribute.");
    if (type.size() == 3) type.push_back(' ');
    if (type.size() > 4) type.resize(4);
    return type;
}

std::string scalarText(const neoxml::Node& node) {
    return trim(node.text);
}

std::uint8_t parseU8(const std::string& text, const char* what) {
    const auto value = ParseUInt32Decimal(trim(text));
    if (value > std::numeric_limits<std::uint8_t>::max()) throw std::out_of_range(std::string(what) + " out of range");
    return static_cast<std::uint8_t>(value);
}

std::uint16_t parseU16(const std::string& text, const char* what) {
    const auto value = ParseUInt32Decimal(trim(text));
    if (value > std::numeric_limits<std::uint16_t>::max()) throw std::out_of_range(std::string(what) + " out of range");
    return static_cast<std::uint16_t>(value);
}

std::int16_t parseI16(const std::string& text, const char* what) {
    const auto value = ParseInt32Decimal(trim(text));
    if (value < std::numeric_limits<std::int16_t>::min() || value > std::numeric_limits<std::int16_t>::max()) throw std::out_of_range(std::string(what) + " out of range");
    return static_cast<std::int16_t>(value);
}

char parseI8AsChar(const std::string& text, const char* what) {
    const auto value = ParseInt32Decimal(trim(text));
    if (value < std::numeric_limits<std::int8_t>::min() || value > std::numeric_limits<std::int8_t>::max()) throw std::out_of_range(std::string(what) + " out of range");
    return static_cast<char>(static_cast<std::int8_t>(value));
}

std::uint32_t parseStrRef(const std::string& text) {
    const std::string v = trim(text);
    if (v.empty() || v == "-1") return 0xFFFFFFFFu;
    return ParseUInt32Decimal(v);
}

static constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(((size + 2u) / 3u) * 4u);
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = (i + 1 < size) ? data[i + 1] : 0u;
        const unsigned b2 = (i + 2 < size) ? data[i + 2] : 0u;
        out.push_back(kBase64Alphabet[(b0 >> 2u) & 0x3Fu]);
        out.push_back(kBase64Alphabet[((b0 << 4u) | (b1 >> 4u)) & 0x3Fu]);
        out.push_back(i + 1 < size ? kBase64Alphabet[((b1 << 2u) | (b2 >> 6u)) & 0x3Fu] : '=');
        out.push_back(i + 2 < size ? kBase64Alphabet[b2 & 0x3Fu] : '=');
    }
    return out;
}

ByteBuffer base64Decode(std::string_view text) {
    auto valueOf = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        if (ch == '=') return -2;
        return -1;
    };
    std::string compact;
    compact.reserve(text.size());
    for (char ch : text) {
        if (!std::isspace(static_cast<unsigned char>(ch))) compact.push_back(ch);
    }
    if (compact.empty()) return {};
    if (compact.size() % 4u != 0u) throw std::invalid_argument("Malformed base64 XML payload.");
    ByteBuffer out;
    out.reserve((compact.size() / 4u) * 3u);
    for (std::size_t i = 0; i < compact.size(); i += 4) {
        const int a = valueOf(compact[i]);
        const int b = valueOf(compact[i + 1]);
        const int c = valueOf(compact[i + 2]);
        const int d = valueOf(compact[i + 3]);
        const bool finalBlock = i + 4 == compact.size();
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            throw std::invalid_argument("Malformed base64 XML payload.");
        }
        out.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
        if (c == -2) {
            if (d != -2 || !finalBlock || (b & 0x0F) != 0) {
                throw std::invalid_argument("Malformed base64 XML payload.");
            }
            continue;
        }
        out.push_back(static_cast<std::uint8_t>(((b & 0x0F) << 4) | (c >> 2)));
        if (d == -2) {
            if (!finalBlock || (c & 0x03) != 0) {
                throw std::invalid_argument("Malformed base64 XML payload.");
            }
            continue;
        }
        out.push_back(static_cast<std::uint8_t>(((c & 0x03) << 6) | d));
    }
    return out;
}

std::string bytesToString(const ByteBuffer& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

std::string fieldContentForXml(const GffField& field) {
    switch (field.fieldtype) {
    case FIELD_TYPE_BYTE:
        return std::to_string(dynamic_cast<const GffByteField&>(field).value);
    case FIELD_TYPE_CHAR:
        return std::to_string(static_cast<int>(static_cast<std::int8_t>(dynamic_cast<const GffCharField&>(field).value)));
    case FIELD_TYPE_WORD:
        return std::to_string(dynamic_cast<const GffWordField&>(field).value);
    case FIELD_TYPE_SHORT:
        return std::to_string(dynamic_cast<const GffShortField&>(field).value);
    case FIELD_TYPE_DWORD:
        return std::to_string(dynamic_cast<const GffUInt32Field&>(field).value);
    case FIELD_TYPE_INT:
        return std::to_string(dynamic_cast<const GffIntField&>(field).value);
    case FIELD_TYPE_DWORD64:
        return std::to_string(dynamic_cast<const GffUInt64Field&>(field).value);
    case FIELD_TYPE_INT64:
        return std::to_string(dynamic_cast<const GffInt64Field&>(field).value);
    case FIELD_TYPE_FLOAT:
        return FormatNumber(dynamic_cast<const GffFloatField&>(field).value);
    case FIELD_TYPE_DOUBLE:
        return FormatNumber(dynamic_cast<const GffDoubleField&>(field).value);
    case FIELD_TYPE_CEXOSTRING:
        return dynamic_cast<const GffExoStringField&>(field).GetString();
    case FIELD_TYPE_RESREF:
        return dynamic_cast<const GffResRefField&>(field).GetString();
    case FIELD_TYPE_JADE_STRREF:
        return dynamic_cast<const GffJadeStringRefField&>(field).GetString();
    default:
        return field.GetString();
    }
}

void indent(std::ostringstream& out, int level) {
    for (int i = 0; i < level; ++i) out << "  ";
}

void writeStructXml(std::ostringstream& out, const GffStruct& structure, int level, const std::string* label);

void writeFieldXml(std::ostringstream& out, const GffField& field, int level) {
    if (field.fieldtype == FIELD_TYPE_STRUCT) {
        const std::string label = field.GetLabel();
        writeStructXml(out, dynamic_cast<const GffStruct&>(field), level, &label);
        return;
    }

    const std::string tag = gffXmlTagForFieldType(field.fieldtype);
    indent(out, level);
    out << '<' << tag << " label=\"" << neoxml::escapeAttribute(field.GetLabel()) << "\"";

    if (field.fieldtype == FIELD_TYPE_CEXOLOCSTRING) {
        const auto& loc = dynamic_cast<const GffLocalizedStringField&>(field);
        out << " strref=\"" << (loc.strref == 0xFFFFFFFFu ? std::string("-1") : std::to_string(loc.strref)) << "\"";
        if (loc.substrings.empty()) {
            out << "/>\n";
            return;
        }
        out << ">\n";
        for (const auto& sub : loc.substrings) {
            indent(out, level + 1);
            out << "<string language=\"" << sub.stringid << "\">" << neoxml::escapeText(sub.GetString()) << "</string>\n";
        }
        indent(out, level);
        out << "</" << tag << ">\n";
        return;
    }

    if (field.fieldtype == FIELD_TYPE_LIST) {
        const auto& list = dynamic_cast<const GffList&>(field);
        if (list.count() == 0) {
            out << "/>\n";
            return;
        }
        out << ">\n";
        for (const auto& structPtr : list.allStructs()) {
            if (structPtr) writeStructXml(out, *structPtr, level + 1, nullptr);
        }
        indent(out, level);
        out << "</" << tag << ">\n";
        return;
    }

    if (field.fieldtype == FIELD_TYPE_VOID) {
        const auto& data = dynamic_cast<const GffVoidField&>(field).data;
        out << '>' << base64Encode(data.data(), data.size()) << "</" << tag << ">\n";
        return;
    }

    if (field.fieldtype == FIELD_TYPE_ORIENTATION || field.fieldtype == FIELD_TYPE_POSITION) {
        out << ">\n";
        if (field.fieldtype == FIELD_TYPE_ORIENTATION) {
            for (float value : dynamic_cast<const GffOrientationField&>(field).value) {
                indent(out, level + 1);
                out << "<double>" << neoxml::escapeText(FormatNumber(value)) << "</double>\n";
            }
        } else {
            for (float value : dynamic_cast<const GffPositionField&>(field).value) {
                indent(out, level + 1);
                out << "<double>" << neoxml::escapeText(FormatNumber(value)) << "</double>\n";
            }
        }
        indent(out, level);
        out << "</" << tag << ">\n";
        return;
    }

    out << '>' << neoxml::escapeText(fieldContentForXml(field)) << "</" << tag << ">\n";
}

void writeStructXml(std::ostringstream& out, const GffStruct& structure, int level, const std::string* label) {
    indent(out, level);
    out << "<struct";
    if (label) out << " label=\"" << neoxml::escapeAttribute(*label) << "\"";
    out << " id=\"" << structure.typeid_ << "\"";
    if (structure.count() == 0) {
        out << "/>\n";
        return;
    }
    out << ">\n";
    for (const auto& field : structure.allFields()) {
        if (field) writeFieldXml(out, *field, level + 1);
    }
    indent(out, level);
    out << "</struct>\n";
}

std::uint32_t parseStructId(const neoxml::Node& node, std::uint32_t fallback) {
    const std::string text = node.attribute("id");
    if (text.empty()) return fallback;
    return ParseUInt32Decimal(trim(text));
}

std::vector<float> parseVectorComponents(const neoxml::Node& node, std::size_t expectedCount, const char* what) {
    std::vector<float> out;
    for (const auto& child : node.children) {
        if (child.name != "double" && child.name != "float") {
            throw std::invalid_argument(std::string("Unexpected <") + child.name + "> inside " + what + ".");
        }
        out.push_back(ParseFloatDecimal(scalarText(child)));
    }
    if (out.size() != expectedCount) {
        throw std::invalid_argument(std::string(what) + " must contain " + std::to_string(expectedCount) + " numeric components.");
    }
    return out;
}

std::unique_ptr<GffStruct> parseStructNode(const neoxml::Node& node, bool requireLabel,
                                           std::uint32_t fallbackId = 0u);

std::unique_ptr<GffField> parseFieldNode(const neoxml::Node& node) {
    const std::string label = node.attribute("label");
    auto requireLabel = [&]() {
        if (label.empty()) throw std::invalid_argument("GFF XML <" + node.name + "> field is missing required label attribute.");
    };

    if (node.name == "byte") { requireLabel(); return std::make_unique<GffByteField>(label, parseU8(scalarText(node), "byte")); }
    if (node.name == "char") { requireLabel(); return std::make_unique<GffCharField>(label, parseI8AsChar(scalarText(node), "char")); }
    if (node.name == "uint16_t" || node.name == "word") { requireLabel(); return std::make_unique<GffWordField>(label, parseU16(scalarText(node), "uint16_t")); }
    if (node.name == "sint16" || node.name == "short") { requireLabel(); return std::make_unique<GffShortField>(label, parseI16(scalarText(node), "sint16")); }
    if (node.name == "uint32_t" || node.name == "dword") { requireLabel(); return std::make_unique<GffUInt32Field>(label, ParseUInt32Decimal(scalarText(node))); }
    if (node.name == "sint32" || node.name == "int") { requireLabel(); return std::make_unique<GffIntField>(label, ParseInt32Decimal(scalarText(node))); }
    if (node.name == "uint64_t" || node.name == "dword64") { requireLabel(); return std::make_unique<GffUInt64Field>(label, ParseUInt64Decimal(scalarText(node))); }
    if (node.name == "sint64" || node.name == "int64") { requireLabel(); return std::make_unique<GffInt64Field>(label, ParseInt64Decimal(scalarText(node))); }
    if (node.name == "float") { requireLabel(); return std::make_unique<GffFloatField>(label, ParseFloatDecimal(scalarText(node))); }
    if (node.name == "double") { requireLabel(); return std::make_unique<GffDoubleField>(label, ParseDoubleDecimal(scalarText(node))); }
    if (node.name == "exostring") {
        requireLabel();
        if (node.attribute("base64") == "true") return std::make_unique<GffExoStringField>(label, bytesToString(base64Decode(node.text)));
        return std::make_unique<GffExoStringField>(label, node.text);
    }
    if (node.name == "resref") {
        requireLabel();
        if (node.attribute("base64") == "true") return std::make_unique<GffResRefField>(label, bytesToString(base64Decode(node.text)));
        return std::make_unique<GffResRefField>(label, trim(node.text));
    }
    if (node.name == "locstring") {
        requireLabel();
        auto loc = std::make_unique<GffLocalizedStringField>(label, parseStrRef(node.attribute("strref")));
        for (const auto& child : node.children) {
            if (child.name != "string") throw std::invalid_argument("Unexpected <" + child.name + "> inside <locstring>.");
            const auto lang = ParseInt32Decimal(trim(child.attribute("language")));
            loc->AddString(lang, child.text);
        }
        return loc;
    }
    if (node.name == "data") {
        requireLabel();
        return std::make_unique<GffVoidField>(label, base64Decode(node.text));
    }
    if (node.name == "struct") {
        requireLabel();
        return parseStructNode(node, true);
    }
    if (node.name == "list") {
        requireLabel();
        auto list = std::make_unique<GffList>(label);
        for (const auto& child : node.children) {
            if (child.name != "struct") throw std::invalid_argument("Unexpected <" + child.name + "> inside <list>.");
            list->AddStruct(parseStructNode(child, false));
        }
        return list;
    }
    if (node.name == "orientation") {
        requireLabel();
        const auto v = parseVectorComponents(node, 4, "orientation");
        return std::make_unique<GffOrientationField>(label, v[0], v[1], v[2], v[3]);
    }
    if (node.name == "vector" || node.name == "position") {
        requireLabel();
        const auto v = parseVectorComponents(node, 3, "vector");
        return std::make_unique<GffPositionField>(label, v[0], v[1], v[2]);
    }
    if (node.name == "strref" || node.name == "jadestringref") {
        requireLabel();
        const std::string text = scalarText(node);
        const auto pipe = text.find('|');
        if (pipe == std::string::npos) {
            return std::make_unique<GffJadeStringRefField>(label, 4u, parseStrRef(text));
        }
        const auto first = ParseUInt32Decimal(trim(text.substr(0, pipe)));
        const auto second = parseStrRef(text.substr(pipe + 1));
        return std::make_unique<GffJadeStringRefField>(label, first, second);
    }
    throw std::invalid_argument("Unsupported GFF XML element <" + node.name + ">.");
}

std::unique_ptr<GffStruct> parseStructNode(const neoxml::Node& node, bool requireLabel,
                                           std::uint32_t fallbackId) {
    if (node.name != "struct") throw std::invalid_argument("Expected <struct> element.");
    const std::string label = node.attribute("label");
    if (requireLabel && label.empty()) throw std::invalid_argument("Nested GFF XML <struct> is missing required label attribute.");
    auto structure = std::make_unique<GffStruct>(label);
    structure->typeid_ = parseStructId(node, fallbackId);
    for (const auto& child : node.children) {
        structure->AddField(parseFieldNode(child));
    }
    return structure;
}

} // namespace

std::string ToGffXml(const GffFile& gff) {
    if (!gff.loaded() || !gff.root()) {
        throw std::invalid_argument("No GFF file is loaded.");
    }
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<gff3 type=\"" << neoxml::escapeAttribute(rtrimSpacesAndNuls(gff.filetype())) << "\">\n";
    writeStructXml(out, *gff.root(), 1, nullptr);
    out << "</gff3>\n";
    return out.str();
}

void LoadGffXml(GffFile& gff, const std::string& xmlText) {
    const neoxml::Node root = neoxml::parse(xmlText);
    if (root.name != "gff3") {
        throw std::invalid_argument("XML does not describe a GFF3 file: expected <gff3> root.");
    }
    const std::string type = normalizeGffTypeForImport(root.attribute("type"));
    const neoxml::Node* rootStructNode = nullptr;
    for (const auto& child : root.children) {
        if (child.name != "struct") throw std::invalid_argument("GFF XML root may only contain one <struct> child.");
        if (rootStructNode) throw std::invalid_argument("GFF XML contains more than one root <struct>.");
        rootStructNode = &child;
    }
    if (!rootStructNode) throw std::invalid_argument("GFF XML does not contain a root <struct>.");

    auto parsed = parseStructNode(*rootStructNode, false, 0xFFFFFFFFu);
    const std::filesystem::path previousFilename = gff.filename();
    gff.NewFile(type, previousFilename);
    GffStruct* destination = gff.root();
    destination->typeid_ = parsed->typeid_;
    for (auto& field : parsed->allFields()) {
        if (field) destination->AddField(std::move(field));
    }
    gff.dirty(true);
}

} // namespace neogff
