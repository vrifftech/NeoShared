#include "GffJson.hpp"

#include "GffTypeNames.hpp"

#include "SimpleJson.hpp"
#include "SimpleXml.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace neogff {
namespace {

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

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

std::string gffTypeForJson(std::string type) {
    if (type.size() == 3) type.push_back(' ');
    return type;
}

std::string gffTypeForXml(std::string type) {
    type = trim(type);
    if (type.empty()) throw std::invalid_argument("GFF JSON is missing fileType/type.");
    return rtrimSpacesAndNuls(type);
}

std::string jsonMember(const std::string& key, const std::string& value, bool comma = true) {
    std::ostringstream out;
    out << neojson::quote(key) << ": " << value;
    if (comma) out << ',';
    return out.str();
}

bool looksLikeJsonNumber(std::string_view text) {
    if (text.empty()) return false;
    std::size_t pos = 0;
    if (text[pos] == '-') ++pos;
    if (pos >= text.size()) return false;
    if (text[pos] == '0') {
        ++pos;
    } else if (std::isdigit(static_cast<unsigned char>(text[pos]))) {
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    } else {
        return false;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) return false;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
        if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) return false;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    }
    return pos == text.size();
}

std::string numberOrZero(std::string text) {
    text = trim(text);
    return looksLikeJsonNumber(text) ? text : std::string("0");
}

std::string numberOrQuoted(const std::string& text) {
    const std::string trimmed = trim(text);
    return looksLikeJsonNumber(trimmed) ? trimmed : neojson::quote(trimmed);
}

std::string optionalStringOrNumber(const neojson::Value& object, const std::string& key, const std::string& fallback = {}) {
    const neojson::Value* value = object.find(key);
    if (!value || value->isNull()) return fallback;
    return value->asText(("JSON member '" + key + "'").c_str());
}

std::string optionalString(const neojson::Value& object, const std::string& key, const std::string& fallback = {}) {
    const neojson::Value* value = object.find(key);
    if (!value || value->isNull()) return fallback;
    return value->asString(("JSON member '" + key + "'").c_str());
}

std::string requireStringOrNumber(const neojson::Value& object, const std::string& key) {
    return object.at(key).asText(("JSON member '" + key + "'").c_str());
}

std::string escapeXmlText(const std::string& text) {
    return neoxml::escapeText(text);
}

std::string escapeXmlAttr(const std::string& text) {
    return neoxml::escapeAttribute(text);
}

void indent(std::ostringstream& out, int level) {
    for (int i = 0; i < level; ++i) out << "  ";
}

std::string scalarXmlText(const neoxml::Node& node) {
    return trim(node.text);
}

void writeGffStructJson(std::ostringstream& out, const neoxml::Node& node, int level,
                        bool includeLabel, const std::string& fallbackId = "0");

