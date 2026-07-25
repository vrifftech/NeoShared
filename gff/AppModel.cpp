#include "AppModel.hpp"

#include "GffXml.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace neogff {
namespace {

std::string joinPath(const std::string& parent, const std::string& child) {
    if (parent.empty()) return child;
    if (child.empty()) return parent;
    return parent + "\\" + child;
}

std::string displayLabel(const std::string& label) {
    return label.empty() ? std::string("(empty)") : label;
}

std::string lowerAsciiLocal(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return s;
}

std::string trim(std::string s) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string normalizeGffFileType(std::string type) {
    type = trim(type);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (type.size() == 3) type.push_back(' ');
    if (type.empty()) type = "UTC ";
    if (type.size() > 4) type.resize(4);
    return type;
}

std::vector<std::string> splitNonEmpty(const std::string& text, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(delimiter, start);
        const std::size_t stop = (end == std::string::npos) ? text.size() : end;
        if (stop > start) out.emplace_back(trim(text.substr(start, stop - start)));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

template <typename T>
T parseIntegral(const std::string& text, const char* what) {
    static_assert(std::is_integral<T>::value, "integral type required");
    if (text.empty()) throw std::invalid_argument(std::string("Missing ") + what + " value.");
    T value{};
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value, 10);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw std::invalid_argument("Invalid " + std::string(what) + " value: " + text);
    }
    return value;
}

template <typename T>
T parseBounded(const std::string& text, T minValue, T maxValue, const char* what) {
    if constexpr (std::is_signed<T>::value) {
        const auto value = parseIntegral<long long>(text, what);
        if (value < static_cast<long long>(minValue) || value > static_cast<long long>(maxValue)) {
            throw std::out_of_range(std::string(what) + " value out of range: " + text);
        }
        return static_cast<T>(value);
    } else {
        const auto value = parseIntegral<unsigned long long>(text, what);
        if (value > static_cast<unsigned long long>(maxValue)) {
            throw std::out_of_range(std::string(what) + " value out of range: " + text);
        }
        return static_cast<T>(value);
    }
}

float parseFloatStrict(const std::string& text) {
    return ParseFloatDecimal(text);
}

double parseDoubleStrict(const std::string& text) {
    return ParseDoubleDecimal(text);
}

std::uint32_t parseLocStrRef(const std::string& value) {
    if (value.empty() || value == "-1") return 0xFFFFFFFFu;
    return ParseUInt32Decimal(value);
}

ByteBuffer parseVoidBytes(const std::string& value) {
    ByteBuffer out;
    std::string compact;
    compact.reserve(value.size());
    for (char ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',' || ch == '-') continue;
        compact.push_back(ch);
    }
    if (compact.empty()) return out;
    if (compact.size() % 2 != 0) {
        throw std::invalid_argument("VOID data must be an even-length hexadecimal byte string.");
    }
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        unsigned int byte = 0;
        const auto result = std::from_chars(compact.data() + i, compact.data() + i + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != compact.data() + i + 2 || byte > 0xFFu) {
            throw std::invalid_argument("Invalid VOID hex byte near: " + compact.substr(i, 2));
        }
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return out;
}

