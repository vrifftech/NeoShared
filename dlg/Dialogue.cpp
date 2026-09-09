#include "Dialogue.hpp"
#include "GFFFile.hpp"
#include <neotlk/TlkLookup.hpp>
#include <neotlk/TextEncoding.hpp>
#include <limits>
#include <fstream>
#include <array>
#include <stdexcept>

namespace neoshared::dlg {
namespace {
using namespace neogff;
const GffField* field(const GffStruct& object, const std::string& label) {
    const GffField* result = nullptr;
    for (const auto& item : object.allFields()) if (item && item->GetLabel() == label) {
        if (result) throw GffError("Duplicate DLG field: " + label);
        result = item.get();
    }
    return result;
}
std::string stringField(const GffStruct& object, const char* label) {
    const auto* value = field(object, label);
    if (!value) return {};
    if (const auto* ref = dynamic_cast<const GffResRefField*>(value)) return ref->GetString();
    if (const auto* text = dynamic_cast<const GffExoStringField*>(value)) return text->GetString();
    throw GffError(std::string("DLG field must contain a string: ") + label);
}
std::string decode(const std::string& raw, std::uint32_t language) {
    const auto preferred = neotlk::detectClassicPreferredEncoding(language, false, {raw});
    return neotlk::decodeTextBytes(raw, neotlk::detectClassicEntryEncoding(raw, preferred, language));
}
bool notBlank(const std::string& text) { return text.find_first_not_of(" \r\n\t") != std::string::npos; }
void resolveText(Line& line, const GffStruct& object, const ReadOptions& options) {
    const auto* raw = field(object, "Text");
    if (!raw) { line.textProblem = "No dialogue text"; return; }
    const auto* localized = dynamic_cast<const GffLocalizedStringField*>(raw);
    if (!localized) throw GffError("KotOR DLG Text must be a CExoLocString");
    line.strref = localized->strref;
    const auto id = options.language * 2u + options.gender;
    std::optional<std::string> local;
    for (const auto& sub : localized->substrings) if (sub.stringid >= 0 && static_cast<std::uint32_t>(sub.stringid) == id) {
        if (local) throw GffError("Duplicate DLG localized substring");
        local = decode(sub.GetString(), options.language);
    }
    // The opposite-gender local text is not silently substituted: it may be a
    // different spoken line. The caller can choose a variant explicitly.
    std::optional<std::string> external;
    const auto* tlk = options.talkTable;
    neotlk::TlkResolution resolution;
    if (options.talkTables) {
        resolution = neotlk::resolveKotorText(*options.talkTables, line.strref, options.language,
            options.gender ? neotlk::TlkGender::Female : neotlk::TlkGender::Male);
        external = resolution.text;
    } else if (tlk && tlk->loaded() && tlk->languageId() == options.language) {
        neotlk::KotorTlkContext context;
        // The supplied single table is explicitly the requested variant.
        (options.gender ? context.femaleTables : context.maleTables).push_back(tlk);
        resolution = neotlk::resolveKotorText(context, line.strref, options.language,
            options.gender ? neotlk::TlkGender::Female : neotlk::TlkGender::Male);
        external = resolution.text;
    }
    if (local && local->find('\0') != std::string::npos) {
        line.textProblem = "Embedded dialogue text contains a NUL; confirm/correct the recorded words";
        return;
    }
    if (local && notBlank(*local)) {
        line.text = *local; line.textSource = TextSource::Embedded;
        if (external && notBlank(*external) && *external != *local)
            line.textProblem = "Embedded text and TLK text differ; confirm the words actually recorded";
    } else if (external && notBlank(*external)) {
        line.text = *external; line.textSource = TextSource::TalkTable;
    } else if (line.strref != 0xffffffffu) {
        if (options.talkTables) line.textProblem = (resolution.status == neotlk::TlkResolutionStatus::Resolved ? "Empty TLK text" : resolution.problem()) + " (StrRef " + std::to_string(line.strref) + ")";
        else if (!tlk || !tlk->loaded()) line.textProblem = "Choose dialog.tlk to resolve StrRef " + std::to_string(line.strref);
        else if (tlk->languageId() != options.language) line.textProblem = "The selected TLK is not in the requested language";
        else line.textProblem = (resolution.status == neotlk::TlkResolutionStatus::Resolved ? "Empty TLK text" : resolution.problem()) +
            " (StrRef " + std::to_string(line.strref) + ")";
    } else if (!localized->substrings.empty()) {
        line.textProblem = "No embedded text for the selected language/variant";
    } else line.textProblem = "No dialogue text";
}
void append(Dialogue& result, const GffStruct& root, const char* label, NodeKind kind, const ReadOptions& options) {
    const auto* value = field(root, label);
    if (!value) return;
    const auto* list = dynamic_cast<const GffList*>(value);
    if (!list) throw GffError(std::string("DLG field must be a list: ") + label);
    if (list->count() > 100000) throw GffError("DLG node count exceeds the reader limit");
    for (std::size_t i = 0; i < list->count(); ++i) {
        if (options.checkpoint) options.checkpoint();
        const auto* node = list->GetStruct(i);
        if (!node) throw GffError("DLG contains an empty node");
        Line line; line.kind = kind; line.index = i;
        try {
            line.speaker = decode(stringField(*node, "Speaker"), options.language);
            line.voiceResRef = stringField(*node, "VO_ResRef");
            line.soundResRef = stringField(*node, "Sound");
            if (const auto* flag = field(*node, "SoundExists")) {
                if (const auto* byte = dynamic_cast<const GffByteField*>(flag)) line.soundExists = byte->value != 0;
                else if (const auto* integer = dynamic_cast<const GffUInt32Field*>(flag)) line.soundExists = integer->value != 0;
                else if (const auto* integer = dynamic_cast<const GffIntField*>(flag)) line.soundExists = integer->value != 0;
                else throw GffError("Invalid SoundExists field type");
            }
            resolveText(line, *node, options);
        } catch (const GffError& error) { line.malformed = true; line.textProblem = std::string("Invalid node: ") + error.what(); }
        catch (const std::invalid_argument& error) { line.malformed = true; line.textProblem = std::string("Invalid text: ") + error.what(); }
        result.lines.push_back(std::move(line));
    }
}
} // namespace
std::string Line::label() const { return (kind == NodeKind::Entry ? "Entry " : "Reply ") + std::to_string(index); }
Dialogue read(const std::filesystem::path& path, const ReadOptions& options) {
    if (options.gender > 1 || options.language > (std::numeric_limits<std::uint32_t>::max() - 1) / 2)
        throw std::invalid_argument("Invalid DLG language/variant");
    if (options.checkpoint) options.checkpoint();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 56 || size > 64 * 1024 * 1024)
        throw GffError("Choose a readable DLG file (maximum 64 MiB): " + path.u8string());
    std::array<char, 8> signature{};
    std::ifstream input(path, std::ios::binary); input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    const std::string header(signature.data(), signature.size());
    if (!input || (header != "DLG V3.2" && header != "DLG V3.3"))
        throw GffError("Expected a classic KotOR/KotOR II DLG header");
    GffFile file; file.LoadFile(path);
    if (file.filetype() != "DLG " || file.isGff4() || (file.version() != "V3.2" && file.version() != "V3.3"))
        throw GffError("Expected a classic KotOR/KotOR II DLG, not " + file.filetype() + file.version());
    if (!file.root() || (!field(*file.root(), "EntryList") && !field(*file.root(), "ReplyList")))
        throw GffError("The DLG does not contain EntryList or ReplyList");
    Dialogue result; result.source = path;
    append(result, *file.root(), "EntryList", NodeKind::Entry, options);
    append(result, *file.root(), "ReplyList", NodeKind::Reply, options);
    return result;
}
} // namespace neoshared::dlg
