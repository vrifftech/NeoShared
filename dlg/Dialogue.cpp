#include "neoshared/PathUtf8.hpp"
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
    std::optional<std::size_t> localBytes, localTrimmedBytes;
    for (const auto& sub : localized->substrings) if (sub.stringid >= 0 && static_cast<std::uint32_t>(sub.stringid) == id) {
        if (local) throw GffError("Duplicate DLG localized substring");
        const auto bytes=sub.GetString();
        localBytes=bytes.size();
        const auto first=bytes.find_first_not_of(' '),last=bytes.find_last_not_of(' ');
        localTrimmedBytes=first==std::string::npos?0u:last-first+1u;
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
    if (local && (options.preserveBlankText || notBlank(*local))) {
        line.text = *local; line.textSource = TextSource::Embedded;
        line.rawTextByteCount=localBytes;line.rawTrimmedTextByteCount=localTrimmedBytes;
        if (external && notBlank(*external) && *external != *local)
            line.textProblem = "Embedded text and TLK text differ; confirm the words actually recorded";
    } else if (external && (options.preserveBlankText || notBlank(*external))) {
        line.text = *external; line.textSource = TextSource::TalkTable;
        line.rawTextByteCount=resolution.rawTextByteCount;
        line.rawTrimmedTextByteCount=resolution.rawTrimmedTextByteCount;line.textSoundLength=resolution.soundLength;
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
// CResGFF native runtime reads use their supplied default when the field is
// absent or has another serialized type. The core supplies native defaults.
template<class Field>
std::string runtimeString(const GffStruct& object,const char* label) {
    const auto* value=dynamic_cast<const Field*>(field(object,label));
    return value?value->GetString():std::string{};
}
template<class Field>
auto numericField(const GffStruct& object, const char* label)
    -> std::optional<decltype(Field{}.value)> {
    const auto* raw=field(object,label);
    if (!raw) return {};
    const auto* value=dynamic_cast<const Field*>(raw);
    if (!value) return {};
    return value->value;
}
std::vector<Link> readLinks(const GffStruct& object, const char* label, const ReadOptions& options) {
    std::vector<Link> result;
    const auto* raw=field(object,label);
    if (!raw) return result;
    const auto* list=dynamic_cast<const GffList*>(raw);
    if (!list) return result;
    for (std::size_t i=0;i<list->count();++i) {
        if (options.checkpoint) options.checkpoint();
        const auto& item=*list->GetStruct(i);
        Link link;
        link.index=numericField<GffUInt32Field>(item,"Index");
        link.active=runtimeString<GffResRefField>(item,"Active");link.active2=runtimeString<GffResRefField>(item,"Active2");
        link.negate=numericField<GffByteField>(item,"Not");link.negate2=numericField<GffByteField>(item,"Not2");
        link.logic=numericField<GffIntField>(item,"Logic");
        for (std::size_t n=0;n<5;++n) {
            const auto name="Param"+std::to_string(n+1);
            link.parameters[n]=numericField<GffIntField>(item,name.c_str());
            link.parameters2[n]=numericField<GffIntField>(item,(name+"b").c_str());
        }
        link.parameterString=runtimeString<GffExoStringField>(item,"ParamStrA");link.parameterString2=runtimeString<GffExoStringField>(item,"ParamStrB");
        result.push_back(std::move(link));
    }
    return result;
}
void readRuntimeLine(Line& line, const GffStruct& node, const ReadOptions& options) {
    line.listener=decode(runtimeString<GffExoStringField>(node,"Listener"),options.language);
    line.script=runtimeString<GffResRefField>(node,"Script");line.script2=runtimeString<GffResRefField>(node,"Script2");
    line.delay=numericField<GffUInt32Field>(node,"Delay");
    line.waitFlags=numericField<GffUInt32Field>(node,"WaitFlags");
    line.nodeUnskippable=numericField<GffIntField>(node,"NodeUnskippable");
    line.soundFlags=numericField<GffByteField>(node,"SoundExists");
    line.alienRaceNode=numericField<GffIntField>(node,"AlienRaceNode");
    line.emotion=numericField<GffIntField>(node,"Emotion");
    line.facialAnimation=numericField<GffIntField>(node,"FacialAnim");
    line.fade.type=numericField<GffByteField>(node,"FadeType");
    line.fade.color=numericField<GffPositionField>(node,"FadeColor");
    line.fade.delay=numericField<GffFloatField>(node,"FadeDelay");
    line.fade.length=numericField<GffFloatField>(node,"FadeLength");
    auto& camera=line.camera;
    camera.angle=numericField<GffUInt32Field>(node,"CameraAngle");
    camera.id=numericField<GffIntField>(node,"CameraID");
    camera.heightOffset=numericField<GffFloatField>(node,"CamHeightOffset");
    camera.targetHeightOffset=numericField<GffFloatField>(node,"TarHeightOffset");
    camera.animation=numericField<GffWordField>(node,"CameraAnimation");
    camera.videoEffect=numericField<GffIntField>(node,"CamVidEffect");
    camera.fieldOfView=numericField<GffFloatField>(node,"CamFieldOfView");
    line.links=readLinks(node,line.kind==NodeKind::Entry?"RepliesList":"EntriesList",options);
    if (const auto* list=dynamic_cast<const GffList*>(field(node,"AnimList"))) {
        for (std::size_t i=0;i<list->count();++i) {
            if (options.checkpoint) options.checkpoint();
            const auto& item=*list->GetStruct(i);
            Animation animation;
            animation.participant=decode(runtimeString<GffExoStringField>(item,"Participant"),options.language);
            animation.animation=numericField<GffWordField>(item,"Animation");
            line.animations.push_back(std::move(animation));
        }
    }
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
            if (options.runtimeFields) readRuntimeLine(line,*node,options);
        } catch (const GffError& error) { line.malformed = true; line.textProblem = std::string("Invalid node: ") + error.what(); }
        catch (const std::invalid_argument& error) { line.malformed = true; line.textProblem = std::string("Invalid text: ") + error.what(); }
        result.lines.push_back(std::move(line));
    }
}
Dialogue readParsed(const GffFile& file, const ReadOptions& options) {
    if (file.filetype() != "DLG " || file.isGff4() || (file.version() != "V3.2" && file.version() != "V3.3"))
        throw GffError("Expected a classic KotOR/KotOR II DLG, not " + file.filetype() + file.version());
    if (!file.root() || (!field(*file.root(), "EntryList") && !field(*file.root(), "ReplyList")))
        throw GffError("The DLG does not contain EntryList or ReplyList");
    Dialogue result;
    append(result,*file.root(),"EntryList",NodeKind::Entry,options);
    append(result,*file.root(),"ReplyList",NodeKind::Reply,options);
    if (options.runtimeFields) {
        result.startingLinks=readLinks(*file.root(),"StartingList",options);
        result.cameraModel=runtimeString<GffResRefField>(*file.root(),"CameraModel");
        result.conversationType=numericField<GffIntField>(*file.root(),"ConversationType");
        result.alienRaceOwner=numericField<GffIntField>(*file.root(),"AlienRaceOwner");
        result.skippable=numericField<GffByteField>(*file.root(),"Skippable");
        result.oldHitCheck=numericField<GffByteField>(*file.root(),"OldHitCheck");
        result.delayEntry=numericField<GffUInt32Field>(*file.root(),"DelayEntry");
        result.delayReply=numericField<GffUInt32Field>(*file.root(),"DelayReply");
    }
    return result;
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
        throw GffError("Choose a readable DLG file (maximum 64 MiB): " + neoshared::pathToUtf8(path));
    std::array<char, 8> signature{};
    std::ifstream input(path, std::ios::binary); input.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    const std::string header(signature.data(), signature.size());
    if (!input || (header != "DLG V3.2" && header != "DLG V3.3"))
        throw GffError("Expected a classic KotOR/KotOR II DLG header");
    GffFile file; file.LoadFile(path);
    auto result=readParsed(file,options);result.source=path;
    return result;
}
Dialogue readBytes(const std::vector<std::uint8_t>& bytes, const ReadOptions& options) {
    if (options.gender > 1 || options.language > (std::numeric_limits<std::uint32_t>::max() - 1) / 2)
        throw std::invalid_argument("Invalid DLG language/variant");
    if (options.checkpoint) options.checkpoint();
    if (bytes.size()<56 || bytes.size()>64u*1024u*1024u)
        throw GffError("Choose a readable DLG file (maximum 64 MiB)");
    GffFile file;file.LoadBytes(bytes);
    if (options.checkpoint) options.checkpoint();
    return readParsed(file,options);
}
} // namespace neoshared::dlg