std::string voidBytesToHex(const ByteBuffer& data) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::uint8_t byte : data) {
        out.push_back(hex[(byte >> 4) & 0x0F]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

std::string locStringValueSummary(const GffLocalizedStringField& loc) {
    std::string out = "strref=";
    out += (loc.strref == 0xFFFFFFFFu) ? "-1" : std::to_string(loc.strref);
    out += ", strings=" + std::to_string(loc.substrings.size());
    return out;
}

std::string strrefDisplayValue(std::uint32_t value) {
    return value == 0xFFFFFFFFu ? std::string("-1") : std::to_string(value);
}

std::string resolvedText(const neotlk::TlkLookup* tlk, std::uint32_t strref) {
    if (tlk == nullptr) return {};
    const auto text = tlk->resolve(strref);
    return text ? *text : std::string{};
}

bool labelSuggestsStrRef(const std::string& label) {
    const std::string key = lowerAsciiLocal(label);
    return key.find("strref") != std::string::npos ||
           key.find("stringref") != std::string::npos ||
           key == "tlk" || key == "text" || key == "name" || key == "description";
}

void appendRowsForStruct(std::vector<GffFieldRow>& out, const GffStruct& structure, const std::string& path, const neotlk::TlkLookup* tlk);

void appendRowsForField(std::vector<GffFieldRow>& out, const GffField& field, const std::string& path, const neotlk::TlkLookup* tlk) {
    const std::uint32_t type = field.fieldtype;
    GffFieldRow row;
    row.path = path;
    row.label = displayLabel(field.GetLabel());
    row.type = fieldTypeName(type);
    row.editable = isEditableFieldType(type);
    row.deletable = !path.empty();

    if (type == FIELD_TYPE_STRUCT) {
        const auto& structure = dynamic_cast<const GffStruct&>(field);
        row.value = "typeid=" + std::to_string(structure.typeid_) + ", fields=" + std::to_string(structure.count());
        row.editable = false;
        out.push_back(row);
        appendRowsForStruct(out, structure, path, tlk);
        return;
    }

    if (type == FIELD_TYPE_LIST) {
        const auto& list = dynamic_cast<const GffList&>(field);
        if (list.gff4CompactPrimitiveList) {
            row.value = "primitive items=" + std::to_string(list.gff4PrimitiveListCount) +
                        ", item bytes=" + std::to_string(list.gff4PrimitiveItemSize) +
                        ", raw bytes=" + std::to_string(list.gff4PrimitiveListData.size());
            row.editable = false;
            out.push_back(row);
            return;
        }
        row.value = "structs=" + std::to_string(list.count());
        row.editable = false;
        out.push_back(row);
        for (std::size_t i = 0; i < list.count(); ++i) {
            const GffStruct* structure = list.GetStruct(i);
            if (!structure) continue;
            const std::string structPath = joinPath(path, std::to_string(i));
            out.push_back(GffFieldRow{structPath,
                                           "[" + std::to_string(i) + "]",
                                           "Struct",
                                           "typeid=" + std::to_string(structure->typeid_) + ", fields=" + std::to_string(structure->count()),
                                           {},
                                           false,
                                           true});
            appendRowsForStruct(out, *structure, structPath, tlk);
        }
        return;
    }

    if (type == FIELD_TYPE_VOID) {
        const auto& voidField = dynamic_cast<const GffVoidField&>(field);
        row.value = voidBytesToHex(voidField.data);
        row.editable = false;
        out.push_back(row);
        return;
    }

    if (type == FIELD_TYPE_CEXOLOCSTRING) {
        const auto& loc = dynamic_cast<const GffLocalizedStringField&>(field);
        row.value = locStringValueSummary(loc);
        row.resolved = resolvedText(tlk, loc.strref);
        row.editable = false;
        out.push_back(row);
        out.push_back(GffFieldRow{path + "(strref)", row.label + " strref", "CExoLocString StrRef",
                                       strrefDisplayValue(loc.strref), resolvedText(tlk, loc.strref), true, false});
        for (const auto& sub : loc.substrings) {
            out.push_back(GffFieldRow{path + "(lang" + std::to_string(sub.stringid) + ")",
                                           row.label + " lang" + std::to_string(sub.stringid),
                                           "CExoLocString Text",
                                           sub.GetString(),
                                           {},
                                           true,
                                           false});
        }
        return;
    }

    row.value = field.GetString();
    if (tlk != nullptr) {
        if (type == FIELD_TYPE_JADE_STRREF) {
            const auto& jade = dynamic_cast<const GffJadeStringRefField&>(field);
            row.resolved = resolvedText(tlk, jade.strref);
        } else if ((type == FIELD_TYPE_INT || type == FIELD_TYPE_DWORD) && labelSuggestsStrRef(field.GetLabel())) {
            try {
                const auto strref = type == FIELD_TYPE_INT && row.value == "-1" ? 0xFFFFFFFFu : ParseUInt32Decimal(row.value);
                row.resolved = resolvedText(tlk, strref);
            } catch (const std::exception&) {
            }
        }
    }
    out.push_back(row);
}

void appendRowsForStruct(std::vector<GffFieldRow>& out, const GffStruct& structure, const std::string& path, const neotlk::TlkLookup* tlk) {
    for (const auto& fieldPtr : structure.allFields()) {
        if (!fieldPtr) continue;
        const std::string label = fieldPtr->GetLabel();
        const std::string childPath = joinPath(path, label);
        appendRowsForField(out, *fieldPtr, childPath, tlk);
    }
}

} // namespace

std::unique_ptr<GffField> createField(const std::string& label,
                                      const std::string& typeName,
                                      const std::string& value,
                                      std::uint32_t structTypeId) {
    const auto type = fieldTypeFromName(typeName);
    switch (type) {
    case FIELD_TYPE_BYTE:
        return std::make_unique<GffByteField>(label, parseBounded<std::uint8_t>(value.empty() ? "0" : value, 0, std::numeric_limits<std::uint8_t>::max(), "BYTE"));
    case FIELD_TYPE_CHAR:
        return std::make_unique<GffCharField>(label, value.empty() ? '\0' : value[0]);
    case FIELD_TYPE_WORD:
        return std::make_unique<GffWordField>(label, parseBounded<std::uint16_t>(value.empty() ? "0" : value, 0, std::numeric_limits<std::uint16_t>::max(), "WORD"));
    case FIELD_TYPE_SHORT:
        return std::make_unique<GffShortField>(label, parseBounded<std::int16_t>(value.empty() ? "0" : value, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), "SHORT"));
    case FIELD_TYPE_DWORD:
        return std::make_unique<GffUInt32Field>(label, value.empty() ? 0u : ParseUInt32Decimal(value));
    case FIELD_TYPE_INT:
        return std::make_unique<GffIntField>(label, value.empty() ? 0 : ParseInt32Decimal(value));
    case FIELD_TYPE_DWORD64:
        return std::make_unique<GffUInt64Field>(label, value.empty() ? 0ull : ParseUInt64Decimal(value));
    case FIELD_TYPE_INT64:
        return std::make_unique<GffInt64Field>(label, value.empty() ? 0ll : ParseInt64Decimal(value));
    case FIELD_TYPE_FLOAT:
        return std::make_unique<GffFloatField>(label, value.empty() ? 0.0f : parseFloatStrict(value));
    case FIELD_TYPE_DOUBLE:
        return std::make_unique<GffDoubleField>(label, value.empty() ? 0.0 : parseDoubleStrict(value));
    case FIELD_TYPE_CEXOSTRING:
        return std::make_unique<GffExoStringField>(label, value);
    case FIELD_TYPE_RESREF:
        return std::make_unique<GffResRefField>(label, value);
    case FIELD_TYPE_CEXOLOCSTRING:
        return std::make_unique<GffLocalizedStringField>(label, parseLocStrRef(value));
    case FIELD_TYPE_VOID:
        return std::make_unique<GffVoidField>(label, parseVoidBytes(value));
    case FIELD_TYPE_STRUCT: {
        auto field = std::make_unique<GffStruct>(label);
        field->typeid_ = structTypeId;
        return field;
    }
    case FIELD_TYPE_LIST:
        return std::make_unique<GffList>(label);
    case FIELD_TYPE_ORIENTATION: {
        const auto parts = splitNonEmpty(value.empty() ? "0|0|0|0" : value, '|');
        if (parts.size() != 4) throw std::invalid_argument("Orientation requires four pipe-separated floats.");
        return std::make_unique<GffOrientationField>(label, parseFloatStrict(parts[0]), parseFloatStrict(parts[1]), parseFloatStrict(parts[2]), parseFloatStrict(parts[3]));
    }
    case FIELD_TYPE_POSITION: {
        const auto parts = splitNonEmpty(value.empty() ? "0|0|0" : value, '|');
        if (parts.size() != 3) throw std::invalid_argument("Position requires three pipe-separated floats.");
        return std::make_unique<GffPositionField>(label, parseFloatStrict(parts[0]), parseFloatStrict(parts[1]), parseFloatStrict(parts[2]));
    }
    case FIELD_TYPE_JADE_STRREF: {
        const auto parts = splitNonEmpty(value.empty() ? "4|-1" : value, '|');
        if (parts.size() != 2) throw std::invalid_argument("JadeStringRef requires two pipe-separated integer values: type|strref.");
        const UInt32 first = ParseUInt32Decimal(parts[0]);
        const UInt32 second = parts[1] == "-1" ? 0xFFFFFFFFu : ParseUInt32Decimal(parts[1]);
        return std::make_unique<GffJadeStringRefField>(label, first, second);
    }
    default:
        throw std::invalid_argument("Unsupported GFF field type: " + typeName);
    }
}

