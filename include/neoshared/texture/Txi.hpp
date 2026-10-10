#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neoshared::texture {

enum class TxiDirectiveValueKind {
    Boolean,
    Integer,
    SignedShort,
    Float,
    Vector3,
    ResourceName,
    Enum,
    FloatList,
    Vector3List,
};

enum class TxiIssueSeverity {
    Info,
    Warning,
    Error,
};

struct TxiDirectiveInfo {
    std::string name;
    std::string category;
    TxiDirectiveValueKind valueKind = TxiDirectiveValueKind::Integer;
    std::string valueHint;
    std::string defaultValue;
    std::string description;
    std::string notes;
    std::vector<std::string> allowedValues;
};

struct TxiEntry {
    std::size_t lineNumber = 0;
    std::string key;
    std::string value;
    bool blankOrComment = false;
    bool listData = false;
    bool listTerminator = false;
};

struct TxiValidationIssue {
    TxiIssueSeverity severity = TxiIssueSeverity::Info;
    std::size_t lineNumber = 0;
    std::string key;
    std::string message;
};

enum class TxiAutocompleteKind {
    None,
    Directive,
    Value,
};

struct TxiAutocompleteResult {
    TxiAutocompleteKind kind = TxiAutocompleteKind::None;
    std::size_t replacementLength = 0;
    std::string directive;
    std::vector<std::string> suggestions;
};

// TXI is a mixed text/binary stream. The two native channel lists accept
// a counted little-endian float payload after the directive's newline. Raw
// spans include that payload, so editors can preserve it byte-for-byte.
struct TxiStreamLine {
    std::size_t begin{}, end{}, next{}, lineNumber{};
    std::string key, value;
    bool listData{}, binary{};
    std::size_t payloadBegin{};
    std::vector<float> binaryValues;
};
struct TxiTextView {
    std::string text;
    // One original text-line number per projected text line. Binary elements
    // refer to their header, not to newline bytes embedded in float payloads.
    std::vector<std::size_t> sourceLines;
};
std::vector<TxiStreamLine> scanTxiStream(std::string_view bytes);
TxiTextView txiTextView(std::string_view bytes);
// Remove only terminator/padding NULs outside counted binary payloads.
std::string trimTxiPadding(std::string bytes);

std::string txiDirectiveValueKindToString(TxiDirectiveValueKind kind);
std::string txiIssueSeverityToString(TxiIssueSeverity severity);
const std::vector<TxiDirectiveInfo>& txiDirectiveCatalog();
std::optional<TxiDirectiveInfo> findTxiDirective(const std::string& key);
std::vector<TxiEntry> parseTxiEntries(const std::string& txi);
std::vector<TxiValidationIssue> validateTxiText(const std::string& txi);
std::string txiValidationReport(const std::string& txi);
std::string txiCatalogReport(const std::optional<std::string>& key = std::nullopt);
std::string txiKeyReferenceText(bool includeDetails = false);
std::string txiKeyReferenceText(const std::string& filter);
TxiAutocompleteResult txiAutocomplete(std::string_view lineBeforeCaret,
                                      bool includeAllDirectives = false);
std::string txiDirectiveSignature(const std::string& key);
std::string txiDirectiveHint(const std::string& key);
std::string txiValueHint(const std::string& key, const std::string& value);

} // namespace neoshared::texture