void writeGffFieldJson(std::ostringstream& out, const neoxml::Node& node, int level) {
    const std::string type = gffFieldTypeNameFromXmlTag(node.name);
    indent(out, level);
    out << "{\n";
    indent(out, level + 1);
    out << jsonMember("label", neojson::quote(node.attribute("label"))) << "\n";
    indent(out, level + 1);
    out << jsonMember("type", neojson::quote(type), false);

    if (type == "Struct") {
        out << ",\n";
        indent(out, level + 1);
        out << jsonMember("id", numberOrZero(node.attribute("id"))) << "\n";
        indent(out, level + 1);
        out << "\"fields\": [";
        if (!node.children.empty()) out << '\n';
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            writeGffFieldJson(out, node.children[i], level + 2);
            out << (i + 1 < node.children.size() ? "," : "") << '\n';
        }
        if (!node.children.empty()) indent(out, level + 1);
        out << "]\n";
        indent(out, level);
        out << '}';
        return;
    }

    if (type == "List") {
        out << ",\n";
        indent(out, level + 1);
        out << "\"items\": [";
        if (!node.children.empty()) out << '\n';
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            writeGffStructJson(out, node.children[i], level + 2, false, "0");
            out << (i + 1 < node.children.size() ? "," : "") << '\n';
        }
        if (!node.children.empty()) indent(out, level + 1);
        out << "]\n";
        indent(out, level);
        out << '}';
        return;
    }

    if (type == "CExoLocString") {
        out << ",\n";
        indent(out, level + 1);
        out << jsonMember("strref", numberOrZero(node.attribute("strref", "-1"))) << "\n";
        indent(out, level + 1);
        out << "\"strings\": [";
        if (!node.children.empty()) out << '\n';
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            const auto& child = node.children[i];
            indent(out, level + 2);
            out << "{ \"language\": " << numberOrZero(child.attribute("language"))
                << ", \"text\": " << neojson::quote(child.text) << " }"
                << (i + 1 < node.children.size() ? "," : "") << '\n';
        }
        if (!node.children.empty()) indent(out, level + 1);
        out << "]\n";
        indent(out, level);
        out << '}';
        return;
    }

    if (type == "Void") {
        out << ",\n";
        indent(out, level + 1);
        out << jsonMember("encoding", neojson::quote("base64")) << "\n";
        indent(out, level + 1);
        out << jsonMember("value", neojson::quote(trim(node.text)), false) << "\n";
        indent(out, level);
        out << '}';
        return;
    }

    if (type == "Orientation" || type == "Position") {
        out << ",\n";
        indent(out, level + 1);
        out << "\"value\": [";
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (i) out << ", ";
            out << numberOrQuoted(scalarXmlText(node.children[i]));
        }
        out << "]\n";
        indent(out, level);
        out << '}';
        return;
    }

    if (type == "JadeStringRef") {
        const std::string text = scalarXmlText(node);
        const std::size_t pipe = text.find('|');
        out << ",\n";
        indent(out, level + 1);
        out << "\"value\": ";
        if (pipe != std::string::npos) {
            const std::string subtype = trim(text.substr(0, pipe));
            const std::string strref = trim(text.substr(pipe + 1));
            out << "{ \"subtype\": " << numberOrQuoted(subtype) << ", \"strref\": " << numberOrQuoted(strref) << " }\n";
        } else {
            out << neojson::quote(text) << "\n";
        }
        indent(out, level);
        out << '}';
        return;
    }

    out << ",\n";
    indent(out, level + 1);
    if (type == "CExoString" || type == "CResRef" || type == "DWORD64" || type == "Int64") {
        out << jsonMember("value", neojson::quote(scalarXmlText(node)), false) << "\n";
    } else {
        out << jsonMember("value", numberOrQuoted(scalarXmlText(node)), false) << "\n";
    }
    indent(out, level);
    out << '}';
}

void writeGffStructJson(std::ostringstream& out, const neoxml::Node& node, int level,
                        bool includeLabel, const std::string& fallbackId) {
    if (node.name != "struct") throw std::invalid_argument("Expected <struct> while converting GFF XML to JSON.");
    indent(out, level);
    out << "{\n";
    if (includeLabel) {
        indent(out, level + 1);
        out << jsonMember("label", neojson::quote(node.attribute("label"))) << "\n";
    }
    indent(out, level + 1);
    out << jsonMember("type", neojson::quote("Struct")) << "\n";
    indent(out, level + 1);
    out << jsonMember("id", numberOrZero(node.attribute("id", fallbackId))) << "\n";
    indent(out, level + 1);
    out << "\"fields\": [";
    if (!node.children.empty()) out << '\n';
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        writeGffFieldJson(out, node.children[i], level + 2);
        out << (i + 1 < node.children.size() ? "," : "") << '\n';
    }
    if (!node.children.empty()) indent(out, level + 1);
    out << "]\n";
    indent(out, level);
    out << '}';
}

void writeGffJsonFieldAsXml(std::ostringstream& out, const neojson::Value& value, int level, bool requireLabel);

std::string jsonFieldLabel(const neojson::Value& field, bool requireLabel) {
    const neojson::Value* label = field.find("label");
    if (!label || label->isNull()) {
        if (requireLabel) throw std::invalid_argument("GFF JSON field is missing required label.");
        return {};
    }
    return label->asString("GFF JSON label");
}

std::string jsonType(const neojson::Value& field) {
    return field.at("type").asString("GFF JSON field type");
}

void writeGffJsonStructAsXml(std::ostringstream& out, const neojson::Value& value, int level,
                             bool includeLabel, const std::string& fallbackId = "0") {
    value.asObject("GFF JSON struct");
    indent(out, level);
    out << "<struct";
    if (includeLabel) out << " label=\"" << escapeXmlAttr(jsonFieldLabel(value, true)) << "\"";
    out << " id=\"" << escapeXmlAttr(optionalStringOrNumber(value, "id", fallbackId)) << "\"";
    const neojson::Value* fields = value.find("fields");
    if (!fields || fields->asArray("GFF JSON struct fields").empty()) {
        out << "/>\n";
        return;
    }
    out << ">\n";
    for (const auto& child : fields->asArray("GFF JSON struct fields")) {
        writeGffJsonFieldAsXml(out, child, level + 1, true);
    }
    indent(out, level);
    out << "</struct>\n";
}

