#pragma once

#include <cstdint>
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
};
struct Dialogue {
    std::filesystem::path source;
    std::vector<Line> lines;
};
// Enumerates the physical EntryList and ReplyList once. It deliberately does
// not execute scripts, follow dialogue links, or predict a player's branch.
Dialogue read(const std::filesystem::path& path, const ReadOptions& options = {});
} // namespace neoshared::dlg
