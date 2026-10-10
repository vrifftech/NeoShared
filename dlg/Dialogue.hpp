#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neotlk { class TlkLookup; struct KotorTlkContext; }
namespace neoshared::dlg {

// A read-only view of classic KotOR/KotOR II dialogue nodes. Binary I/O and
// localized-string decoding remain in neoshared::gff and neoshared::tlk.
enum class NodeKind { Entry, Reply };
enum class TextSource { None, Embedded, TalkTable };
// Serialized fields only. Native defaults, conditions and playback policy are
// interpreted by the client; absence is preserved by the shared codec.
struct Link {
    std::optional<std::uint32_t> index;
    std::string active, active2;
    std::optional<std::uint8_t> negate, negate2;
    std::optional<std::int32_t> logic;
    std::array<std::optional<std::int32_t>,5> parameters, parameters2;
    std::string parameterString, parameterString2;
};
struct CameraFields {
    std::optional<std::uint32_t> angle;
    std::optional<std::int32_t> id, videoEffect;
    std::optional<float> heightOffset, targetHeightOffset, fieldOfView;
    std::optional<std::uint16_t> animation;
};
struct FadeFields {
    std::optional<std::uint8_t> type;
    std::optional<std::array<float,3>> color;
    std::optional<float> delay, length;
};
struct Animation {
    std::string participant;
    std::optional<std::uint16_t> animation;
};
struct Line {
    NodeKind kind = NodeKind::Entry;
    std::size_t index = 0;
    std::string speaker;
    std::uint32_t strref = 0xffffffffu;
    std::string text; // UTF-8; never a stringified GFF field or a StrRef number.
    TextSource textSource = TextSource::None;
    std::string voiceResRef; // VO_ResRef: lip resource name; may differ from Sound.
    std::string soundResRef;
    std::optional<bool> soundExists; // Metadata, not proof that an audio file exists.
    std::string textProblem;
    bool malformed = false; // Invalid field types are not unvoiced nodes.
    std::string listener, script, script2;
    std::optional<std::uint32_t> delay, waitFlags;
    std::optional<std::int32_t> nodeUnskippable;
    std::optional<std::uint8_t> soundFlags;
    CameraFields camera;
    FadeFields fade;
    std::vector<Link> links;
    std::optional<std::size_t> rawTextByteCount, rawTrimmedTextByteCount;
    std::optional<float> textSoundLength;
    std::optional<std::int32_t> alienRaceNode, emotion, facialAnimation;
    std::vector<Animation> animations;
    std::string label() const;
};
struct ReadOptions {
    const neotlk::TlkLookup* talkTable = nullptr;
    std::uint32_t language = 0;
    std::uint32_t gender = 0;
    std::function<void()> checkpoint;
    // Optional explicit ordered/gendered lookup. Without it, talkTable is the
    // single candidate supplied for the requested variant; no tables are guessed.
    const neotlk::KotorTlkContext* talkTables = nullptr;
    // Existing line-enumeration clients need not interpret graph/camera fields.
    bool runtimeFields = false;
    // Preserve an explicitly selected blank substring and its timing metadata.
    // Transcript clients keep their existing nonblank-text preference.
    bool preserveBlankText = false;
};
struct Dialogue {
    std::filesystem::path source;
    std::vector<Line> lines;
    std::vector<Link> startingLinks;
    std::string cameraModel;
    std::optional<std::uint32_t> delayEntry, delayReply;
    std::optional<std::int32_t> conversationType;
    std::optional<std::int32_t> alienRaceOwner;
    std::optional<std::uint8_t> skippable, oldHitCheck;
};
// Enumerates the physical EntryList and ReplyList once. It deliberately does
// not execute scripts, follow dialogue links, or predict a player's branch.
Dialogue read(const std::filesystem::path& path, const ReadOptions& options = {});
Dialogue readBytes(const std::vector<std::uint8_t>& bytes, const ReadOptions& options = {});
} // namespace neoshared::dlg