std::string defaultGffTypeForExtension(const std::filesystem::path& file) {
    std::string ext = lowerAsciiLocal(file.extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    if (ext == "gff") return "GFF ";
    if (ext == "utc") return "UTC ";
    if (ext == "utd") return "UTD ";
    if (ext == "ute") return "UTE ";
    if (ext == "uti") return "UTI ";
    if (ext == "utm") return "UTM ";
    if (ext == "utp") return "UTP ";
    if (ext == "uts") return "UTS ";
    if (ext == "utt") return "UTT ";
    if (ext == "utw") return "UTW ";
    if (ext == "jrl") return "JRL ";
    if (ext == "dlg") return "DLG ";
    if (ext == "are") return "ARE ";
    if (ext == "git") return "GIT ";
    if (ext == "ifo") return "IFO ";
    if (ext == "pth") return "PTH ";
    if (ext == "fac") return "FAC ";
    if (ext == "gui") return "GUI ";
    if (ext == "sto") return "STO ";
    if (ext == "cwa") return "CWA ";
    if (ext == "fsm") return "FSM ";
    if (ext == "qst" || ext == "qst2") return "QST ";
    if (ext == "cre") return "CRE ";
    if (ext == "pla") return "PLA ";
    if (ext == "trg") return "TRG ";
    if (ext == "bic") return "BIC ";
    if (ext == "btc") return "BTC ";
    if (ext == "bti") return "BTI ";
    if (ext == "itp") return "ITP ";
    if (ext == "btp") return "PLA ";
    if (ext == "btt") return "TRG ";
    return "UTC ";
}


namespace {

} // namespace

void GffModel::newFile(const std::string& fileType) {
    file_.NewFile(normalizeGffFileType(fileType.empty() ? "UTC" : fileType));
}

void GffModel::load(const std::filesystem::path& file) {
    file_.LoadFile(file);
}

void GffModel::save(const std::filesystem::path& file) {
    file_.SaveFile(file);
}

void GffModel::loadTlk(const std::filesystem::path& file) {
    tlk_.load(file);
}

void GffModel::clearTlk() {
    tlk_.clear();
}

std::vector<GffFieldRow> GffModel::rows() const {
    std::vector<GffFieldRow> out;
    if (!file_.loaded() || file_.root() == nullptr) return out;
    appendRowsForStruct(out, *file_.root(), "", tlk_.loaded() ? &tlk_ : nullptr);
    return out;
}

neotabular::Table GffModel::toTable() const {
    neotabular::Table table;
    table.columns = {"Path", "Label", "Type", "Editable", "Value", "Resolved"};
    for (const auto& row : rows()) {
        table.rows.push_back({row.path, row.label, row.type, row.editable ? "yes" : "no", row.value, row.resolved});
    }
    return table;
}

std::string GffModel::toXml() const {
    return ToGffXml(file_);
}

void GffModel::importXml(const std::string& xmlText) {
    LoadGffXml(file_, xmlText);
}

void GffModel::setValue(const std::string& path, const std::string& value) {
    if (!file_.ChangeFieldValue(path, value)) {
        throw std::invalid_argument("Unable to edit GFF value at path: " + path);
    }
}

void GffModel::addField(const std::string& parentPath,
                             const std::string& label,
                             const std::string& type,
                             const std::string& value,
                             std::uint32_t structTypeId) {
    std::string normalizedParent = parentPath;
    if (normalizedParent == "." || lowerAsciiLocal(normalizedParent) == "root") normalizedParent.clear();
    file_.AddField(createField(label, type, value, structTypeId), normalizedParent);
}

void GffModel::deleteField(const std::string& path) {
    if (path.find('(') != std::string::npos) {
        throw std::invalid_argument("Localized-string child rows cannot be deleted directly. Delete the parent CExoLocString field instead.");
    }
    file_.DeleteField(path);
}

} // namespace neogff