void writeGffJsonFieldAsXml(std::ostringstream& out, const neojson::Value& value, int level, bool requireLabel) {
    value.asObject("GFF JSON field");
    const std::string typeText = jsonType(value);
    const std::string tag = gffXmlTagFromFieldTypeName(typeText);
    if (tag == "struct") {
        writeGffJsonStructAsXml(out, value, level, true);
        return;
    }
    const std::string label = jsonFieldLabel(value, requireLabel);
    indent(out, level);
    out << '<' << tag;
    if (requireLabel || !label.empty()) out << " label=\"" << escapeXmlAttr(label) << "\"";

    if (tag == "list") {
        const neojson::Value* items = value.find("items");
        if (!items || items->asArray("GFF JSON list items").empty()) {
            out << "/>\n";
            return;
        }
        out << ">\n";
        for (const auto& item : items->asArray("GFF JSON list items")) {
            writeGffJsonStructAsXml(out, item, level + 1, false);
        }
        indent(out, level);
        out << "</list>\n";
        return;
    }

    if (tag == "locstring") {
        out << " strref=\"" << escapeXmlAttr(optionalStringOrNumber(value, "strref", "-1")) << "\"";
        const neojson::Value* strings = value.find("strings");
        if (!strings || strings->asArray("GFF JSON localized strings").empty()) {
            out << "/>\n";
            return;
        }
        out << ">\n";
        for (const auto& item : strings->asArray("GFF JSON localized strings")) {
            item.asObject("GFF JSON localized string");
            indent(out, level + 1);
            out << "<string language=\"" << escapeXmlAttr(requireStringOrNumber(item, "language")) << "\">"
                << escapeXmlText(optionalString(item, "text")) << "</string>\n";
        }
        indent(out, level);
        out << "</locstring>\n";
        return;
    }

    if (tag == "orientation" || tag == "vector") {
        out << ">\n";
        const auto& components = value.at("value").asArray("GFF JSON vector value");
        for (const auto& component : components) {
            indent(out, level + 1);
            out << "<double>" << escapeXmlText(component.asText("GFF JSON vector component")) << "</double>\n";
        }
        indent(out, level);
        out << "</" << tag << ">\n";
        return;
    }

    std::string text;
    if (tag == "data") {
        const std::string encoding = lowerAscii(optionalString(value, "encoding", "base64"));
        if (encoding != "base64") throw std::invalid_argument("GFF JSON Void data must use base64 encoding.");
        text = value.at("value").asString("GFF JSON base64 value");
    } else if (tag == "strref") {
        const neojson::Value& jadeValue = value.at("value");
        if (jadeValue.isObject()) {
            text = requireStringOrNumber(jadeValue, "subtype") + "|" + requireStringOrNumber(jadeValue, "strref");
        } else {
            text = jadeValue.asText("GFF JSON JadeStringRef value");
        }
    } else {
        text = value.at("value").asText("GFF JSON field value");
    }
    out << '>' << escapeXmlText(text) << "</" << tag << ">\n";
}

} // namespace

std::string gffXmlToJson(const std::string& gffXml) {
    const neoxml::Node root = neoxml::parse(gffXml);
    if (root.name != "gff3") throw std::invalid_argument("GFF XML root must be <gff3>.");
    const neoxml::Node* rootStruct = nullptr;
    for (const auto& child : root.children) {
        if (child.name == "struct") {
            if (rootStruct) throw std::invalid_argument("GFF XML has more than one root struct.");
            rootStruct = &child;
        }
    }
    if (!rootStruct) throw std::invalid_argument("GFF XML has no root struct.");
    std::ostringstream out;
    out << "{\n"
        << "  \"format\": \"GFF3\",\n"
        << "  \"fileType\": " << neojson::quote(gffTypeForJson(root.attribute("type"))) << ",\n"
        << "  \"version\": " << neojson::quote(root.attribute("version", "V3.2")) << ",\n"
        << "  \"root\": ";
    writeGffStructJson(out, *rootStruct, 1, false, "4294967295");
    out << "\n}\n";
    return out.str();
}

std::string gffJsonToXml(const std::string& jsonText) {
    const neojson::Value root = neojson::parse(jsonText);
    if (!root.isObject()) throw std::invalid_argument("GFF JSON must be an object.");
    std::string fileType = optionalString(root, "fileType", optionalString(root, "type"));
    if (fileType.empty()) fileType = "GFF";
    const std::string version = optionalString(root, "version", "V3.2");
    if (version != "V3.2" && version != "V3.3") {
        throw std::invalid_argument("GFF JSON version must be V3.2 or V3.3.");
    }
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<gff3 type=\"" << escapeXmlAttr(gffTypeForXml(fileType))
        << "\" version=\"" << escapeXmlAttr(version) << "\">\n";
    writeGffJsonStructAsXml(out, root.at("root"), 1, false, "4294967295");
    out << "</gff3>\n";
    return out.str();
}

} // namespace neogff
