#include "neoshared/erf/Archive.hpp"

#include "neoshared/erf/Utils.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <streambuf>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zlib.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace neoshared::erf {

class detail::ArchiveInputStream::Buffer final : public std::streambuf {
public:
    ~Buffer() override { close(); }

    bool open_file(const std::filesystem::path& filename) {
        close();
        last_error_.clear();
        if (file_.open(filename, std::ios::in | std::ios::binary) == nullptr) {
            last_error_ = "Unable to open archive file";
            return false;
        }
        kind_ = Kind::File;
        return true;
    }

    bool open_reader(std::uint64_t size, ArchiveRangeReader reader) {
        close();
        last_error_.clear();
        if (!reader) {
            last_error_ = "Archive range reader is unavailable";
            return false;
        }
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            last_error_ = "Archive is too large for this stream implementation";
            return false;
        }
        kind_ = Kind::Reader;
        reader_ = std::move(reader);
        reader_size_ = size;
        reader_position_ = 0;
        cache_start_ = 0;
        cache_size_ = 0;
        cache_.clear();
        return true;
    }

    bool is_open() const noexcept { return kind_ != Kind::Closed; }

    void close() noexcept {
        if (file_.is_open()) {
            try {
                file_.close();
            } catch (...) {
            }
        }
        kind_ = Kind::Closed;
        reader_ = {};
        reader_size_ = 0;
        reader_position_ = 0;
        cache_start_ = 0;
        cache_size_ = 0;
        cache_.clear();
    }

    void discard_cache() noexcept {
        if (kind_ != Kind::Reader) return;
        cache_start_ = 0;
        cache_size_ = 0;
        std::vector<char>().swap(cache_);
    }

    const std::string& last_error() const noexcept { return last_error_; }

protected:
    int_type underflow() override {
        if (kind_ == Kind::File) return file_.sgetc();
        if (kind_ != Kind::Reader || reader_position_ >= reader_size_) {
            return traits_type::eof();
        }
        if (!ensure_cache()) return traits_type::eof();
        const std::size_t offset = static_cast<std::size_t>(reader_position_ - cache_start_);
        return traits_type::to_int_type(cache_[offset]);
    }

    int_type uflow() override {
        if (kind_ == Kind::File) return file_.sbumpc();
        const int_type value = underflow();
        if (!traits_type::eq_int_type(value, traits_type::eof())) ++reader_position_;
        return value;
    }

    std::streamsize xsgetn(char* destination, std::streamsize count) override {
        if (count <= 0) return 0;
        if (kind_ == Kind::File) return file_.sgetn(destination, count);
        if (kind_ != Kind::Reader) return 0;

        std::streamsize total = 0;
        while (total < count && reader_position_ < reader_size_) {
            if (!ensure_cache()) break;
            const std::size_t cache_offset =
                static_cast<std::size_t>(reader_position_ - cache_start_);
            const std::size_t available = cache_size_ - cache_offset;
            const std::size_t requested = static_cast<std::size_t>(count - total);
            const std::size_t take = std::min(available, requested);
            std::memcpy(destination + total, cache_.data() + cache_offset, take);
            reader_position_ += static_cast<std::uint64_t>(take);
            total += static_cast<std::streamsize>(take);
        }
        return total;
    }

    int_type pbackfail(int_type value) override {
        if (kind_ == Kind::File) {
            return traits_type::eq_int_type(value, traits_type::eof())
                ? file_.sungetc()
                : file_.sputbackc(traits_type::to_char_type(value));
        }
        if (kind_ != Kind::Reader || reader_position_ == 0) return traits_type::eof();

        --reader_position_;
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            if (!ensure_cache()) {
                ++reader_position_;
                return traits_type::eof();
            }
            const std::size_t offset =
                static_cast<std::size_t>(reader_position_ - cache_start_);
            if (!traits_type::eq(cache_[offset], traits_type::to_char_type(value))) {
                ++reader_position_;
                return traits_type::eof();
            }
        }
        return traits_type::not_eof(value);
    }

    pos_type seekoff(off_type offset,
                     std::ios_base::seekdir direction,
                     std::ios_base::openmode which) override {
        if ((which & std::ios_base::in) == 0) return pos_type(off_type(-1));
        if (kind_ == Kind::File) return file_.pubseekoff(offset, direction, which);
        if (kind_ != Kind::Reader) return pos_type(off_type(-1));

        std::uint64_t base = 0;
        if (direction == std::ios_base::beg) {
            base = 0;
        } else if (direction == std::ios_base::cur) {
            base = reader_position_;
        } else if (direction == std::ios_base::end) {
            base = reader_size_;
        } else {
            return pos_type(off_type(-1));
        }

        std::uint64_t next = base;
        if (offset >= 0) {
            const auto positive = static_cast<std::uint64_t>(offset);
            if (positive > reader_size_ - std::min(base, reader_size_)) {
                return pos_type(off_type(-1));
            }
            next = base + positive;
        } else {
            const std::uint64_t magnitude =
                static_cast<std::uint64_t>(-(offset + 1)) + 1u;
            if (magnitude > base) return pos_type(off_type(-1));
            next = base - magnitude;
        }
        if (next > reader_size_) return pos_type(off_type(-1));
        reader_position_ = next;
        return pos_type(static_cast<off_type>(reader_position_));
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode which) override {
        const off_type offset = static_cast<off_type>(position);
        return seekoff(offset, std::ios_base::beg, which);
    }

    std::streamsize showmanyc() override {
        if (kind_ == Kind::File) return file_.in_avail();
        if (kind_ != Kind::Reader || reader_position_ >= reader_size_) return 0;
        const std::uint64_t remaining = reader_size_ - reader_position_;
        const auto maximum = static_cast<std::uint64_t>(
            std::numeric_limits<std::streamsize>::max());
        return static_cast<std::streamsize>(std::min(remaining, maximum));
    }

private:
    enum class Kind { Closed, File, Reader };
    static constexpr std::size_t kCacheBytes = 256u * 1024u;

    bool ensure_cache() {
        if (reader_position_ >= reader_size_) return false;
        if (cache_size_ != 0u && reader_position_ >= cache_start_ &&
            reader_position_ - cache_start_ < cache_size_) {
            return true;
        }

        const std::uint64_t remaining = reader_size_ - reader_position_;
        const std::size_t length = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, kCacheBytes));
        std::vector<std::uint8_t> bytes;
        std::string error;
        bool ok = false;
        try {
            ok = reader_(reader_position_, length, bytes, error);
        } catch (const std::exception& ex) {
            error = ex.what();
        } catch (...) {
            error = "Archive range reader raised an unknown exception";
        }
        if (!ok) {
            last_error_ = error.empty() ? "Archive range reader failed" : std::move(error);
            cache_size_ = 0;
            return false;
        }
        if (bytes.size() != length) {
            last_error_ = "Archive range reader returned " +
                std::to_string(bytes.size()) + " byte(s), expected " +
                std::to_string(length);
            cache_size_ = 0;
            return false;
        }

        cache_.assign(bytes.begin(), bytes.end());
        cache_start_ = reader_position_;
        cache_size_ = cache_.size();
        last_error_.clear();
        return true;
    }

    Kind kind_{Kind::Closed};
    std::filebuf file_;
    ArchiveRangeReader reader_;
    std::uint64_t reader_size_{};
    std::uint64_t reader_position_{};
    std::vector<char> cache_;
    std::uint64_t cache_start_{};
    std::size_t cache_size_{};
    std::string last_error_;
};

detail::ArchiveInputStream::ArchiveInputStream()
    : std::istream(nullptr) {
    clear(std::ios::badbit);
}

detail::ArchiveInputStream::~ArchiveInputStream() {
    close();
}

bool detail::ArchiveInputStream::open_file(const std::filesystem::path& filename) {
    close();
    auto next = std::make_unique<Buffer>();
    if (!next->open_file(filename)) {
        last_error_ = next->last_error();
        return false;
    }
    buffer_ = std::move(next);
    rdbuf(buffer_.get());
    clear();
    last_error_.clear();
    return true;
}

bool detail::ArchiveInputStream::open_reader(std::uint64_t size,
                                             ArchiveRangeReader reader) {
    close();
    auto next = std::make_unique<Buffer>();
    if (!next->open_reader(size, std::move(reader))) {
        last_error_ = next->last_error();
        return false;
    }
    buffer_ = std::move(next);
    rdbuf(buffer_.get());
    clear();
    last_error_.clear();
    return true;
}

bool detail::ArchiveInputStream::is_open() const noexcept {
    return buffer_ != nullptr && buffer_->is_open();
}

void detail::ArchiveInputStream::close() noexcept {
    rdbuf(nullptr);
    if (buffer_) buffer_->close();
    buffer_.reset();
    clear(std::ios::badbit);
}

void detail::ArchiveInputStream::discard_cache() noexcept {
    if (buffer_) buffer_->discard_cache();
}

const std::string& detail::ArchiveInputStream::last_error() const noexcept {
    if (buffer_ && !buffer_->last_error().empty()) return buffer_->last_error();
    return last_error_;
}

std::optional<ResourceNameProfile> resource_name_profile_for_game_id(
    std::string_view game_id) noexcept {
    if (game_id == "kotor" || game_id == "kotor2") {
        return ResourceNameProfile::KotOR;
    }
    if (game_id == "jade") return ResourceNameProfile::JadeEmpire;
    if (game_id == "nwn") return ResourceNameProfile::NeverwinterNights;
    if (game_id == "nwn2") return ResourceNameProfile::NeverwinterNights2;
    if (game_id == "witcher1") return ResourceNameProfile::Witcher;
    if (game_id == "dao") return ResourceNameProfile::DragonAgeOrigins;
    if (game_id == "da2") return ResourceNameProfile::DragonAge2;
    return std::nullopt;
}

namespace {

constexpr std::uint16_t kUnknownType = 0xFFFFu;
constexpr std::uint32_t kMaxSafeTableRecords = 1000000u;
constexpr std::uint32_t kMaxSafeLocalizedStringBytes = 64u * 1024u * 1024u;

struct ResourceTypeName {
    std::uint16_t type;
    const char* extension;
    bool primary;
};

constexpr ResourceTypeName kJadeResourceTypes[] = {
    {0x0000u, "res", true},
    {0x0001u, "bmp", true},
    {0x0002u, "mve", true},
    {0x0003u, "tga", true},
    {0x0004u, "wav", true},
    {0x0005u, "wfx", true},
    {0x0007u, "ini", true},
    {0x0008u, "mp3", true},
    {0x0009u, "mpg", true},
    {0x000Au, "txt", true},
    {0x000Bu, "pvd", true},
    {0x270Fu, "key", true},
    {0x270Eu, "bif", true},
    {0x270Du, "erf", true},
    {0x270Cu, "ids", true},
    {0x07D0u, "plh", true},
    {0x07D1u, "tex", true},
    {0x07D2u, "mdl", true},
    {0x07D3u, "thg", true},
    {0x07D5u, "fnt", true},
    {0x07D7u, "lua", true},
    {0x07D8u, "slt", true},
    {0x07D9u, "nss", true},
    {0x07DAu, "ncs", true},
    {0x07DBu, "mod", true},
    {0x07DCu, "are", true},
    {0x07DDu, "set", true},
    {0x07DEu, "ifo", true},
    {0x07E0u, "wok", true},
    {0x07E1u, "2da", true},
    {0x07E2u, "tlk", true},
    {0x07E6u, "txi", true},
    {0x07E7u, "git", true},
    {0x07E8u, "itm", true},
    {0x07EAu, "cre", true},
    {0x07EDu, "dlg", true},
    {0x07EEu, "pal", true},
    {0x07EFu, "trg", true},
    {0x07F1u, "dds", true},
    {0x07F2u, "snd", true},
    {0x07F4u, "ltr", true},
    {0x07F5u, "gff", true},
    {0x07F6u, "fac", true},
    {0x07F7u, "enc", true},
    {0x07F8u, "con", true},
    {0x07F9u, "dor", true},
    {0x07FBu, "pla", true},
    {0x07FDu, "dft", true},
    {0x07FEu, "gic", true},
    {0x07FFu, "gui", true},
    {0x0800u, "css", true},
    {0x0801u, "ccs", true},
    {0x0802u, "mer", true},
    {0x0804u, "dwk", true},
    {0x0805u, "pwk", true},
    {0x0806u, "gen", true},
    {0x0808u, "jrl", true},
    {0x0809u, "sav", true},
    {0x080Au, "way", true},
    {0x080Bu, "4pc", true},
    {0x080Cu, "ssf", true},
    {0x080Du, "hak", true},
    {0x080Eu, "nwm", true},
    {0x080Fu, "bik", true},
    {0x0810u, "min", true},
    {0x0811u, "ndb", true},
    {0x0812u, "xwb", true},
    {0x0813u, "xsb", true},
    {0x0814u, "bin", true},
    {0x0BB8u, "lyt", true},
    {0x0BB9u, "vis", true},
    {0x0BBAu, "rim", true},
    {0x0BBCu, "lip", true},
    {0x0BBDu, "xmv", true},
    {0x0BBEu, "wma", true},
    {0x0BBFu, "cwd", true},
    {0x0BC0u, "pro", true},
    {0x0BC1u, "aoe", true},
    {0x0BC2u, "mat", true},
    {0x0BC3u, "mab", true},
    {0x0BC4u, "qst", true},
    {0x0BC5u, "sto", true},
    {0x0BC6u, "apl", true},
    {0x0BC7u, "hex", true},
    {0x0BC8u, "mdx", true},
    {0x0BC9u, "txb", true},
    {0x0BCAu, "tpc", true},
    {0x0BCBu, "cam", true},
    {0x0BCCu, "spt", true},
    {0x0BCDu, "sty", true},
    {0x0BCEu, "fsm", true},
    {0x0BCFu, "art", true},
    {0x0BD0u, "amp", true},
    {0x0BD1u, "cwa", true},
    {0x0BD2u, "xls", true},
    {0x0BD3u, "spf", true},
    {0x0BD4u, "bip", true},
    {0x0BD5u, "abc", true},
    {0x0BD6u, "sbm", true},
    {0x0BD7u, "ttc", true},
    {0x0BD8u, "sac", true},
    {0xFFFFu, "", true},
};

constexpr ResourceTypeName kKotORResourceTypes[] = {
    {0x0000u, "res", true},
    {0x0001u, "bmp", true},
    {0x0002u, "mve", true},
    {0x0003u, "tga", true},
    {0x0004u, "wav", true},
    {0x0006u, "plt", true},
    {0x0007u, "ini", true},
    {0x0008u, "mp3", true},
    {0x0009u, "mpg", true},
    {0x000Au, "txt", true},
    {0x000Bu, "wma", true},
    {0x000Cu, "wmv", true},
    {0x000Du, "xmv", true},
    {0x000Eu, "log", true},
    {0x07D0u, "plh", true},
    {0x07D1u, "tex", true},
    {0x07D2u, "mdl", true},
    {0x07D3u, "thg", true},
    {0x07D5u, "fnt", true},
    {0x07D7u, "lua", true},
    {0x07D8u, "slt", true},
    {0x07D9u, "nss", true},
    {0x07DAu, "ncs", true},
    {0x07DBu, "mod", true},
    {0x07DCu, "are", true},
    {0x07DDu, "set", true},
    {0x07DEu, "ifo", true},
    {0x07DFu, "bic", true},
    {0x07E0u, "wok", true},
    {0x07E1u, "2da", true},
    {0x07E2u, "tlk", true},
    {0x07E6u, "txi", true},
    {0x07E7u, "git", true},
    {0x07E8u, "bti", true},
    {0x07E9u, "uti", true},
    {0x07EAu, "btc", true},
    {0x07EBu, "utc", true},
    {0x07EDu, "dlg", true},
    {0x07EEu, "itp", true},
    {0x07EFu, "btt", true},
    {0x07F0u, "utt", true},
    {0x07F1u, "dds", true},
    {0x07F2u, "bts", true},
    {0x07F3u, "uts", true},
    {0x07F4u, "ltr", true},
    {0x07F5u, "gff", true},
    {0x07F6u, "fac", true},
    {0x07F7u, "bte", true},
    {0x07F8u, "ute", true},
    {0x07F9u, "btd", true},
    {0x07FAu, "utd", true},
    {0x07FBu, "btp", true},
    {0x07FCu, "utp", true},
    {0x07FDu, "dft", true},
    {0x07FEu, "gic", true},
    {0x07FFu, "gui", true},
    {0x0800u, "css", true},
    {0x0801u, "ccs", true},
    {0x0802u, "btm", true},
    {0x0803u, "utm", true},
    {0x0804u, "dwk", true},
    {0x0805u, "pwk", true},
    {0x0806u, "btg", true},
    {0x0807u, "utg", true},
    {0x0808u, "jrl", true},
    {0x0809u, "sav", true},
    {0x080Au, "utw", true},
    {0x080Bu, "4pc", true},
    {0x080Cu, "ssf", true},
    {0x080Du, "hak", true},
    {0x080Eu, "nwm", true},
    {0x080Fu, "bik", true},
    {0x0BB8u, "lyt", true},
    {0x0BB9u, "vis", true},
    {0x0BBAu, "rim", true},
    {0x0BBBu, "pth", true},
    {0x0BBCu, "lip", true},
    {0x0BBDu, "bwm", true},
    {0x0BBEu, "txb", true},
    {0x0BBFu, "tpc", true},
    {0x0BC0u, "mdx", true},
    {0x0BC1u, "rsv", true},
    {0x0BC2u, "sig", true},
    {0x0BC3u, "xbx", true},
    {0x270Du, "erf", true},
    {0x270Eu, "bif", true},
    {0x270Fu, "key", true},
    {0xFFFFu, "", true},
    {0x080Fu, "mov", false},
};


constexpr ResourceTypeName kNeverwinterNights2ResourceTypes[] = {
    // NWN2 compiled terrain/resource payloads not present in the KotOR table.
    {0x0BDBu, "trx", true},
};

constexpr ResourceTypeName kWitcherResourceTypes[] = {
    // The Witcher 1 keeps the ERF V1 container shape but has a few KEY/BIF
    // resource-type differences. Entries not listed here fall back to the
    // BioWare/KotOR-family table.
    {0x000Bu, "xml", true},
    {0x000Cu, "wma", true},
    {0x000Du, "wmv", true},
    {0x0811u, "ptm", true},
    {0x0812u, "ptt", true},
    {0x0813u, "lng", true},
    // Witcher MOD archives use the BioWare-family NCS id (0x07DA). Keep
    // 0x0814 as a readable alias for installations that contain it, but
    // prefer 0x07DA when authoring new resources.
    {0x07DAu, "ncs", true},
    {0x0814u, "ncs", true},
    {0x0815u, "mmd", true},
    {0x0816u, "mdb", true},
    {0x0817u, "say", true},
    {0x0818u, "ttf", true},
    {0x0819u, "ttc", true},
    {0x081Au, "cut", true},
    {0x081Bu, "ka", true},
    {0x0832u, "w2strings", true},
    {0x0833u, "csv", true},
};

constexpr ResourceTypeName kDragonAgeResourceTypes[] = {
    // DAO ERF V2.0 stores filenames, not type ids. These ids are internal
    // presentation keys so CLI/UI extension lookup still works for insert,
    // delete, and display. Common GFF-based extensions reuse the historical
    // BioWare values where they are unambiguous; DAO-only extensions use a
    // private high range.
    {0x000Au, "txt", true},
    {0x000Bu, "xml", true},
    {0x000Cu, "wma", true},
    {0x000Du, "wmv", true},
    {0x07D9u, "nss", true},
    {0x07DAu, "ncs", true},
    {0x07DBu, "mod", true},
    {0x07DCu, "are", true},
    {0x07DEu, "ifo", true},
    {0x07E7u, "git", true},
    {0x07E9u, "uti", true},
    {0x07EBu, "utc", true},
    {0x07EDu, "dlg", true},
    {0x07F0u, "utt", true},
    {0x07F1u, "dds", true},
    {0x07F3u, "uts", true},
    {0x07F5u, "gff", true},
    {0x07F8u, "ute", true},
    {0x07FAu, "utd", true},
    {0x07FCu, "utp", true},
    {0x0808u, "jrl", true},
    {0x080Cu, "ssf", true},
    {0x9000u, "gda", true},
    {0x9001u, "map", true},
    {0x9002u, "cut", true},
    {0x9003u, "mor", true},
    {0x9004u, "mop", true},
    {0x9005u, "rim", true},
    {0x9006u, "erf", true},
    {0x9007u, "bic", true},
    {0x9008u, "bik", true},
    {0x9103u, "stg", true},
    {0x9104u, "plo", true},
    {0xFFFFu, "", true},
};

constexpr ResourceTypeName kDragonAge2ResourceTypes[] = {
    // DA2 ERF V3.0 stores FNV hashes instead of 16-bit resource type IDs.
    // These presentation IDs let the CLI/UI render and accept DA2 extensions
    // while the actual V3 writer computes/preserves FNV lookup hashes.
    {0x000Au, "txt", true},
    {0x000Bu, "xml", true},
    {0x000Cu, "wma", true},
    {0x000Du, "wmv", true},
    {0x07D9u, "nss", true},
    {0x07DAu, "ncs", true},
    {0x07DBu, "mod", true},
    {0x07DCu, "are", true},
    {0x07DEu, "ifo", true},
    {0x07E7u, "git", true},
    {0x07E9u, "uti", true},
    {0x07EBu, "utc", true},
    {0x07EDu, "dlg", true},
    {0x07F0u, "utt", true},
    {0x07F1u, "dds", true},
    {0x07F3u, "uts", true},
    {0x07F5u, "gff", true},
    {0x07F8u, "ute", true},
    {0x07FAu, "utd", true},
    {0x07FCu, "utp", true},
    {0x0803u, "utm", true},
    {0x0808u, "jrl", true},
    {0x080Cu, "ssf", true},
    {0x9000u, "gda", true},
    {0x9001u, "map", true},
    {0x9002u, "cut", true},
    {0x9003u, "mor", true},
    {0x9004u, "mop", true},
    {0x9005u, "rim", true},
    {0x9006u, "erf", true},
    {0x9007u, "bic", true},
    {0x9008u, "bik", true},
    {0x9100u, "cl", true},
    {0x9101u, "cnv", true},
    {0x9102u, "evt", true},
    {0x9103u, "stg", true},
    {0x9104u, "plo", true},
    {0x9105u, "msh", true},
    {0x9106u, "ani", true},
    {0x9107u, "mmh", true},
    {0x9108u, "mao", true},
    {0x9109u, "mat", true},
    {0x910Au, "lst", true},
    {0x910Bu, "ncc", true},
    {0x910Cu, "bnk", true},
    {0x910Du, "gad", true},
    {0x910Eu, "vlm", true},
    {0x910Fu, "dx11", true},
    {0x9110u, "gfx", true},
    {0x9111u, "rml", true},
    {0x9112u, "anb", true},
    {0x9113u, "cre", true},
    {0x9114u, "fxo", true},
    {0x9115u, "ldf", true},
    {0x9116u, "tnt", true},
    {0xFFFFu, "", true},
};

const ResourceTypeName* resourceTypeTable(ResourceNameProfile profile, std::size_t& count) noexcept {
    switch (profile) {
        case ResourceNameProfile::JadeEmpire:
            count = sizeof(kJadeResourceTypes) / sizeof(kJadeResourceTypes[0]);
            return kJadeResourceTypes;
        case ResourceNameProfile::NeverwinterNights2:
            count = sizeof(kNeverwinterNights2ResourceTypes) / sizeof(kNeverwinterNights2ResourceTypes[0]);
            return kNeverwinterNights2ResourceTypes;
        case ResourceNameProfile::Witcher:
            count = sizeof(kWitcherResourceTypes) / sizeof(kWitcherResourceTypes[0]);
            return kWitcherResourceTypes;
        case ResourceNameProfile::DragonAgeOrigins:
            count = sizeof(kDragonAgeResourceTypes) / sizeof(kDragonAgeResourceTypes[0]);
            return kDragonAgeResourceTypes;
        case ResourceNameProfile::DragonAge2:
            count = sizeof(kDragonAge2ResourceTypes) / sizeof(kDragonAge2ResourceTypes[0]);
            return kDragonAge2ResourceTypes;
        case ResourceNameProfile::NeverwinterNights:
        case ResourceNameProfile::KotOR:
            break;
    }
    count = sizeof(kKotORResourceTypes) / sizeof(kKotORResourceTypes[0]);
    return kKotORResourceTypes;
}


bool same_header(const std::array<char, 4>& value, const char* literal) {
    return std::memcmp(value.data(), literal, 4) == 0;
}

std::array<char, 4> make_header(const char* literal) {
    std::array<char, 4> out{{0, 0, 0, 0}};
    std::memcpy(out.data(), literal, 4);
    return out;
}

void read_exact(std::istream& in, char* data, std::streamsize size, const char* what) {
    in.read(data, size);
    if (!in) {
        std::string detail;
        if (auto* archive_stream = dynamic_cast<detail::ArchiveInputStream*>(&in)) {
            detail = archive_stream->last_error();
        }
        throw ErfError(std::string("Unexpected end of archive while reading ") + what +
                       (detail.empty() ? std::string(".") : std::string(": ") + detail));
    }
}

std::uint16_t read_u16(std::istream& in) {
    unsigned char b[2]{};
    read_exact(in, reinterpret_cast<char*>(b), 2, "16-bit field");
    return static_cast<std::uint16_t>(b[0] | (static_cast<std::uint16_t>(b[1]) << 8));
}

std::uint32_t read_u32(std::istream& in) {
    unsigned char b[4]{};
    read_exact(in, reinterpret_cast<char*>(b), 4, "32-bit field");
    return static_cast<std::uint32_t>(b[0]) |
           (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) |
           (static_cast<std::uint32_t>(b[3]) << 24);
}

std::int32_t read_i32(std::istream& in) {
    return static_cast<std::int32_t>(read_u32(in));
}

std::uint64_t read_u64(std::istream& in) {
    unsigned char b[8]{};
    read_exact(in, reinterpret_cast<char*>(b), 8, "64-bit field");
    return static_cast<std::uint64_t>(b[0]) |
           (static_cast<std::uint64_t>(b[1]) << 8) |
           (static_cast<std::uint64_t>(b[2]) << 16) |
           (static_cast<std::uint64_t>(b[3]) << 24) |
           (static_cast<std::uint64_t>(b[4]) << 32) |
           (static_cast<std::uint64_t>(b[5]) << 40) |
           (static_cast<std::uint64_t>(b[6]) << 48) |
           (static_cast<std::uint64_t>(b[7]) << 56);
}


void checked_direct_write(std::ostream& out, const char* data, std::streamsize size, const char* what) {
    out.write(data, size);
    if (!out) {
        throw ErfError(std::string("Failed to write ") + what + ".");
    }
}

void write_u16(std::ostream& out, std::uint16_t value) {
    const unsigned char b[2] = {
        static_cast<unsigned char>(value & 0xFFu),
        static_cast<unsigned char>((value >> 8) & 0xFFu)
    };
    checked_direct_write(out, reinterpret_cast<const char*>(b), 2, "16-bit field");
}

void write_u32(std::ostream& out, std::uint32_t value) {
    const unsigned char b[4] = {
        static_cast<unsigned char>(value & 0xFFu),
        static_cast<unsigned char>((value >> 8) & 0xFFu),
        static_cast<unsigned char>((value >> 16) & 0xFFu),
        static_cast<unsigned char>((value >> 24) & 0xFFu)
    };
    checked_direct_write(out, reinterpret_cast<const char*>(b), 4, "32-bit field");
}

void write_i32(std::ostream& out, std::int32_t value) {
    write_u32(out, static_cast<std::uint32_t>(value));
}

void write_u64(std::ostream& out, std::uint64_t value) {
    const unsigned char b[8] = {
        static_cast<unsigned char>(value & 0xFFu),
        static_cast<unsigned char>((value >> 8) & 0xFFu),
        static_cast<unsigned char>((value >> 16) & 0xFFu),
        static_cast<unsigned char>((value >> 24) & 0xFFu),
        static_cast<unsigned char>((value >> 32) & 0xFFu),
        static_cast<unsigned char>((value >> 40) & 0xFFu),
        static_cast<unsigned char>((value >> 48) & 0xFFu),
        static_cast<unsigned char>((value >> 56) & 0xFFu)
    };
    checked_direct_write(out, reinterpret_cast<const char*>(b), 8, "64-bit field");
}

template <std::size_t N>
std::array<char, N> read_char_array(std::istream& in) {
    std::array<char, N> out{};
    read_exact(in, out.data(), static_cast<std::streamsize>(N), "fixed character field");
    return out;
}


template <std::size_t N>
void write_char_array(std::ostream& out, const std::array<char, N>& value) {
    checked_direct_write(out, value.data(), static_cast<std::streamsize>(N), "fixed character field");
}

template <std::size_t N>
void write_byte_array(std::ostream& out, const std::array<std::uint8_t, N>& value) {
    checked_direct_write(out, reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(N), "fixed byte field");
}

template <std::size_t N>
void read_byte_array(std::istream& in, std::array<std::uint8_t, N>& value) {
    read_exact(in, reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(N), "fixed byte field");
}



bool utf16le_magic_equals(const std::array<char, 16>& magic, char major, char minor) {
    const std::array<unsigned char, 16> expected{{
        'E', 0, 'R', 0, 'F', 0, ' ', 0, 'V', 0,
        static_cast<unsigned char>(major), 0, '.', 0,
        static_cast<unsigned char>(minor), 0
    }};
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (static_cast<unsigned char>(magic[i]) != expected[i]) {
            return false;
        }
    }
    return true;
}

bool is_erf_v2_0_magic(const std::array<char, 16>& magic) {
    return utf16le_magic_equals(magic, '2', '0');
}

bool is_erf_v2_2_magic(const std::array<char, 16>& magic) {
    return utf16le_magic_equals(magic, '2', '2');
}

void write_erf_v2_magic(std::ostream& out, char minor) {
    const unsigned char kMagic[16] = {
        'E', 0, 'R', 0, 'F', 0, ' ', 0, 'V', 0, '2', 0, '.', 0,
        static_cast<unsigned char>(minor), 0
    };
    checked_direct_write(out, reinterpret_cast<const char*>(kMagic), 16,
                         minor == '2' ? "ERF V2.2 UTF-16LE magic" : "ERF V2.0 UTF-16LE magic");
}

bool is_erf_v3_magic(const std::array<char, 16>& magic) {
    return utf16le_magic_equals(magic, '3', '0');
}

void write_erf_v3_magic(std::ostream& out) {
    static constexpr unsigned char kMagic[16] = {
        'E', 0, 'R', 0, 'F', 0, ' ', 0, 'V', 0, '3', 0, '.', 0, '0', 0
    };
    checked_direct_write(out, reinterpret_cast<const char*>(kMagic), 16, "ERF V3.0 UTF-16LE magic");
}

void append_utf8_from_bmp(std::string& out, std::uint16_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string read_fixed_utf16le_string(std::istream& in, std::size_t code_units, const char* what) {
    std::string out;
    out.reserve(code_units);
    bool terminated = false;
    for (std::size_t i = 0; i < code_units; ++i) {
        const std::uint16_t cp = read_u16(in);
        if (cp == 0) {
            terminated = true;
            continue;
        }
        if (!terminated) {
            // DAO filenames in shipped ERFs are ASCII, but decode BMP code units
            // rather than discarding non-ASCII names. Surrogate pairs are not
            // expected in archive leaf names; replace a stray surrogate safely.
            if (cp >= 0xD800u && cp <= 0xDFFFu) {
                out.push_back('?');
            } else {
                append_utf8_from_bmp(out, cp);
            }
        }
    }
    if (out.empty()) {
        throw ErfError(std::string("Malformed archive: empty ") + what + ".");
    }
    return out;
}

std::vector<std::uint16_t> utf16_units_from_utf8_name(const std::string& text) {
    std::vector<std::uint16_t> units;
    units.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 0x80u) {
            units.push_back(static_cast<std::uint16_t>(ch));
            ++i;
        } else if ((ch & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            const unsigned char ch1 = static_cast<unsigned char>(text[i + 1]);
            if ((ch1 & 0xC0u) != 0x80u) {
                throw ErfError("Invalid UTF-8 in ERF V2.0 resource name: " + text);
            }
            units.push_back(static_cast<std::uint16_t>(((ch & 0x1Fu) << 6u) | (ch1 & 0x3Fu)));
            i += 2;
        } else if ((ch & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            const unsigned char ch1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char ch2 = static_cast<unsigned char>(text[i + 2]);
            if ((ch1 & 0xC0u) != 0x80u || (ch2 & 0xC0u) != 0x80u) {
                throw ErfError("Invalid UTF-8 in ERF V2.0 resource name: " + text);
            }
            const std::uint16_t cp = static_cast<std::uint16_t>(((ch & 0x0Fu) << 12u) |
                                                               ((ch1 & 0x3Fu) << 6u) |
                                                               (ch2 & 0x3Fu));
            if (cp >= 0xD800u && cp <= 0xDFFFu) {
                throw ErfError("UTF-8 surrogate code point is not valid in ERF V2.0 resource name: " + text);
            }
            units.push_back(cp);
            i += 3;
        } else {
            throw ErfError("ERF V2.0 resource names must fit in UTF-16 BMP code units: " + text);
        }
    }
    return units;
}

void write_fixed_utf16le_string(std::ostream& out, const std::string& text, std::size_t code_units, const char* what) {
    const auto units = utf16_units_from_utf8_name(text);
    if (units.empty()) {
        throw ErfError(std::string("Refusing to write empty ") + what + ".");
    }
    if (units.size() > code_units) {
        throw ErfError(std::string("ERF V2.0 ") + what + " exceeds the 32 UTF-16 code-unit filename field: " + text);
    }
    for (std::size_t i = 0; i < code_units; ++i) {
        write_u16(out, i < units.size() ? units[i] : 0u);
    }
}

std::uint16_t type_from_filename_extension_allow_unknown(const std::string& leaf, ResourceNameProfile profile) {
    const std::string ext_with_dot = extension_string(std::filesystem::path(leaf));
    if (ext_with_dot.empty()) {
        return kUnknownType;
    }
    try {
        return Resource::string_to_res_type(ext_with_dot, profile);
    } catch (const ErfError&) {
        if (profile == ResourceNameProfile::DragonAge2) {
            // Stripped DA2 entries with an unknown FNV32 type hash are exposed
            // as synthetic names such as hash_<fnv64>.#<fnv32>.  That decimal
            // FNV32 value is not a legacy 16-bit ResType and must not make the
            // archive unloadable.  Keep the actual v3_type_hash separately and
            // show the presentation TypeId as unknown.
            return kUnknownType;
        }
        throw;
    }
}

std::uint32_t fnv1_32_lower(std::string text) {
    std::uint32_t hash = 0x811C9DC5u;
    for (char raw : text) {
        unsigned char ch = static_cast<unsigned char>(raw);
        hash *= 0x01000193u;
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
        }
        hash ^= static_cast<std::uint32_t>(ch);
    }
    return hash;
}

std::uint64_t fnv1_64_lower(std::string text) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (char raw : text) {
        unsigned char ch = static_cast<unsigned char>(raw);
        hash *= 0x00000100000001B3ull;
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
        }
        hash ^= static_cast<std::uint64_t>(ch);
    }
    return hash;
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}


std::string extension_no_dot_from_name(const std::string& name) {
    std::string ext = extension_string(std::filesystem::path(name));
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    return ascii_lower(ext);
}

std::string da2_extension_from_type_hash(std::uint32_t type_hash) {
    std::size_t count = 0;
    const ResourceTypeName* table = resourceTypeTable(ResourceNameProfile::DragonAge2, count);
    for (std::size_t i = 0; i < count; ++i) {
        if (table[i].extension[0] == '\0') {
            continue;
        }
        if (fnv1_32_lower(table[i].extension) == type_hash) {
            return table[i].extension;
        }
    }
    return {};
}

std::uint16_t da2_restype_from_type_hash(std::uint32_t type_hash) {
    const std::string ext = da2_extension_from_type_hash(type_hash);
    if (ext.empty()) {
        return kUnknownType;
    }
    return Resource::string_to_res_type(ext, ResourceNameProfile::DragonAge2);
}

std::uint32_t da2_type_hash_from_name(const std::string& name, std::uint32_t fallback = 0) {
    std::string ext = extension_no_dot_from_name(name);
    if (ext.empty()) {
        return fallback;
    }
    if (ext.size() > 1 && ext[0] == '#') {
        const std::string digits = ext.substr(1);
        try {
            std::size_t consumed = 0;
            const unsigned long parsed = std::stoul(digits, &consumed, 0);
            if (consumed == digits.size()) {
                return static_cast<std::uint32_t>(parsed);
            }
        } catch (...) {
        }
    }
    return fnv1_32_lower(ext);
}

std::string da2_synthetic_resource_name(std::uint64_t name_hash, std::uint32_t type_hash) {
    std::string ext = da2_extension_from_type_hash(type_hash);
    if (ext.empty()) {
        ext = std::string("#") + std::to_string(type_hash);
    }
    return std::string("hash_") + hex_u64(name_hash) + "." + ext;
}

bool parse_hex_u64(std::string text, std::uint64_t& out) {
    if (text.empty() || text.size() > 16u) {
        return false;
    }
    std::uint64_t value = 0;
    for (char ch : text) {
        unsigned digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<unsigned>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = 10u + static_cast<unsigned>(ch - 'a');
        } else if (ch >= 'A' && ch <= 'F') {
            digit = 10u + static_cast<unsigned>(ch - 'A');
        } else {
            return false;
        }
        value = (value << 4u) | static_cast<std::uint64_t>(digit);
    }
    out = value;
    return true;
}

bool parse_da2_synthetic_resource_name(const std::string& name, std::uint64_t& name_hash, std::uint32_t& type_hash) {
    const std::string leaf = filename_string(std::filesystem::path(name));
    const std::string lower = ascii_lower(leaf);
    if (lower.rfind("hash_", 0) != 0) {
        return false;
    }
    const std::size_t dot = lower.find('.', 5);
    if (dot == std::string::npos || dot <= 5) {
        return false;
    }
    std::uint64_t parsed_name_hash = 0;
    if (!parse_hex_u64(lower.substr(5, dot - 5), parsed_name_hash)) {
        return false;
    }
    name_hash = parsed_name_hash;
    type_hash = da2_type_hash_from_name(lower, 0);
    return type_hash != 0;
}

std::string normalize_archive_leaf(std::string text) {
    text = filename_string(std::filesystem::path(std::move(text)));
    if (text.empty()) {
        throw ErfError("Invalid empty archive resource filename.");
    }
    return text;
}

std::string normalize_archive_path(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    while (!text.empty() && text.front() == '/') {
        text.erase(text.begin());
    }
    if (text.size() >= 2 && text[1] == ':') {
        throw ErfError("Archive resource filename must be relative: " + text);
    }

    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = text.find('/', start);
        std::string part = slash == std::string::npos ? text.substr(start) : text.substr(start, slash - start);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                throw ErfError("Archive resource filename may not contain '..': " + text);
            }
            parts.push_back(std::move(part));
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }

    if (parts.empty()) {
        throw ErfError("Invalid empty archive resource filename.");
    }
    std::string out = parts.front();
    for (std::size_t i = 1; i < parts.size(); ++i) {
        out += '/';
        out += parts[i];
    }
    return out;
}

std::string normalize_filename_resource_name(const std::string& resource_name, ArchiveDiskFormat disk_format) {
    if (disk_format == ArchiveDiskFormat::ErfV3_0) {
        return normalize_archive_path(resource_name);
    }
    return normalize_archive_leaf(resource_name);
}

std::string normalized_filename_resource_key(const std::string& resource_name, ArchiveDiskFormat disk_format) {
    return ascii_lower(normalize_filename_resource_name(resource_name, disk_format));
}

std::string resource_filename_for_archive(const Resource& res, ResourceNameProfile profile) {
    if (!res.filename.empty()) {
        return res.filename;
    }
    std::string out_name = res.resref;
    const std::string ext = Resource::res_type_to_string(res.restype, profile);
    if (!ext.empty()) {
        out_name += "." + ext;
    }
    return out_name;
}

void validate_erf_v2_resource_leaf(const std::string& leaf) {
    if (leaf.empty()) {
        throw ErfError("ERF V2.0 resource filename is empty.");
    }
    const auto units = utf16_units_from_utf8_name(leaf);
    if (units.empty() || units.size() > 32u) {
        throw ErfError("ERF V2.0 resource filename must be 1-32 UTF-16 code units: " + leaf);
    }
}

void validate_erf_v3_resource_name(const std::string& name) {
    const std::string normalized = normalize_archive_path(name);
    if (normalized != name) {
        throw ErfError("ERF V3.0 resource filename is not a normalized relative path: " + name);
    }
    if (name.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw ErfError("ERF V3.0 resource filename is too long: " + name);
    }
    for (char raw : name) {
        unsigned char ch = static_cast<unsigned char>(raw);
        if (ch == 0) {
            throw ErfError("ERF V3.0 resource filename contains a NUL byte.");
        }
        if (ch > 0x7Fu) {
            throw ErfError("ERF V3.0 resource filenames must be ASCII: " + name);
        }
    }
}

void ensure_unique_loaded_resource_filenames(const std::vector<Resource>& resources, ResourceNameProfile profile, ArchiveDiskFormat disk_format) {
    std::unordered_set<std::string> seen;
    seen.reserve(resources.size());
    for (const auto& res : resources) {
        const std::string name = resource_filename_for_archive(res, profile);
        if (!seen.insert(normalized_filename_resource_key(name, disk_format)).second) {
            throw ErfError("Malformed archive: duplicate resource filename " + name + ".");
        }
    }
}

void remember_unique_save_filename(std::unordered_set<std::string>& seen,
                                   const std::string& name,
                                   const char* context,
                                   ArchiveDiskFormat disk_format) {
    const std::string normalized = normalize_filename_resource_name(name, disk_format);
    if (disk_format == ArchiveDiskFormat::ErfV2_0 || disk_format == ArchiveDiskFormat::ErfV2_2) {
        validate_erf_v2_resource_leaf(normalized);
    } else if (disk_format == ArchiveDiskFormat::ErfV3_0) {
        validate_erf_v3_resource_name(normalized);
    }
    const std::string key = ascii_lower(normalized);
    if (!seen.insert(key).second) {
        throw ErfError(std::string("Refusing to save filename-keyed archive because multiple resources would produce the same filename for ") + context + ": " + normalized);
    }
}



bool is_valid_filetype(const std::array<char, 4>& filetype) {
    return same_header(filetype, "ERF ") ||
           same_header(filetype, "MOD ") ||
           same_header(filetype, "HAK ") ||
           same_header(filetype, "SAV ") ||
           same_header(filetype, "RIM ");
}

std::pair<std::uint16_t, std::uint16_t> current_erf_build_time() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
#if defined(_WIN32)
    std::tm local_tm{};
    localtime_s(&local_tm, &tt);
#else
    std::tm local_tm{};
    localtime_r(&tt, &local_tm);
#endif
    const auto year = static_cast<std::uint16_t>((local_tm.tm_year + 1900) - 1900);
    const auto day = static_cast<std::uint16_t>(local_tm.tm_yday + 1);
    return {year, day};
}



void seekg_from_u32_offset(std::istream& in, std::uint32_t offset) {
    // Data-safe policy: valid ERF/RIM archives use unsigned 32-bit offsets.
    // Malformed offsets must fail closed instead of reading from a previous
    // stream position and potentially feeding bad bytes into a subsequent save.
    in.clear();
    const auto original = in.tellg();
    in.clear();
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (!in || end < std::streampos(0) || static_cast<std::uint64_t>(offset) > static_cast<std::uint64_t>(end)) {
        in.clear();
        if (original != std::streampos(std::streamoff(-1))) {
            in.seekg(original);
        }
        throw ErfError("Invalid archive read offset.");
    }
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) {
        throw ErfError("Invalid archive read offset.");
    }
}

void seekp_from_u32_offset(std::ostream& out, std::uint32_t offset) {
    out.clear();
    out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!out) {
        throw ErfError("Failed to seek while writing archive.");
    }
}

std::uint32_t checked_u32(std::uintmax_t value, const std::string& what) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw ErfError(what + " exceeds the 32-bit archive format limit.");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t checked_add_u32(std::uint32_t lhs, std::uint32_t rhs, const std::string& what) {
    return checked_u32(static_cast<std::uintmax_t>(lhs) + static_cast<std::uintmax_t>(rhs), what);
}

std::uint32_t checked_mul_u32(std::uint32_t lhs, std::uint32_t rhs, const std::string& what) {
    return checked_u32(static_cast<std::uintmax_t>(lhs) * static_cast<std::uintmax_t>(rhs), what);
}

std::uint32_t align_u32(std::uint32_t value, std::uint32_t alignment, const std::string& what) {
    if (alignment == 0) {
        return value;
    }
    const std::uint32_t remainder = value % alignment;
    if (remainder == 0) {
        return value;
    }
    return checked_add_u32(value, alignment - remainder, what);
}

void write_zero_padding_to_alignment(std::ostream& out, std::uint32_t alignment) {
    if (alignment == 0) {
        return;
    }
    const auto pos = out.tellp();
    if (pos < std::streampos(0)) {
        throw ErfError("Failed to determine archive write position for alignment padding.");
    }
    const auto current = static_cast<std::uintmax_t>(pos);
    const std::uint32_t pad = static_cast<std::uint32_t>((alignment - (current % alignment)) % alignment);
    static constexpr char zeros[4] = {0, 0, 0, 0};
    if (pad != 0) {
        checked_direct_write(out, zeros, static_cast<std::streamsize>(pad), "alignment padding");
    }
}

constexpr std::uint32_t kErfCompressionBiowareZlib = 0x20000000u; // scheme 1 << 29

std::uint32_t erf_encryption_scheme(std::uint32_t flags) noexcept {
    return (flags >> 4u) & 0x0Fu;
}

std::uint32_t erf_compression_scheme(std::uint32_t flags) noexcept {
    return (flags >> 29u) & 0x07u;
}

void ensure_supported_erf_compression_flags(std::uint32_t flags, const char* version_label) {
    const auto encryption = erf_encryption_scheme(flags);
    if (encryption != 0) {
        throw ErfError(std::string(version_label) + " encrypted payloads are not supported by this build.");
    }
    const auto compression = erf_compression_scheme(flags);
    if (compression != 0 && compression != 1 && compression != 7) {
        throw ErfError(std::string(version_label) + " uses an unsupported compression scheme: " + std::to_string(compression));
    }
}

void ensure_supported_v3_flags(std::uint32_t flags) {
    ensure_supported_erf_compression_flags(flags, "ERF V3.0");
}

void ensure_rim_resid_fits_word(std::size_t count, const std::string& what) {
    if (count > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1u) {
        throw ErfError(what + " cannot be represented safely because RIM resource IDs are 16-bit.");
    }
}

std::uintmax_t checked_open_archive_stream_size(std::istream& stream, const std::filesystem::path& filename) {
    // Data-safe policy: size the exact stream that was opened for parsing.
    // Sizing the path before opening/locking leaves a TOCTOU window where the
    // file can be replaced or truncated between the filesystem size probe and
    // the stream used for metadata/payload reads.
    stream.clear();
    stream.seekg(0, std::ios::end);
    const auto end_pos = stream.tellg();
    if (!stream || end_pos < std::streampos(0)) {
        throw ErfError("Unable to size opened archive stream for validation: " + path_to_string(filename));
    }
    stream.clear();
    stream.seekg(0, std::ios::beg);
    if (!stream) {
        throw ErfError("Unable to rewind opened archive stream for validation: " + path_to_string(filename));
    }
    return static_cast<std::uintmax_t>(end_pos);
}

std::uintmax_t checked_table_bytes(std::uint32_t count, std::uintmax_t record_size, const char* table_name) {
    if (record_size != 0 && static_cast<std::uintmax_t>(count) > std::numeric_limits<std::uintmax_t>::max() / record_size) {
        throw ErfError(std::string("Malformed archive: ") + table_name + " table is too large.");
    }
    return static_cast<std::uintmax_t>(count) * record_size;
}

void ensure_safe_metadata_count(std::uint32_t count, const char* table_name) {
    if (count > kMaxSafeTableRecords) {
        throw ErfError(std::string("Malformed archive: ") + table_name + " count exceeds the safety limit.");
    }
}

void ensure_archive_extent(std::uint32_t offset, std::uintmax_t size, std::uintmax_t archive_size, const char* what) {
    const std::uintmax_t start = offset;
    if (start > archive_size || size > archive_size - start) {
        throw ErfError(std::string("Malformed archive: ") + what + " extends beyond the file.");
    }
}

struct ArchiveRange {
    std::uintmax_t start = 0;
    std::uintmax_t end = 0;
    const char* name = "archive section";
};

ArchiveRange make_archive_range(std::uint32_t offset, std::uintmax_t size,
                                std::uintmax_t archive_size, const char* name) {
    ensure_archive_extent(offset, size, archive_size, name);
    return ArchiveRange{static_cast<std::uintmax_t>(offset),
                        static_cast<std::uintmax_t>(offset) + size,
                        name};
}

bool ranges_overlap(const ArchiveRange& a, const ArchiveRange& b) {
    if (a.start == a.end || b.start == b.end) {
        return false;
    }
    return a.start < b.end && b.start < a.end;
}

void ensure_non_overlapping_archive_sections(const std::vector<ArchiveRange>& ranges) {
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        for (std::size_t j = i + 1; j < ranges.size(); ++j) {
            if (ranges_overlap(ranges[i], ranges[j])) {
                throw ErfError(std::string("Malformed archive: ") + ranges[i].name +
                               " overlaps " + ranges[j].name + ".");
            }
        }
    }
}

void ensure_payload_does_not_overlap_metadata(std::uint32_t data_offset, std::uint32_t data_size,
                                             const std::vector<ArchiveRange>& metadata_ranges,
                                             const char* payload_name) {
    if (data_size == 0) {
        return;
    }
    const ArchiveRange payload{static_cast<std::uintmax_t>(data_offset),
                               static_cast<std::uintmax_t>(data_offset) +
                                   static_cast<std::uintmax_t>(data_size),
                               payload_name};
    for (const auto& metadata : metadata_ranges) {
        if (ranges_overlap(payload, metadata)) {
            throw ErfError(std::string("Malformed archive: ") + payload_name +
                           " overlaps " + metadata.name + ".");
        }
    }
}



void ensure_unique_loaded_resource_keys(const std::vector<Resource>& resources, bool extended_resrefs) {
    std::unordered_set<std::string> seen;
    seen.reserve(resources.size());
    for (const auto& res : resources) {
        const std::string normalized = string_to_resref(res.resref, extended_resrefs);
        if (normalized.empty()) {
            throw ErfError("Malformed archive: resource key sanitizes to an empty ResRef.");
        }
        const std::string key = ascii_lower(normalized) + "#" + std::to_string(res.restype);
        if (!seen.insert(key).second) {
            throw ErfError("Malformed archive: duplicate resource key " + normalized + "." + Resource::res_type_to_string(res.restype) + ".");
        }
    }
}

std::filesystem::path default_temp_root() {
#if defined(_WIN32)
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    return std::filesystem::path(module).parent_path();
#else
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        return include_trailing_separator(cwd);
    }
    return std::filesystem::path(".");
#endif
}


#if defined(_WIN32)
void close_loaded_archive_lock(void*& handle) noexcept {
    if (handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
    }
}

void* open_loaded_archive_lock(const std::filesystem::path& filename) {
    // Create(..., fmOpenRead or fmShareDenyWrite) permits readers
    // but denies writers while the archive remains loaded. Keep an explicit
    // Win32 handle open beside the standard stream to preserve that behavior.
    HANDLE handle = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    return handle;
}

class ScopedWin32ShareHandle {
public:
    ScopedWin32ShareHandle() = default;
    explicit ScopedWin32ShareHandle(HANDLE handle) noexcept : handle_(handle) {}
    ScopedWin32ShareHandle(const ScopedWin32ShareHandle&) = delete;
    ScopedWin32ShareHandle& operator=(const ScopedWin32ShareHandle&) = delete;
    ScopedWin32ShareHandle(ScopedWin32ShareHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    ScopedWin32ShareHandle& operator=(ScopedWin32ShareHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~ScopedWin32ShareHandle() { close(); }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

    bool close() noexcept {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return true;
        }
        HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return CloseHandle(handle) != 0;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

ScopedWin32ShareHandle open_stream_share_lock(const std::filesystem::path& filename, DWORD share_mode, const char* context) {
    HANDLE handle = CreateFileW(filename.c_str(), GENERIC_READ, share_mode,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw ErfError(std::string("Unable to apply file-sharing mode for ") + context + ": " + path_to_string(filename));
    }
    return ScopedWin32ShareHandle(handle);
}

ScopedWin32ShareHandle open_read_deny_write_lock(const std::filesystem::path& filename) {
    // fmOpenRead or fmShareDenyWrite: allow readers, deny writers.
    return open_stream_share_lock(filename, FILE_SHARE_READ, "resource input");
}

enum class OutputCreateMode { Create, CreateNew };

class Win32OutputStreamBuf : public std::streambuf {
public:
    explicit Win32OutputStreamBuf(HANDLE handle) : handle_(handle) {
        setp(buffer_.data(), buffer_.data() + buffer_.size());
    }

    ~Win32OutputStreamBuf() override {
        sync();
    }

protected:
    int overflow(int ch) override {
        if (flush_buffer() != 0) {
            return traits_type::eof();
        }
        if (ch != traits_type::eof()) {
            char one = static_cast<char>(ch);
            if (!write_all(&one, 1)) {
                return traits_type::eof();
            }
        }
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize count) override {
        if (count <= 0) {
            return 0;
        }
        if (flush_buffer() != 0) {
            return 0;
        }
        return write_all(s, static_cast<std::size_t>(count)) ? count : 0;
    }

    int sync() override {
        return flush_buffer();
    }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
        if ((which & std::ios_base::out) == 0 || flush_buffer() != 0) {
            return pos_type(off_type(-1));
        }
        DWORD method = FILE_BEGIN;
        if (dir == std::ios_base::cur) {
            method = FILE_CURRENT;
        } else if (dir == std::ios_base::end) {
            method = FILE_END;
        }
        LARGE_INTEGER distance{};
        distance.QuadPart = static_cast<LONGLONG>(off);
        LARGE_INTEGER result{};
        if (!SetFilePointerEx(handle_, distance, &result, method)) {
            return pos_type(off_type(-1));
        }
        return pos_type(static_cast<off_type>(result.QuadPart));
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }

private:
    bool write_all(const char* data, std::size_t count) {
        std::size_t written_total = 0;
        while (written_total < count) {
            const auto remaining = count - written_total;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 0x7ffff000u));
            DWORD written = 0;
            if (!WriteFile(handle_, data + written_total, chunk, &written, nullptr) || written == 0) {
                return false;
            }
            written_total += written;
        }
        return true;
    }

    int flush_buffer() {
        const auto count = pptr() - pbase();
        if (count > 0) {
            if (!write_all(pbase(), static_cast<std::size_t>(count))) {
                return -1;
            }
            setp(buffer_.data(), buffer_.data() + buffer_.size());
        }
        return 0;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::array<char, 64 * 1024> buffer_{};
};
class ExclusiveOutputFile {
public:
    ExclusiveOutputFile(const std::filesystem::path& filename, OutputCreateMode mode, const char* context)
        : handle_(CreateFileW(filename.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              0,
                              nullptr,
                              mode == OutputCreateMode::CreateNew ? CREATE_NEW : CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr)),
          buffer_(handle_.get()),
          stream_(&buffer_) {
        if (!handle_.valid()) {
            throw ErfError(std::string("Unable to create ") + context + ": " + path_to_string(filename));
        }
    }

    std::ostream& stream() { return stream_; }

    FileIdentity identity() const {
        FileIdentity identity{};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle_.get(), &info)) {
            return identity;
        }
        identity.valid = true;
        identity.volume_serial = static_cast<std::uint32_t>(info.dwVolumeSerialNumber);
        identity.file_index_high = static_cast<std::uint32_t>(info.nFileIndexHigh);
        identity.file_index_low = static_cast<std::uint32_t>(info.nFileIndexLow);
        return identity;
    }

    void close() {
        stream_.flush();
        if (!stream_) {
            throw ErfError("Failed to flush output file.");
        }
#if defined(_WIN32)
        if (!FlushFileBuffers(handle_.get())) {
            throw ErfError("Failed to flush output file to disk.");
        }
#endif
        if (!handle_.close()) {
            throw ErfError("Failed to close output file.");
        }
    }

private:
    ScopedWin32ShareHandle handle_;
    Win32OutputStreamBuf buffer_;
    std::ostream stream_;
};
#else

#if !defined(__EMSCRIPTEN__)
bool fsync_retry(int fd) {
    for (;;) {
        if (::fsync(fd) == 0) {
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}
#endif

enum class OutputCreateMode { Create, CreateNew };
struct ScopedWin32ShareHandle {};
ScopedWin32ShareHandle open_read_deny_write_lock(const std::filesystem::path&) { return {}; }

class ScopedPosixFd {
public:
    explicit ScopedPosixFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedPosixFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    ScopedPosixFd(const ScopedPosixFd&) = delete;
    ScopedPosixFd& operator=(const ScopedPosixFd&) = delete;
    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    bool close() noexcept {
        if (fd_ < 0) {
            return true;
        }
        const int fd = fd_;
        fd_ = -1;
        return ::close(fd) == 0;
    }

private:
    int fd_ = -1;
};


int close_on_exec_flag() {
#ifdef O_CLOEXEC
    return O_CLOEXEC;
#else
    return 0;
#endif
}

class PosixOutputStreamBuf : public std::streambuf {
public:
    explicit PosixOutputStreamBuf(int fd) : fd_(fd) {
        setp(buffer_.data(), buffer_.data() + buffer_.size());
    }

    ~PosixOutputStreamBuf() override { sync(); }

protected:
    int overflow(int ch) override {
        if (flush_buffer() != 0) return traits_type::eof();
        if (ch != traits_type::eof()) {
            char one = static_cast<char>(ch);
            if (!write_all(&one, 1)) return traits_type::eof();
        }
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize count) override {
        if (count <= 0) return 0;
        if (flush_buffer() != 0) return 0;
        return write_all(s, static_cast<std::size_t>(count)) ? count : 0;
    }

    int sync() override { return flush_buffer(); }

    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override {
        if ((which & std::ios_base::out) == 0 || flush_buffer() != 0) {
            return pos_type(off_type(-1));
        }
        int whence = SEEK_SET;
        if (dir == std::ios_base::cur) whence = SEEK_CUR;
        else if (dir == std::ios_base::end) whence = SEEK_END;
        const off_t result = ::lseek(fd_, static_cast<off_t>(off), whence);
        if (result < 0) return pos_type(off_type(-1));
        return pos_type(static_cast<off_type>(result));
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }

private:
    bool write_all(const char* data, std::size_t count) {
        std::size_t written_total = 0;
        while (written_total < count) {
            const ssize_t written = ::write(fd_, data + written_total, count - written_total);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return false;
            }
            written_total += static_cast<std::size_t>(written);
        }
        return true;
    }

    int flush_buffer() {
        const auto count = pptr() - pbase();
        if (count > 0) {
            if (!write_all(pbase(), static_cast<std::size_t>(count))) return -1;
            setp(buffer_.data(), buffer_.data() + buffer_.size());
        }
        return 0;
    }

    int fd_ = -1;
    std::array<char, 64 * 1024> buffer_{};
};

class ExclusiveOutputFile {
public:
    ExclusiveOutputFile(const std::filesystem::path& filename, OutputCreateMode mode, const char* context)
        : fd_(::open(path_to_string(filename).c_str(),
                     O_WRONLY | O_CREAT | close_on_exec_flag() | (mode == OutputCreateMode::CreateNew ? O_EXCL : O_TRUNC),
                     0600)),
          buffer_(fd_.get()),
          stream_(&buffer_) {
        if (!fd_.valid()) {
            throw ErfError(std::string("Unable to create ") + context + ": " + path_to_string(filename));
        }
    }

    std::ostream& stream() { return stream_; }

    FileIdentity identity() const {
        FileIdentity identity{};
        struct stat st {};
        if (::fstat(fd_.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
            return identity;
        }
        identity.valid = true;
        identity.size = static_cast<std::uintmax_t>(st.st_size);
        identity.device = static_cast<std::uint64_t>(st.st_dev);
        identity.inode = static_cast<std::uint64_t>(st.st_ino);
        return identity;
    }

    void close() {
        stream_.flush();
        if (!stream_) {
            throw ErfError("Failed to flush output file.");
        }
#if !defined(__EMSCRIPTEN__)
        if (!fsync_retry(fd_.get())) {
            throw ErfError("Failed to flush output file to disk.");
        }
#endif
        if (!fd_.close()) {
            throw ErfError("Failed to close output file.");
        }
    }

private:
    ScopedPosixFd fd_;
    PosixOutputStreamBuf buffer_;
    std::ostream stream_;
};
#endif


void open_binary_input_stream(detail::ArchiveInputStream& stream, const std::filesystem::path& filename) {
    stream.open_file(filename);
}


std::uint16_t parse_archive_resource_type_number(const std::string& digits) {
    // Data-safe policy: the on-disk ResType field is 16-bit.  The old historic
    // helper could wrap large #<number> text through a WORD assignment, turning
    // e.g. #65536 into type 0 ("res").  That is not safe archive semantics for
    // user-supplied unknown resource types, so reject values that cannot be
    // represented exactly in the archive format.
    std::uint64_t value = 0;
    for (char raw : digits) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint16_t>::max() - digit) / 10u) {
            throw ErfError("Unknown resource type exceeds the 16-bit archive format limit: #" + digits);
        }
        value = value * 10u + digit;
    }
    return static_cast<std::uint16_t>(value);
}

template <std::size_t N>
std::string resref_text_from_loaded_key(const std::array<char, N>& raw) {
    // Load() reads the on-disk CResRef into a string and assigns it via
    // the ResRef property, not raw_resref. That invokes set_resref(), which filters
    // to the allowed character set and caps before filtering.
    return string_to_resref(std::string(raw.begin(), raw.end()), N == 32);
}

std::string read_sized_string(std::istream& in, std::uint32_t size) {
    // Read returns a byte count. The loader ignores that count
    // for localized string text, so short reads leave the pre-sized string in
    // place rather than raising at metadata-load time.
    // resize(String, ...) takes a signed Integer. oLocStr.size is a
    // DWORD, so values whose low 32-bit representation is negative reach
    // resize as an invalid negative length instead of allocating a huge
    // Cardinal-sized buffer. Preserve that deterministic malformed-archive
    // boundary before constructing the C++ string.
    if (static_cast<std::int32_t>(size) < 0) {
        throw ErfError("Invalid localized string size.");
    }
    if (size > kMaxSafeLocalizedStringBytes) {
        throw ErfError("Localized string size exceeds the safety limit.");
    }
    std::string text(size, '\0');
    if (size > 0) {
        in.read(text.data(), static_cast<std::streamsize>(size));
        if (in.gcount() < static_cast<std::streamsize>(size)) {
            in.clear();
        }
    }
    return text;
}


std::string save_staging_leaf_for_existing_resource(const Resource& res, ResourceNameProfile profile) {
    return resource_filename_for_archive(res, profile);
}

std::string save_staging_leaf(std::size_t index, const std::string& archive_name) {
    std::string leaf = filename_string(std::filesystem::path(archive_name));
    if (leaf.empty()) {
        leaf = "resource";
    }
    return std::string("__neoerf_stage_") + std::to_string(index) + "_" + leaf;
}

void remember_unique_save_staging_leaf(std::unordered_set<std::string>& seen, const std::string& leaf, const char* context) {
    if (leaf.empty()) {
        throw ErfError(std::string("Refusing to save archive because a ") + context + " would stage with an empty filename.");
    }
    const std::string key = ascii_lower(leaf);
    if (!seen.insert(key).second) {
        throw ErfError("Refusing to save archive because multiple resources would stage to the same temporary filename: " + leaf);
    }
}

void remember_unique_save_archive_key(std::unordered_set<std::string>& seen,
                                      const std::string& resref,
                                      std::uint16_t restype,
                                      bool extended,
                                      const char* context,
                                      ResourceNameProfile profile = ResourceNameProfile::KotOR) {
    const std::string normalized = string_to_resref(resref, extended);
    if (normalized.empty()) {
        throw ErfError(std::string("Refusing to save archive because a ") + context + " would produce an empty ResRef.");
    }
    const std::string key = ascii_lower(normalized) + "#" + std::to_string(restype);
    if (!seen.insert(key).second) {
        throw ErfError("Refusing to save archive because multiple resources would produce the same archive key: " + normalized + "." + Resource::res_type_to_string(restype, profile));
    }
}

void remember_unique_save_archive_key_for_leaf(std::unordered_set<std::string>& seen,
                                               const std::string& leaf,
                                               bool extended,
                                               const char* context,
                                               ResourceNameProfile profile) {
    const std::string ext_with_dot = extension_string(std::filesystem::path(leaf));
    const std::string name = resource_stem_from_text(leaf);
    std::string ext = ext_with_dot.empty() ? std::string() : ext_with_dot.substr(1);
    ext = ascii_lower(ext);
    const std::uint16_t type = Resource::string_to_res_type(ext, profile);
    remember_unique_save_archive_key(seen, name, type, extended, context, profile);
}

std::string strip_staged_marker_for_delete(std::string resref) {
    if (!resref.empty() && resref.back() == '*') {
        resref.pop_back();
    }
    return resref;
}

bool staged_logical_name_matches(const std::filesystem::path& staged_name,
                                 const std::string& target_resref,
                                 std::uint16_t target_type,
                                 bool extended_resrefs,
                                 ResourceNameProfile profile) {
    const std::string leaf = filename_string(staged_name);
    if (ascii_lower(leaf) == ascii_lower(target_resref + "." + Resource::res_type_to_string(target_type, profile))) {
        return true;
    }
    const std::string ext_with_dot = extension_string(std::filesystem::path(leaf));
    const std::string staged_ext = ascii_lower(ext_with_dot.empty() ? std::string() : ext_with_dot.substr(1));
    if (Resource::string_to_res_type(staged_ext, profile) != target_type) {
        return false;
    }
    const std::string staged_resref = string_to_resref(ascii_lower(resource_stem_from_text(leaf)), extended_resrefs);
    const std::string wanted_resref = string_to_resref(ascii_lower(target_resref), extended_resrefs);
    return !wanted_resref.empty() && staged_resref == wanted_resref;
}

void ensure_save_target_does_not_overwrite_staged_inputs(const std::filesystem::path& target,
                                                        const std::vector<std::filesystem::path>& staged_inputs) {
    for (const auto& staged_input : staged_inputs) {
        if (paths_refer_to_same_existing_file_or_location(target, staged_input)) {
            throw ErfError("Refusing to save archive over a staged resource input file: " + path_to_string(target));
        }
    }
}

void ensure_extract_target_does_not_overwrite_inputs(const std::filesystem::path& target,
                                                    const std::filesystem::path& loaded_archive,
                                                    const std::vector<std::filesystem::path>& staged_inputs) {
    if (!loaded_archive.empty() && paths_refer_to_same_existing_file_or_location(target, loaded_archive)) {
        throw ErfError("Refusing to extract a resource over the loaded archive file: " + path_to_string(target));
    }
    for (const auto& staged_input : staged_inputs) {
        if (paths_refer_to_same_existing_file_or_location(target, staged_input)) {
            throw ErfError("Refusing to extract a resource over a staged resource input file: " + path_to_string(target));
        }
    }
}

void ensure_unique_save_staging_leaves(const std::vector<Resource>& resources,
                                       const std::vector<std::filesystem::path>& new_files,
                                       const std::vector<std::string>& new_names,
                                       bool extended_resrefs,
                                       ResourceNameProfile profile,
                                       ArchiveDiskFormat disk_format) {
    (void)new_files;
    if (disk_format == ArchiveDiskFormat::ErfV2_0 || disk_format == ArchiveDiskFormat::ErfV2_2 || disk_format == ArchiveDiskFormat::ErfV3_0) {
        std::unordered_set<std::string> seen_names;
        seen_names.reserve(resources.size() + new_names.size());
        for (const auto& res : resources) {
            remember_unique_save_filename(seen_names, resource_filename_for_archive(res, profile), "loaded resource", disk_format);
        }
        for (const auto& name : new_names) {
            remember_unique_save_filename(seen_names, name, "staged resource", disk_format);
        }
        return;
    }

    std::unordered_set<std::string> seen_leaves;
    std::unordered_set<std::string> seen_archive_keys;
    seen_leaves.reserve(resources.size() + new_names.size());
    seen_archive_keys.reserve(resources.size() + new_names.size());
    for (const auto& res : resources) {
        const std::string leaf = save_staging_leaf_for_existing_resource(res, profile);
        remember_unique_save_staging_leaf(seen_leaves, leaf, "loaded resource");
        remember_unique_save_archive_key(seen_archive_keys, res.resref, res.restype, extended_resrefs, "loaded resource", profile);
    }
    for (std::size_t i = 0; i < new_files.size(); ++i) {
        const std::string leaf = filename_string(std::filesystem::path(new_names.at(i)));
        remember_unique_save_staging_leaf(seen_leaves, leaf, "staged resource");
        remember_unique_save_archive_key_for_leaf(seen_archive_keys, leaf, extended_resrefs, "staged resource", profile);
    }
}

std::vector<std::filesystem::path> temp_files_full_path(const std::filesystem::path& temp_folder) {
    // archive save has its own local GetFileList helper, and that helper
    // calls FindClose(rFile). Keep that behavior separate from the
    // global GetFilesInFolder helper, which intentionally omits
    // FindClose.
    return files_in_folder(temp_folder, false, false, true);
}




void ensure_staged_input_fits_archive_format(const std::filesystem::path& source) {
    std::uintmax_t size = 0;
    try {
        size = regular_file_size_after_open(source);
    } catch (const std::exception& ex) {
        throw ErfError(std::string("Unable to size regular resource input file before staging: ") + path_to_string(source) + ": " + ex.what());
    }
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw ErfError("Resource input payload exceeds the 32-bit archive format limit: " + path_to_string(source));
    }
}

std::filesystem::path stable_staged_input_path(const std::filesystem::path& source) {
    // Data-safe staging: bind relative add-resource inputs to their current
    // location at add time. Otherwise a later current-directory change could
    // make Save package a different file from the one the user added.
    std::error_code ec;
    auto absolute = std::filesystem::absolute(source, ec);
    return ec ? source : absolute.lexically_normal();
}

std::uint64_t current_process_id_for_temp_suffix() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t temp_process_nonce_for_suffix() {
    static const char nonce_anchor = 0;
    static const std::uint64_t nonce = [] {
        const auto steady = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto high = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        const auto pid = current_process_id_for_temp_suffix();
        const auto anchor = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&nonce_anchor));
        return steady ^ (high << 7u) ^ (pid << 17u) ^ anchor;
    }();
    return nonce;
}

std::string save_temp_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream out;
    out << std::hex << ticks << "-p" << current_process_id_for_temp_suffix()
        << "-n" << temp_process_nonce_for_suffix()
        << "-" << counter.fetch_add(1, std::memory_order_relaxed);
    return out.str();
}

bool create_private_save_directory(const std::filesystem::path& candidate, std::error_code& ec) {
#if defined(_WIN32)
    return std::filesystem::create_directory(candidate, ec);
#else
    // Data-safe staging: archive saves copy user-selected resources into this
    // folder before rewriting the archive.  Use a private 0700 directory so
    // another local user cannot inject, remove, or replace staged files between
    // the copy and enumeration phases when the temp root is shared or has a
    // permissive umask.
    if (::mkdir(path_to_string(candidate).c_str(), 0700) == 0) {
        ec.clear();
        return true;
    }
    ec = std::error_code(errno, std::generic_category());
    return false;
#endif
}

std::filesystem::path make_unique_save_temp_folder(const std::filesystem::path& temp_root, const std::filesystem::path& archive_filename) {
    std::filesystem::path root = temp_root.empty() ? std::filesystem::current_path() : temp_root;
    std::filesystem::create_directories(root);
    if (!directory_exists(root)) {
        throw ErfError("Unable to create save temporary root: " + path_to_string(root));
    }

    std::string stem = ascii_lower(archive_filename.stem().string());
    if (stem.empty()) {
        stem = "archive";
    }
    for (int attempt = 0; attempt < 128; ++attempt) {
        std::filesystem::path candidate = root / (stem + "_tmp.neoerf-" + save_temp_suffix());
        std::error_code ec;
        if (create_private_save_directory(candidate, ec)) {
            return candidate;
        }
        if (ec && ec != std::errc::file_exists) {
            throw ErfError("Unable to create save temporary folder: " + path_to_string(candidate) + ": " + ec.message());
        }
    }
    throw ErfError("Unable to create a unique save temporary folder in: " + path_to_string(root));
}

std::filesystem::path make_unique_output_temp_path(const std::filesystem::path& target) {
    std::filesystem::path dir = target.parent_path();
    if (dir.empty()) {
        dir = std::filesystem::current_path();
    }
    const std::string leaf = target.filename().string().empty() ? std::string("archive") : target.filename().string();
    for (int attempt = 0; attempt < 128; ++attempt) {
        std::filesystem::path candidate = dir / (leaf + ".neoerf-saving-" + save_temp_suffix() + ".tmp");
        if (!file_exists(candidate) && !directory_exists(candidate)) {
            return candidate;
        }
    }
    throw ErfError("Unable to choose a unique temporary output archive path near: " + path_to_string(target));
}


class ScopedSaveTempFolder {
public:
    ScopedSaveTempFolder(std::filesystem::path path, PathIdentity identity)
        : path_(std::move(path)), identity_(identity) {}
    ~ScopedSaveTempFolder() {
        if (active_) {
            cleanup_owned_files_noexcept();
        }
    }
    ScopedSaveTempFolder(const ScopedSaveTempFolder&) = delete;
    ScopedSaveTempFolder& operator=(const ScopedSaveTempFolder&) = delete;

    void remember_file(std::filesystem::path file, FileIdentity identity) {
        if (identity.valid) {
            owned_files_.push_back({std::move(file), identity});
        }
    }

    void cleanup_now_noexcept() noexcept {
        cleanup_owned_files_noexcept();
        active_ = false;
    }

    void release() noexcept { active_ = false; }

private:
    void cleanup_owned_files_noexcept() noexcept {
        try {
            if (!same_path_identity(path_, identity_)) {
                return;
            }
            for (const auto& owned : owned_files_) {
                if (!same_path_identity(path_, identity_)) {
                    return;
                }
                remove_file_if_same_identity_noexcept(owned.first, owned.second);
            }
            if (same_path_identity(path_, identity_)) {
                std::error_code ignored;
                std::filesystem::remove(path_, ignored);
            }
        } catch (...) {
        }
    }

    std::filesystem::path path_;
    PathIdentity identity_{};
    std::vector<std::pair<std::filesystem::path, FileIdentity>> owned_files_;
    bool active_ = true;
};

class ScopedTempOutputFile {
public:
    explicit ScopedTempOutputFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~ScopedTempOutputFile() {
        if (active_) {
            remove_file_if_same_identity_noexcept(path_, identity_);
        }
    }
    ScopedTempOutputFile(const ScopedTempOutputFile&) = delete;
    ScopedTempOutputFile& operator=(const ScopedTempOutputFile&) = delete;
    void activate(FileIdentity identity) noexcept {
        identity_ = identity;
        active_ = identity_.valid;
    }
    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    FileIdentity identity_{};
    bool active_ = false;
};

void verify_temp_archive_before_replace(const std::filesystem::path& filename) {
    ErfArchive probe;
    probe.load(filename);

    std::error_code ec;
    const auto archive_size = std::filesystem::file_size(filename, ec);
    if (ec) {
        throw ErfError("Refusing to replace target; unable to size temporary archive: " + path_to_string(filename));
    }

    const bool packed_toc = probe.disk_format() == ArchiveDiskFormat::ErfV2_2 ||
                            probe.disk_format() == ArchiveDiskFormat::ErfV3_0;
    for (const auto& res : probe.resources()) {
        const std::uintmax_t stored_size = packed_toc && res.packed_size != 0
            ? static_cast<std::uintmax_t>(res.packed_size)
            : static_cast<std::uintmax_t>(res.data_size);
        if (stored_size == 0) {
            continue;
        }
        const std::uintmax_t offset = res.data_offset;
        if (offset > archive_size || stored_size > archive_size - offset) {
            throw ErfError("Refusing to replace target with a temporary archive containing out-of-range resource data.");
        }
    }
}




#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
void fsync_parent_directory_after_replace(const std::filesystem::path& target) {
    std::filesystem::path dir = target.parent_path();
    if (dir.empty()) {
        dir = std::filesystem::current_path();
    }
    const int fd = ::open(path_to_string(dir).c_str(), O_RDONLY | O_DIRECTORY | close_on_exec_flag());
    if (fd < 0) {
        throw ErfError("Unable to open output directory for sync after replace: " + path_to_string(dir));
    }
    ScopedPosixFd dir_fd(fd);
    if (!fsync_retry(dir_fd.get())) {
        throw ErfError("Unable to sync output directory after replace: " + path_to_string(dir));
    }
    if (!dir_fd.close()) {
        throw ErfError("Unable to close output directory after replace: " + path_to_string(dir));
    }
}
#endif

void replace_file_atomically(const std::filesystem::path& source, const FileIdentity& source_identity, const std::filesystem::path& target, const ReplacementTargetState& target_state) {
    if (!same_regular_file_identity(source, source_identity)) {
        throw ErfError("Temporary output file was replaced before final destination update: " + path_to_string(source));
    }
    ensure_replacement_target_unchanged(target, target_state, "Output replacement safety check failed");
#if defined(_WIN32)
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (target_state.identity.valid) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (!MoveFileExW(source.c_str(), target.c_str(), flags)) {
        throw ErfError("Unable to replace output archive safely: " + path_to_string(target));
    }
    if (!same_regular_file_identity(target, source_identity)) {
        throw ErfError("Output replacement target does not match the verified temporary output: " + path_to_string(target));
    }
#elif defined(__EMSCRIPTEN__)
    // MEMFS is process-local and single-threaded in the browser build. POSIX
    // hard-link creation and directory fsync are desktop durability mechanisms,
    // not browser persistence primitives. Rename the verified temp node inside
    // the same virtual directory, then verify that the destination is that node.
    std::error_code ec;
    std::filesystem::rename(source, target, ec);
    if (ec) {
        throw ErfError("Unable to finalize browser archive output: " +
                       path_to_string(target) + ": " + ec.message());
    }
    if (!same_regular_file_identity(target, source_identity)) {
        throw ErfError("Browser archive output does not match the verified temporary output: " +
                       path_to_string(target));
    }
#else
    if (!target_state.identity.valid) {
        // If the requested output did not exist when the operation started,
        // create it without overwriting.  A plain rename() would replace a file
        // that appeared after the last destination-state check, which violates
        // the data-safe gate.  The temp output is created in the target
        // directory, so a hard link is same-filesystem and preserves bytes.
        if (::link(path_to_string(source).c_str(), path_to_string(target).c_str()) != 0) {
            throw ErfError("Unable to create output safely without overwriting: " + path_to_string(target) + ": " + std::strerror(errno));
        }
        if (!same_regular_file_identity(target, source_identity)) {
            const FileIdentity created_identity = capture_regular_file_identity(target);
            remove_file_if_same_identity_noexcept(target, created_identity);
            throw ErfError("Output replacement target does not match the verified temporary output: " + path_to_string(target));
        }
        remove_file_if_same_identity_noexcept(source, source_identity);
    } else {
        std::error_code ec;
        std::filesystem::rename(source, target, ec);
        if (ec) {
            throw ErfError("Unable to replace output archive safely: " + target.string() + ": " + ec.message());
        }
        if (!same_regular_file_identity(target, source_identity)) {
            throw ErfError("Output replacement target does not match the verified temporary output: " + path_to_string(target));
        }
    }
    fsync_parent_directory_after_replace(target);
#endif
}

void copy_stream_bytes(std::istream& in, std::ostream& out, std::uint64_t bytes) {
    // Data-safe mode keeps the same valid archive semantics but is no longer
    // constrained to small $F000 copy chunk.  A larger buffer reduces
    // syscall/stream overhead on large resource extraction while retaining the
    // same fail-closed read-before-write boundary for malformed/truncated data.
    std::array<char, kDataSafeIoBufferSize> buffer{};
    std::uint64_t remaining = bytes;
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining));
        in.read(buffer.data(), static_cast<std::streamsize>(want));
        const auto got = in.gcount();
        // stream copy calls Source.ReadBuffer before WriteBuffer
        // for each positive chunk.  If ReadBuffer raises on a short final
        // read, the partial chunk is not written to the destination stream.
        if (got != static_cast<std::streamsize>(want)) {
            throw ErfError("Unexpected end of archive while copying resource payload.");
        }
        out.write(buffer.data(), static_cast<std::streamsize>(want));
        if (!out) {
            throw ErfError("Failed to write resource payload.");
        }
        remaining -= static_cast<std::uint64_t>(want);
    }
}

std::vector<std::uint8_t> read_payload_bytes(std::istream& in, std::uint32_t offset, std::uint32_t size) {
    std::vector<std::uint8_t> bytes(size);
    if (size == 0) {
        return bytes;
    }
    seekg_from_u32_offset(in, offset);
    read_exact(in, reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()), "compressed resource payload");
    return bytes;
}

void inflate_bytes_to_stream(const std::vector<std::uint8_t>& payload,
                             std::size_t start_offset,
                             std::ostream& out,
                             std::uint32_t expected_size,
                             int window_bits,
                             const char* context) {
    if (start_offset > payload.size()) {
        throw ErfError(std::string("Malformed archive: ") + context + " compressed payload is shorter than its header.");
    }

    z_stream stream{};
    const int init = inflateInit2(&stream, window_bits);
    if (init != Z_OK) {
        throw ErfError(std::string("Unable to initialize zlib for ") + context + ".");
    }

    struct InflateGuard {
        z_stream* stream = nullptr;
        ~InflateGuard() {
            if (stream != nullptr) {
                inflateEnd(stream);
            }
        }
    } guard{&stream};

    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(payload.data() + start_offset));
    stream.avail_in = static_cast<uInt>(payload.size() - start_offset);

    std::array<unsigned char, kDataSafeIoBufferSize> buffer{};
    std::uint64_t produced_total = 0;
    int ret = Z_OK;
    do {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        ret = inflate(&stream, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            throw ErfError(std::string("Unable to decompress ERF resource payload for ") + context + ".");
        }
        const std::size_t produced = buffer.size() - stream.avail_out;
        if (produced != 0) {
            out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(produced));
            if (!out) {
                throw ErfError("Failed to write decompressed ERF resource payload.");
            }
            produced_total += produced;
            if (produced_total > expected_size) {
                throw ErfError(std::string("Malformed archive: decompressed ERF resource payload for ") + context + " is larger than the TOC size.");
            }
        }
    } while (ret != Z_STREAM_END);

    if (produced_total != expected_size) {
        throw ErfError(std::string("Malformed archive: decompressed ERF resource payload for ") + context + " does not match the TOC size.");
    }
}

void copy_resource_payload_to_stream(std::istream& archive,
                                     std::ostream& out,
                                     const Resource& res,
                                     ArchiveDiskFormat disk_format,
                                     std::uint32_t archive_flags) {
    const bool has_packed_toc = disk_format == ArchiveDiskFormat::ErfV2_2 ||
                                disk_format == ArchiveDiskFormat::ErfV3_0;
    if (!has_packed_toc) {
        if (res.data_size == 0) {
            return;
        }
        seekg_from_u32_offset(archive, res.data_offset);
        copy_stream_bytes(archive, out, res.data_size);
        return;
    }

    const char* version_label = disk_format == ArchiveDiskFormat::ErfV2_2 ? "ERF V2.2" : "ERF V3.0";
    ensure_supported_erf_compression_flags(archive_flags, version_label);
    const auto compression = erf_compression_scheme(archive_flags);
    const std::uint32_t packed_size = res.packed_size != 0 || res.data_size == 0 ? res.packed_size : res.data_size;
    if (packed_size == 0 && res.data_size == 0) {
        return;
    }
    if (compression == 0) {
        seekg_from_u32_offset(archive, res.data_offset);
        copy_stream_bytes(archive, out, packed_size);
        return;
    }

    const auto payload = read_payload_bytes(archive, res.data_offset, packed_size);
    const std::string context = resource_filename_for_archive(res,
        disk_format == ArchiveDiskFormat::ErfV2_2 ? ResourceNameProfile::DragonAgeOrigins : ResourceNameProfile::DragonAge2);
    if (compression == 1) {
        // BioWare-zlib streams store a one-byte wrapper before raw DEFLATE.
        inflate_bytes_to_stream(payload, 1u, out, res.data_size, -MAX_WBITS, context.c_str());
    } else if (compression == 7) {
        inflate_bytes_to_stream(payload, 0u, out, res.data_size, -MAX_WBITS, context.c_str());
    } else {
        throw ErfError(std::string(version_label) + " uses an unsupported compression scheme: " + std::to_string(compression));
    }
}


class VectorOutputStreamBuf final : public std::streambuf {
public:
    explicit VectorOutputStreamBuf(std::size_t expected_size) {
        bytes_.reserve(expected_size);
    }

    std::vector<std::uint8_t> take() && {
        return std::move(bytes_);
    }

protected:
    std::streamsize xsputn(const char* source, std::streamsize count) override {
        if (count <= 0) return 0;
        const auto* begin = reinterpret_cast<const std::uint8_t*>(source);
        bytes_.insert(bytes_.end(), begin, begin + static_cast<std::size_t>(count));
        return count;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        bytes_.push_back(static_cast<std::uint8_t>(traits_type::to_char_type(value)));
        return value;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

std::vector<std::uint8_t> read_resource_payload_bytes(std::istream& archive,
                                                      const Resource& resource,
                                                      ArchiveDiskFormat disk_format,
                                                      std::uint32_t archive_flags) {
    VectorOutputStreamBuf buffer(resource.data_size);
    std::ostream output(&buffer);
    copy_resource_payload_to_stream(archive, output, resource, disk_format, archive_flags);
    output.flush();
    if (!output) {
        throw ErfError("Failed to prepare the resource payload in memory.");
    }
    return std::move(buffer).take();
}

std::string zlib_result_message(const char* operation, int code, const z_stream& stream) {
    std::string message = std::string(operation) + " failed";
    if (stream.msg != nullptr) {
        message += ": ";
        message += stream.msg;
    } else {
        message += " with code ";
        message += std::to_string(code);
    }
    return message;
}

std::uint64_t write_file_payload_zlib(std::ostream& archive,
                                      const std::filesystem::path& source,
                                      std::uint32_t compression_scheme,
                                      const FileIdentity* expected_identity = nullptr) {
    if (compression_scheme != 1 && compression_scheme != 7) {
        throw ErfError("Unsupported ERF compression scheme for writing: " + std::to_string(compression_scheme));
    }

    const auto input_share_lock = open_read_deny_write_lock(source);
    (void)input_share_lock;

    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw ErfError("Unable to open resource input file for compression: " + path_to_string(source));
    }

    z_stream stream{};
    const int init = deflateInit2(&stream,
                                  Z_BEST_COMPRESSION,
                                  Z_DEFLATED,
                                  -MAX_WBITS,
                                  8,
                                  Z_DEFAULT_STRATEGY);
    if (init != Z_OK) {
        throw ErfError(zlib_result_message("deflateInit2", init, stream));
    }
    struct DeflateGuard {
        z_stream* stream = nullptr;
        ~DeflateGuard() {
            if (stream != nullptr) {
                deflateEnd(stream);
            }
        }
    } guard{&stream};

    std::uint64_t written_total = 0;
    if (compression_scheme == 1) {
        const unsigned char header = 0xF9u;
        archive.write(reinterpret_cast<const char*>(&header), 1);
        if (!archive) {
            throw ErfError("Failed to write BioWare-zlib payload header.");
        }
        written_total = 1;
    }
    std::uint64_t read_total = 0;

    std::array<unsigned char, kDataSafeIoBufferSize> inbuf{};
    std::array<unsigned char, kDataSafeIoBufferSize> outbuf{};
    int rc = Z_OK;
    bool finished_input = false;
    while (!finished_input) {
        input.read(reinterpret_cast<char*>(inbuf.data()), static_cast<std::streamsize>(inbuf.size()));
        const std::streamsize got = input.gcount();
        if (input.bad()) {
            throw ErfError("Unable to read resource input file for compression: " + path_to_string(source));
        }
        read_total += static_cast<std::uint64_t>(got);
        finished_input = input.eof();

        stream.next_in = inbuf.data();
        stream.avail_in = static_cast<uInt>(got);
        const int flush = finished_input ? Z_FINISH : Z_NO_FLUSH;
        do {
            stream.next_out = outbuf.data();
            stream.avail_out = static_cast<uInt>(outbuf.size());
            rc = deflate(&stream, flush);
            if (rc == Z_STREAM_ERROR) {
                throw ErfError(zlib_result_message("deflate", rc, stream));
            }
            const std::size_t produced = outbuf.size() - stream.avail_out;
            if (produced != 0) {
                archive.write(reinterpret_cast<const char*>(outbuf.data()), static_cast<std::streamsize>(produced));
                if (!archive) {
                    throw ErfError("Failed to write compressed ERF resource payload.");
                }
                written_total += produced;
            }
        } while (stream.avail_out == 0);
    }

    if (rc != Z_STREAM_END) {
        throw ErfError("Failed to finish ERF resource compression.");
    }
    if (expected_identity != nullptr && expected_identity->valid && read_total != expected_identity->size) {
        throw ErfError("Save-staged resource file changed size before archive rewrite: " + path_to_string(source));
    }
    return written_total;
}

std::uint64_t write_file_payload(std::ostream& archive, const std::filesystem::path& source, const FileIdentity* expected_identity = nullptr) {
    const auto input_share_lock = open_read_deny_write_lock(source);
    (void)input_share_lock;
    try {
        const auto copied = write_regular_file_to_stream_limited(archive, source, std::numeric_limits<std::uint32_t>::max(), expected_identity);
        if (expected_identity != nullptr && expected_identity->valid && copied != expected_identity->size) {
            throw ErfError("Save-staged resource file changed size before archive rewrite: " + path_to_string(source));
        }
        return copied;
    } catch (const std::exception& ex) {
        throw ErfError(std::string("Unable to copy resource input file into archive: ") + path_to_string(source) + ": " + ex.what());
    }
}


template <std::size_t N>
std::uint32_t read_u32_from_byte_array(const std::array<std::uint8_t, N>& bytes, std::size_t offset) {
    if (offset + 4 > N) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

template <std::size_t N>
void write_u32_to_byte_array(std::array<std::uint8_t, N>& bytes, std::size_t offset, std::uint32_t value) {
    if (offset + 4 > N) {
        return;
    }
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}


} // namespace

std::array<char, 16> Resource::raw_resref() const {
    if (raw_resref16_assigned) {
        return raw_resref16_storage;
    }

    std::array<char, 16> raw{};
    if (extended) {
        // resource-name model keeps a separate 16-byte buffer. set_resref() populates
        // only raw resref32 when extended=true, so reading raw_resref returns the
        // untouched/zero 16-byte buffer rather than a truncated 32-byte ResRef.
        return raw;
    }

    const std::string normalized = string_to_resref(resref, false);
    std::copy_n(normalized.begin(), std::min<std::size_t>(normalized.size(), raw.size()), raw.begin());
    return raw;
}

std::array<char, 32> Resource::raw_resref32() const {
    if (raw_resref32_assigned) {
        return raw_resref32_storage;
    }

    std::array<char, 32> raw{};
    if (!extended) {
        // Symmetric behavior: a non-extended resource-name model never fills
        // raw resref32 through set_resref(), so raw_resref32 remains zero unless it
        // was explicitly assigned through the raw32 property.
        return raw;
    }

    const std::string normalized = string_to_resref(resref, true);
    std::copy_n(normalized.begin(), std::min<std::size_t>(normalized.size(), raw.size()), raw.begin());
    return raw;
}

void Resource::set_raw_resref(const std::array<char, 16>& raw) {
    // resource entry.set_resrefRaw writes the raw resref array directly. It only
    // changes the ResRef text observed through resref() when extended=false;
    // an extended resource reads text from the independent 32-byte buffer.
    raw_resref16_storage = raw;
    raw_resref16_assigned = true;
    if (!extended) {
        resref.clear();
        for (char ch : raw) {
            if (ch != '\0') {
                resref.push_back(ch);
            }
        }
    }
}

void Resource::set_raw_resref32(const std::array<char, 32>& raw) {
    raw_resref32_storage = raw;
    raw_resref32_assigned = true;
    if (extended) {
        resref.clear();
        for (char ch : raw) {
            if (ch != '\0') {
                resref.push_back(ch);
            }
        }
    }
}

std::string Resource::extension() const {
    return extension(ResourceNameProfile::KotOR);
}

std::string Resource::extension(ResourceNameProfile profile) const {
    if (profile == ResourceNameProfile::DragonAge2 && !filename.empty()) {
        const std::string ext = extension_no_dot_from_name(filename);
        if (!ext.empty()) {
            return ext;
        }
    }
    return res_type_to_string(restype, profile);
}

std::string Resource::res_type_to_string(std::uint16_t type) {
    return res_type_to_string(type, ResourceNameProfile::KotOR);
}

std::string Resource::res_type_to_string(std::uint16_t type, ResourceNameProfile profile) {
    auto lookup = [type](const ResourceTypeName* table, std::size_t count) -> const char* {
        for (std::size_t i = 0; i < count; ++i) {
            if (table[i].primary && table[i].type == type) {
                return table[i].extension;
            }
        }
        return nullptr;
    };

    std::size_t count = 0;
    const ResourceTypeName* table = resourceTypeTable(profile, count);
    if (const char* ext = lookup(table, count)) {
        return ext;
    }
    if (profile != ResourceNameProfile::KotOR) {
        table = resourceTypeTable(ResourceNameProfile::KotOR, count);
        if (const char* ext = lookup(table, count)) {
            return ext;
        }
    }
    return "#" + std::to_string(type);
}

std::uint16_t Resource::string_to_res_type(std::string type) {
    return string_to_res_type(std::move(type), ResourceNameProfile::KotOR);
}

std::uint16_t Resource::string_to_res_type(std::string type, ResourceNameProfile profile) {
    type = ascii_lower(std::move(type));
    if (type.empty()) {
        throw ErfError("Invalid empty resource type");
    }
    if (type[0] == '.') {
        type.erase(type.begin());
    }

    if (type.size() > 1 && type[0] == '#' && is_number(type.substr(1))) {
        return parse_archive_resource_type_number(type.substr(1));
    }

    auto lookup = [&type](const ResourceTypeName* table, std::size_t count) -> std::uint16_t {
        for (std::size_t i = 0; i < count; ++i) {
            if (type == table[i].extension) {
                return table[i].type;
            }
        }
        return kUnknownType;
    };

    std::size_t count = 0;
    const ResourceTypeName* table = resourceTypeTable(profile, count);
    const std::uint16_t selected = lookup(table, count);
    if (selected != kUnknownType) {
        return selected;
    }
    if (profile != ResourceNameProfile::KotOR) {
        table = resourceTypeTable(ResourceNameProfile::KotOR, count);
        return lookup(table, count);
    }
    return kUnknownType;
}

bool ErfArchive::Header::is_rim() const {
    return same_header(filetype, "RIM ");
}

ErfArchive::ErfArchive()
    : temp_folder_path_(default_temp_root()) {
    reset();
}

ErfArchive::ErfArchive(const std::filesystem::path& file_to_load)
    : temp_folder_path_(default_temp_root()) {
    load(file_to_load);
}

ErfArchive::~ErfArchive() {
    try {
        reset(true);
    } catch (...) {
    }
}

void ErfArchive::reset(bool destroying) {
    if (archive_stream_.is_open()) {
        archive_stream_.close();
    }
    archive_stream_reference_dangling_ = false;
    loaded_file_identity_ = FileIdentity{};
    reader_backed_ = false;
#if defined(_WIN32)
    close_loaded_archive_lock(archive_lock_handle_);
#endif
    locstrings_.clear();
    resources_.clear();
    new_files_.clear();
    new_file_identities_.clear();
    new_names_.clear();
    new_resource_metadata_.clear();
    new_lists_allocated_ = !destroying;
    header_ = Header{};
    has_header_ = false;
    filename_.clear();
    loaded_ = false;
    newfile_ = false;
    dirty_ = false;
    resref32_ = false;
    disk_format_ = ArchiveDiskFormat::ErfV1;
    (void)destroying;
    temp_folder_path_ = default_temp_root();
}

void ErfArchive::set_temp_path(std::filesystem::path path) {
    temp_folder_path_ = std::move(path);
}

void ErfArchive::new_archive(const std::filesystem::path& filename, ArchiveType type) {
    reset();
    filename_ = filename;
    header_ = Header{};
    has_header_ = true;
    header_.filetype = archive_type_to_header(type);
    if (type == ArchiveType::ERF_V2) {
        header_.version = make_header("V2.0");
        header_.v2_unknown = 0xFFFFFFFFu;
        header_.v2_flags = 0;
        header_.v2_module_id = 0;
        header_.v2_password_digest = {};
        disk_format_ = ArchiveDiskFormat::ErfV2_0;
        resource_type_profile_ = ResourceNameProfile::DragonAgeOrigins;
        resref32_ = false;
    } else if (type == ArchiveType::ERF_V2_2 || type == ArchiveType::ERF_V2_2_UNCOMPRESSED) {
        header_.version = make_header("V2.2");
        header_.v2_unknown = 0xFFFFFFFFu;
        header_.v2_flags = type == ArchiveType::ERF_V2_2 ? kErfCompressionBiowareZlib : 0;
        header_.v2_module_id = 0;
        header_.v2_password_digest = {};
        disk_format_ = ArchiveDiskFormat::ErfV2_2;
        resource_type_profile_ = ResourceNameProfile::DragonAgeOrigins;
        resref32_ = false;
    } else if (type == ArchiveType::ERF_V3) {
        header_.version = make_header("V3.0");
        header_.v3_string_table_size = 0;
        header_.v3_flags = 0;
        header_.v3_module_id = 0;
        header_.v3_password_digest = {};
        disk_format_ = ArchiveDiskFormat::ErfV3_0;
        resource_type_profile_ = ResourceNameProfile::DragonAge2;
        resref32_ = false;
    } else {
        const bool use_extended_resrefs =
            type != ArchiveType::RIM && resource_type_profile_ == ResourceNameProfile::NeverwinterNights2;
        header_.version = make_header(use_extended_resrefs ? "V1.1" : "V1.0");
        disk_format_ = (type == ArchiveType::RIM) ? ArchiveDiskFormat::RimV1 : ArchiveDiskFormat::ErfV1;
        resref32_ = use_extended_resrefs;
    }
    const auto [year, day] = current_erf_build_time();
    header_.buildyear = year;
    header_.buildday = day;
    header_.locstringstrref = 0xFFFFFFFFu;
    loaded_ = true;
    newfile_ = true;
}

bool ErfArchive::is_valid_archive(const std::filesystem::path& filename) {
    // Data-safe validation: a header-only probe can report malformed archives
    // as "valid" even when their key/resource tables or payload extents point
    // outside the file.  Load through the normal parser so the same fail-closed
    // table/payload checks used by the UI and CLI are applied.
    try {
        ErfArchive probe;
        probe.load(filename);
        return true;
    } catch (...) {
        return false;
    }
}

void ErfArchive::load(const std::filesystem::path& filename) {
    if (!file_exists(filename)) {
        throw ErfError("Unable to load file \"" + path_to_string(filename) + "\", file not found!");
    }

    reset();
    FileIdentity identity{};
    try {
        identity = capture_regular_file_identity(filename);
    } catch (const std::exception& ex) {
        throw ErfError("Unable to load file \"" + path_to_string(filename) +
                       "\", the file identity could not be captured: " + ex.what());
    }
#if defined(_WIN32)
    archive_lock_handle_ = open_loaded_archive_lock(filename);
    if (archive_lock_handle_ == nullptr) {
        throw ErfError("Unable to load file \"" + path_to_string(filename) + "\", the file could not be opened!");
    }
#endif
    open_binary_input_stream(archive_stream_, filename);
    if (!archive_stream_.is_open()) {
#if defined(_WIN32)
        close_loaded_archive_lock(archive_lock_handle_);
#endif
        const std::string detail = archive_stream_.last_error();
        throw ErfError("Unable to load file \"" + path_to_string(filename) +
                       "\", the file could not be opened" +
                       (detail.empty() ? std::string("!") : std::string(": ") + detail));
    }
    if (!same_regular_file_revision(filename, identity)) {
        archive_stream_.close();
#if defined(_WIN32)
        close_loaded_archive_lock(archive_lock_handle_);
#endif
        throw ErfError("Unable to load file \"" + path_to_string(filename) +
                       "\", the file changed while it was being opened!");
    }
    loaded_file_identity_ = identity;
    reader_backed_ = false;
    load_open_stream(filename);
    if (!same_regular_file_revision(filename, loaded_file_identity_)) {
        reset();
        throw ErfError("Archive changed while indexing; reopen the archive.");
    }
}

void ErfArchive::load_from_reader(const std::filesystem::path& logical_filename,
                                  std::uint64_t archive_size,
                                  ArchiveRangeReader reader) {
    if (!reader) {
        throw ErfError("Unable to load archive, the range reader is unavailable.");
    }

    reset();
    const std::filesystem::path display_name = logical_filename.empty()
        ? std::filesystem::path("browser-archive")
        : logical_filename;
    if (!archive_stream_.open_reader(archive_size, std::move(reader))) {
        const std::string detail = archive_stream_.last_error();
        throw ErfError("Unable to load archive \"" + path_to_string(display_name) +
                       "\" from its range reader" +
                       (detail.empty() ? std::string(".") : std::string(": ") + detail));
    }
    reader_backed_ = true;
    load_open_stream(display_name);
}

void ErfArchive::release_input_handle() noexcept {
    if (reader_backed_) {
        archive_stream_.discard_cache();
        return;
    }
    if (archive_stream_.is_open()) archive_stream_.close();
#if defined(_WIN32)
    close_loaded_archive_lock(archive_lock_handle_);
#endif
}

void ErfArchive::load_open_stream(const std::filesystem::path& filename) {
    try {
        const auto archive_size = checked_open_archive_stream_size(archive_stream_, filename);
        Header next_header{};
        const auto first16 = read_char_array<16>(archive_stream_);

        const bool is_erf_v2_0 = is_erf_v2_0_magic(first16);
        const bool is_erf_v2_2 = is_erf_v2_2_magic(first16);
        if (is_erf_v2_0 || is_erf_v2_2) {
            const char* version_label = is_erf_v2_2 ? "ERF V2.2" : "ERF V2.0";
            const std::uint32_t header_size = is_erf_v2_2 ? 56u : 32u;
            const std::uint32_t toc_entry_size = is_erf_v2_2 ? 76u : 72u;

            next_header.filetype = make_header("ERF ");
            next_header.version = make_header(is_erf_v2_2 ? "V2.2" : "V2.0");
            next_header.entrycount = read_u32(archive_stream_);
            next_header.buildyear = read_u32(archive_stream_);
            next_header.buildday = read_u32(archive_stream_);
            next_header.v2_unknown = read_u32(archive_stream_);
            if (is_erf_v2_2) {
                next_header.v2_flags = read_u32(archive_stream_);
                next_header.v2_module_id = read_u32(archive_stream_);
                read_byte_array(archive_stream_, next_header.v2_password_digest);
                ensure_supported_erf_compression_flags(next_header.v2_flags, version_label);
            }

            resref32_ = false;
            disk_format_ = is_erf_v2_2 ? ArchiveDiskFormat::ErfV2_2 : ArchiveDiskFormat::ErfV2_0;
            resource_type_profile_ = ResourceNameProfile::DragonAgeOrigins;

            const std::string entrycount_label = std::string(version_label) + " entrycount";
            const std::string toc_label = std::string(version_label) + " table of contents";
            const std::string header_label = std::string(version_label) + " header";
            const std::string payload_label = std::string(version_label) + " resource payload";
            ensure_safe_metadata_count(next_header.entrycount, entrycount_label.c_str());
            const auto toc_bytes = checked_table_bytes(next_header.entrycount, toc_entry_size, toc_label.c_str());
            const ArchiveRange header_range = make_archive_range(0, header_size, archive_size, header_label.c_str());
            const ArchiveRange toc_range = make_archive_range(header_size, toc_bytes, archive_size, toc_label.c_str());
            const std::vector<ArchiveRange> metadata_ranges{header_range, toc_range};
            ensure_non_overlapping_archive_sections(metadata_ranges);

            resources_.reserve(static_cast<std::size_t>(next_header.entrycount));
            seekg_from_u32_offset(archive_stream_, header_size);
            for (std::uint32_t i = 0; i < next_header.entrycount; ++i) {
                const std::string loaded_name = read_fixed_utf16le_string(archive_stream_, 32, "ERF V2.x resource filename");
                const std::string leaf = normalize_archive_leaf(loaded_name);
                if (leaf != loaded_name) {
                    throw ErfError(std::string("Malformed archive: ") + version_label + " resource filename contains a path component: " + loaded_name);
                }
                validate_erf_v2_resource_leaf(leaf);

                Resource res;
                res.extended = false;
                res.filename = leaf;
                res.resref = resource_stem_from_text(leaf);
                res.resid = i;
                res.restype = type_from_filename_extension_allow_unknown(leaf, ResourceNameProfile::DragonAgeOrigins);
                res.data_offset = read_u32(archive_stream_);
                if (is_erf_v2_2) {
                    res.packed_size = read_u32(archive_stream_);
                    res.data_size = read_u32(archive_stream_);
                } else {
                    res.data_size = read_u32(archive_stream_);
                    res.packed_size = res.data_size;
                }

                const std::uint32_t stored_size = is_erf_v2_2 ? res.packed_size : res.data_size;
                if (stored_size > 0) {
                    ensure_archive_extent(res.data_offset, stored_size, archive_size, payload_label.c_str());
                    ensure_payload_does_not_overlap_metadata(res.data_offset, stored_size,
                                                             metadata_ranges,
                                                             payload_label.c_str());
                } else if (res.data_size > 0) {
                    throw ErfError(std::string("Malformed archive: ") + version_label + " resource has zero packed size but non-zero unpacked size.");
                }
                resources_.push_back(std::move(res));
            }
        } else if (is_erf_v3_magic(first16)) {
            next_header.filetype = make_header("ERF ");
            next_header.version = make_header("V3.0");
            next_header.v3_string_table_size = read_u32(archive_stream_);
            next_header.entrycount = read_u32(archive_stream_);
            next_header.v3_flags = read_u32(archive_stream_);
            next_header.v3_module_id = read_u32(archive_stream_);
            read_byte_array(archive_stream_, next_header.v3_password_digest);

            ensure_supported_v3_flags(next_header.v3_flags);
            resref32_ = false;
            disk_format_ = ArchiveDiskFormat::ErfV3_0;
            resource_type_profile_ = ResourceNameProfile::DragonAge2;

            ensure_safe_metadata_count(next_header.entrycount, "ERF V3.0 file count");
            const std::uint32_t string_table_offset = 48u;
            const std::uint32_t toc_offset = checked_add_u32(string_table_offset, next_header.v3_string_table_size, "ERF V3.0 TOC offset");
            const auto toc_bytes = checked_table_bytes(next_header.entrycount, 28u, "ERF V3.0 table of contents");
            const ArchiveRange header_range = make_archive_range(0, 48, archive_size, "ERF V3.0 header");
            const ArchiveRange string_table_range = make_archive_range(string_table_offset, next_header.v3_string_table_size, archive_size, "ERF V3.0 string table");
            const ArchiveRange toc_range = make_archive_range(toc_offset, toc_bytes, archive_size, "ERF V3.0 table of contents");
            const std::vector<ArchiveRange> metadata_ranges{header_range, string_table_range, toc_range};
            ensure_non_overlapping_archive_sections(metadata_ranges);

            std::vector<char> string_table(next_header.v3_string_table_size);
            if (!string_table.empty()) {
                seekg_from_u32_offset(archive_stream_, string_table_offset);
                read_exact(archive_stream_, string_table.data(), static_cast<std::streamsize>(string_table.size()), "ERF V3.0 string table");
            }

            auto read_v3_string = [&](std::int32_t name_offset) -> std::string {
                if (name_offset < 0) {
                    throw ErfError("Malformed archive: negative ERF V3.0 string-table offset.");
                }
                const std::size_t offset = static_cast<std::size_t>(name_offset);
                if (offset >= string_table.size()) {
                    throw ErfError("Malformed archive: ERF V3.0 string-table offset is out of range.");
                }
                std::size_t end = offset;
                while (end < string_table.size() && string_table[end] != '\0') {
                    const unsigned char ch = static_cast<unsigned char>(string_table[end]);
                    if (ch > 0x7Fu) {
                        throw ErfError("Malformed archive: ERF V3.0 string-table name is not ASCII.");
                    }
                    ++end;
                }
                if (end == offset) {
                    throw ErfError("Malformed archive: ERF V3.0 string-table name is empty.");
                }
                if (end >= string_table.size()) {
                    throw ErfError("Malformed archive: ERF V3.0 string-table name is not NUL-terminated.");
                }
                return std::string(string_table.data() + offset, string_table.data() + end);
            };

            resources_.reserve(static_cast<std::size_t>(next_header.entrycount));
            seekg_from_u32_offset(archive_stream_, toc_offset);
            for (std::uint32_t i = 0; i < next_header.entrycount; ++i) {
                const std::int32_t name_offset = read_i32(archive_stream_);
                const std::uint64_t name_hash = read_u64(archive_stream_);
                const std::uint32_t type_hash = read_u32(archive_stream_);
                const std::uint32_t data_offset = read_u32(archive_stream_);
                const std::uint32_t packed_size = read_u32(archive_stream_);
                const std::uint32_t unpacked_size = read_u32(archive_stream_);

                Resource res;
                res.extended = false;
                res.resid = i;
                res.v3_name_hash = name_hash;
                res.v3_type_hash = type_hash;
                res.data_offset = data_offset;
                res.packed_size = packed_size;
                res.data_size = unpacked_size;

                if (name_offset == -1) {
                    res.v3_filename_stripped = true;
                    res.filename = da2_synthetic_resource_name(name_hash, type_hash);
                } else if (name_offset >= 0) {
                    res.v3_filename_stripped = false;
                    res.filename = normalize_archive_path(read_v3_string(name_offset));
                    validate_erf_v3_resource_name(res.filename);
                } else {
                    throw ErfError("Malformed archive: ERF V3.0 name offset is invalid.");
                }

                res.resref = resource_stem_from_text(filename_string(std::filesystem::path(res.filename)));
                res.restype = type_from_filename_extension_allow_unknown(res.filename, ResourceNameProfile::DragonAge2);
                if (res.restype == kUnknownType) {
                    res.restype = da2_restype_from_type_hash(type_hash);
                }
                if (packed_size > 0) {
                    ensure_archive_extent(data_offset, packed_size, archive_size, "ERF V3.0 resource payload");
                    ensure_payload_does_not_overlap_metadata(data_offset, packed_size,
                                                             metadata_ranges,
                                                             "ERF V3.0 resource payload");
                }
                resources_.push_back(std::move(res));
            }
        } else {
            archive_stream_.clear();
            archive_stream_.seekg(0, std::ios::beg);
            if (!archive_stream_) {
                throw ErfError("Unable to load file \"" + path_to_string(filename) + "\", it does not appear to be a valid ERF file type!");
            }

            next_header.filetype = read_char_array<4>(archive_stream_);
            next_header.version = read_char_array<4>(archive_stream_);

            if (!is_valid_filetype(next_header.filetype)) {
                throw ErfError("Unable to load file \"" + path_to_string(filename) + "\", it does not appear to be a valid ERF file type!");
            }
            if (same_header(next_header.version, "V1.0")) {
                resref32_ = false;
            } else if (same_header(next_header.version, "V1.1")) {
                resref32_ = true;
            } else {
                throw ErfError("Unable to load file \"" + path_to_string(filename) + "\", it does not appear to be a valid ERF file type!");
            }

            const bool is_rim = same_header(next_header.filetype, "RIM ");
            disk_format_ = is_rim ? ArchiveDiskFormat::RimV1 : ArchiveDiskFormat::ErfV1;
            if (resource_type_profile_ == ResourceNameProfile::KotOR) {
                const std::string loaded_extension = ascii_lower(extension_string(filename));
                if (resref32_) {
                    resource_type_profile_ = ResourceNameProfile::NeverwinterNights2;
                } else if (loaded_extension == ".nwm" || loaded_extension == ".hak") {
                    resource_type_profile_ = ResourceNameProfile::NeverwinterNights;
                }
            }
            // UERFHandler.Load declares iNum/resource field once and reuses them for
            // localized-string, key-list, and resource-list reads.  Short Read
            // calls therefore preserve the previous value's unread bytes.  Initialize the
            // emulated locals to zero for the first deterministic read boundary.
            std::uint32_t i_num = 0;
            std::uint16_t i_short_num = 0;
            if (!is_rim) {
                const auto loaded_type = next_header.filetype;
                const auto loaded_version = next_header.version;
                const auto [year, day] = current_erf_build_time();
                next_header = Header{};
                next_header.filetype = loaded_type;
                next_header.version = loaded_version;
                next_header.buildyear = year;
                next_header.buildday = day;
                next_header.locstringstrref = 0xFFFFFFFFu;
            }
            if (is_rim) {
                next_header.rim_unknown = read_u32(archive_stream_);
                next_header.entrycount = read_u32(archive_stream_);
                next_header.offsetkeylist = read_u32(archive_stream_);
                read_byte_array(archive_stream_, next_header.rim_reserved);

                ensure_safe_metadata_count(next_header.entrycount, "RIM entrycount");
                const auto rim_table_bytes = checked_table_bytes(next_header.entrycount,
                                                                   resref32_ ? 48u : 32u,
                                                                   "RIM key/resource table");
                const ArchiveRange rim_header_range = make_archive_range(0, 120, archive_size, "RIM header");
                const ArchiveRange rim_table_range = make_archive_range(next_header.offsetkeylist,
                                                                        rim_table_bytes,
                                                                        archive_size,
                                                                        "RIM key/resource table");
                const std::vector<ArchiveRange> rim_metadata_ranges{rim_header_range, rim_table_range};
                ensure_non_overlapping_archive_sections(rim_metadata_ranges);
                resources_.reserve(static_cast<std::size_t>(next_header.entrycount));
                seekg_from_u32_offset(archive_stream_, next_header.offsetkeylist);
                // Reserve after metadata caps/extent validation to avoid repeated reallocations.
                for (std::uint32_t i = 0; i < next_header.entrycount; ++i) {
                    Resource res;
                    res.extended = resref32_;
                    if (resref32_) {
                        res.resref = resref_text_from_loaded_key(read_char_array<32>(archive_stream_));
                    } else {
                        res.resref = resref_text_from_loaded_key(read_char_array<16>(archive_stream_));
                    }
                    i_short_num = read_u16(archive_stream_);
                    res.restype = i_short_num;
                    // The active RIM layout is ResType WORD, reserved WORD, ResID WORD,
                    // reserved WORD, offset, size. Older compatibility code used a
                    // different reserved-field layout; that separate path has been removed.
                    i_short_num = read_u16(archive_stream_);
                    res.reserved_erf = i_short_num;
                    i_short_num = read_u16(archive_stream_);
                    res.resid = i_short_num;
                    i_short_num = read_u16(archive_stream_);
                    res.reserved_rim = i_short_num;
                    i_num = read_u32(archive_stream_);
                    res.data_offset = i_num;
                    i_num = read_u32(archive_stream_);
                    res.data_size = i_num;
                    if (res.data_size > 0) {
                        ensure_archive_extent(res.data_offset, res.data_size, archive_size, "RIM resource payload");
                        ensure_payload_does_not_overlap_metadata(res.data_offset, res.data_size,
                                                                 rim_metadata_ranges,
                                                                 "RIM resource payload");
                    }
                    resources_.push_back(std::move(res));
                }
            } else {
                next_header.locstringcount = read_u32(archive_stream_);
                next_header.locstringsize = read_u32(archive_stream_);
                next_header.entrycount = read_u32(archive_stream_);
                next_header.offsetlocstring = read_u32(archive_stream_);
                next_header.offsetkeylist = read_u32(archive_stream_);
                next_header.offsetreslist = read_u32(archive_stream_);
                next_header.buildyear = read_u32(archive_stream_);
                next_header.buildday = read_u32(archive_stream_);
                next_header.locstringstrref = read_u32(archive_stream_);
                read_byte_array(archive_stream_, next_header.erf_reserved);

                ensure_safe_metadata_count(next_header.locstringcount, "localized string");
                ensure_safe_metadata_count(next_header.entrycount, "ERF entrycount");
                const auto loc_table_bytes = checked_table_bytes(next_header.locstringcount, 8u, "localized string");
                const ArchiveRange erf_header_range = make_archive_range(0, 160, archive_size, "ERF header");
                const ArchiveRange locstring_range = make_archive_range(next_header.offsetlocstring,
                                                                        next_header.locstringsize,
                                                                        archive_size,
                                                                        "localized string section");
                if (loc_table_bytes > next_header.locstringsize) {
                    throw ErfError("Malformed archive: localized string table is larger than the localized string section.");
                }
                const std::uintmax_t locstring_section_end = static_cast<std::uintmax_t>(next_header.offsetlocstring) +
                                                            static_cast<std::uintmax_t>(next_header.locstringsize);
                const auto erf_key_table_bytes = checked_table_bytes(next_header.entrycount,
                                                                     resref32_ ? 40u : 24u,
                                                                     "ERF key table");
                const auto erf_res_table_bytes = checked_table_bytes(next_header.entrycount,
                                                                     8u,
                                                                     "ERF resource table");
                const ArchiveRange erf_key_table_range = make_archive_range(next_header.offsetkeylist,
                                                                            erf_key_table_bytes,
                                                                            archive_size,
                                                                            "ERF key table");
                const ArchiveRange erf_res_table_range = make_archive_range(next_header.offsetreslist,
                                                                            erf_res_table_bytes,
                                                                            archive_size,
                                                                            "ERF resource table");
                const std::vector<ArchiveRange> erf_metadata_ranges{erf_header_range,
                                                                    locstring_range,
                                                                    erf_key_table_range,
                                                                    erf_res_table_range};
                ensure_non_overlapping_archive_sections(erf_metadata_ranges);

                locstrings_.reserve(static_cast<std::size_t>(next_header.locstringcount));
                resources_.reserve(static_cast<std::size_t>(next_header.entrycount));
                seekg_from_u32_offset(archive_stream_, next_header.offsetlocstring);
                // Reserve after metadata caps/extent validation to avoid repeated reallocations.
                for (std::uint32_t i = 0; i < next_header.locstringcount; ++i) {
                    LocalizedString loc;
                    i_num = read_u32(archive_stream_);
                    loc.language_id = i_num;
                    i_num = read_u32(archive_stream_);
                    const std::uint32_t size = i_num;
                    const auto loc_data_pos = archive_stream_.tellg();
                    if (loc_data_pos < std::streampos(0)) {
                        throw ErfError("Malformed archive: localized string data extends beyond the file.");
                    }
                    const auto loc_data_offset = static_cast<std::uintmax_t>(loc_data_pos);
                    if (loc_data_offset > locstring_section_end ||
                        static_cast<std::uintmax_t>(size) > locstring_section_end - loc_data_offset) {
                        throw ErfError("Malformed archive: localized string data extends beyond the localized string section.");
                    }
                    loc.text = read_sized_string(archive_stream_, size);
                    locstrings_.push_back(std::move(loc));
                }

                seekg_from_u32_offset(archive_stream_, next_header.offsetkeylist);
                for (std::uint32_t i = 0; i < next_header.entrycount; ++i) {
                    Resource res;
                    res.extended = resref32_;
                    if (resref32_) {
                        res.resref = resref_text_from_loaded_key(read_char_array<32>(archive_stream_));
                    } else {
                        res.resref = resref_text_from_loaded_key(read_char_array<16>(archive_stream_));
                    }
                    i_num = read_u32(archive_stream_);
                    res.resid = i_num;
                    i_short_num = read_u16(archive_stream_);
                    res.restype = i_short_num;
                    i_short_num = read_u16(archive_stream_);
                    res.reserved_erf = i_short_num;
                    resources_.push_back(std::move(res));
                }

                seekg_from_u32_offset(archive_stream_, next_header.offsetreslist);
                for (auto& res : resources_) {
                    i_num = read_u32(archive_stream_);
                    res.data_offset = i_num;
                    i_num = read_u32(archive_stream_);
                    res.data_size = i_num;
                    if (res.data_size > 0) {
                        ensure_archive_extent(res.data_offset, res.data_size, archive_size, "ERF resource payload");
                        ensure_payload_does_not_overlap_metadata(res.data_offset, res.data_size,
                                                                 erf_metadata_ranges,
                                                                 "ERF resource payload");
                    }
                }
            }
        }

        if (disk_format_ == ArchiveDiskFormat::ErfV2_0 || disk_format_ == ArchiveDiskFormat::ErfV2_2 || disk_format_ == ArchiveDiskFormat::ErfV3_0) {
            ensure_unique_loaded_resource_filenames(resources_, resource_type_profile_, disk_format_);
        } else {
            ensure_unique_loaded_resource_keys(resources_, resref32_);
        }

        header_ = next_header;
        has_header_ = true;
        filename_ = filename;
        loaded_ = true;
        newfile_ = false;
        dirty_ = false;
        archive_stream_.discard_cache();
    } catch (...) {
        reset();
        throw;
    }
}

void ErfArchive::save(std::filesystem::path filename, std::string filetype_override) {
    const Header header_before_save = header_;
    const bool had_header_before_save = has_header_;
    std::filesystem::path output_temp;

    try {
        if (!filetype_override.empty()) {
            if (!has_header_) {
                throw ErfError("Cannot set archive file type before a header is loaded.");
            }
            header_.filetype = std::array<char, 4>{{0, 0, 0, 0}};
            for (std::size_t i = 0; i < header_.filetype.size() && i < filetype_override.size(); ++i) {
                header_.filetype[i] = filetype_override[i];
            }
        }

        if (!loaded_) {
            throw ErfError("Unable to save, no ERF file is open!");
        }
        if (!newfile_) {
            ensure_archive_stream();
        }
        if (filename.empty() && !dirty_) {
            return;
        }
        if (filename.empty()) {
            filename = filename_;
        }
        if (filename.empty()) {
            throw ErfError("Unable to save, no file name has been specified!");
        }

        for (std::size_t i=0;i<new_files_.size();++i) {
            if (!same_regular_file_revision(new_files_[i],new_file_identities_.at(i)))
                throw ErfError("Staged resource changed; add it again before saving: " + path_to_string(new_files_[i]));
        }
        ensure_save_target_does_not_overwrite_staged_inputs(filename, new_files_);
        const ReplacementTargetState save_target_state = capture_replacement_target_state(filename);

        ensure_unique_save_staging_leaves(resources_, new_files_, new_names_, resref32_, resource_type_profile_, disk_format_);

        temp_folder_ = make_unique_save_temp_folder(temp_folder_path_, filename_);
        const PathIdentity temp_folder_identity = capture_path_identity(temp_folder_);
        if (!temp_folder_identity.valid || !temp_folder_identity.is_directory) {
            throw ErfError("Unable to capture save temporary folder ownership: " + path_to_string(temp_folder_));
        }
        ScopedSaveTempFolder temp_guard(temp_folder_, temp_folder_identity);
        output_temp = make_unique_output_temp_path(filename);
        ScopedTempOutputFile output_guard(output_temp);
        std::vector<std::filesystem::path> all_files;
        std::vector<FileIdentity> all_file_identities;
        std::vector<const Resource*> direct_resources;
        std::vector<std::string> all_archive_names;
        std::vector<Resource> all_resource_metadata;
        all_files.reserve(resources_.size() + new_files_.size());
        all_file_identities.reserve(resources_.size() + new_files_.size());
        direct_resources.reserve(resources_.size() + new_files_.size());
        all_archive_names.reserve(resources_.size() + new_files_.size());
        all_resource_metadata.reserve(resources_.size() + new_files_.size());

        auto remember_staged_file = [&](const std::filesystem::path& staged_file,
                                        const FileIdentity& identity,
                                        std::string archive_name,
                                        Resource metadata) {
            if (!identity.valid) {
                throw ErfError("Unable to bind save-staged file ownership: " + path_to_string(staged_file));
            }
            temp_guard.remember_file(staged_file, identity);
            all_files.push_back(staged_file);
            all_file_identities.push_back(identity);
            direct_resources.push_back(nullptr);
            if (filename_based_resources()) {
                archive_name = normalize_filename_resource_name(archive_name, disk_format_);
                metadata.filename = archive_name;
                metadata.resref = resource_stem_from_text(filename_string(std::filesystem::path(archive_name)));
                metadata.restype = type_from_filename_extension_allow_unknown(archive_name, resource_type_profile_);
                if (disk_format_ == ArchiveDiskFormat::ErfV3_0) {
                    std::uint64_t parsed_hash = 0;
                    std::uint32_t parsed_type_hash = 0;
                    if (metadata.v3_name_hash == 0 && parse_da2_synthetic_resource_name(archive_name, parsed_hash, parsed_type_hash)) {
                        metadata.v3_filename_stripped = true;
                        metadata.v3_name_hash = parsed_hash;
                        metadata.v3_type_hash = parsed_type_hash;
                    }
                    if (metadata.v3_type_hash == 0) {
                        metadata.v3_type_hash = da2_type_hash_from_name(archive_name, 0);
                    }
                    if (metadata.restype == kUnknownType) {
                        metadata.restype = da2_restype_from_type_hash(metadata.v3_type_hash);
                    }
                }
            } else {
                archive_name = filename_string(std::filesystem::path(archive_name));
            }
            all_archive_names.push_back(std::move(archive_name));
            all_resource_metadata.push_back(std::move(metadata));
        };

        auto remember_direct_resource = [&](const Resource& resource, std::string archive_name) {
            Resource metadata = resource;
            if (filename_based_resources()) {
                archive_name = normalize_filename_resource_name(archive_name, disk_format_);
                metadata.filename = archive_name;
            } else {
                archive_name = filename_string(std::filesystem::path(archive_name));
            }
            all_files.emplace_back();
            all_file_identities.emplace_back();
            direct_resources.push_back(&resource);
            all_archive_names.push_back(std::move(archive_name));
            all_resource_metadata.push_back(std::move(metadata));
        };

        if (!newfile_) {
            const bool must_recompress_existing =
                disk_format_ == ArchiveDiskFormat::ErfV2_2 && erf_compression_scheme(header_.v2_flags) != 0;
            for (const auto& res : resources_) {
                const std::string archive_name = resource_filename_for_archive(res, resource_type_profile_);
                if (!must_recompress_existing) {
                    remember_direct_resource(res, archive_name);
                    continue;
                }
                const auto out_path = (temp_folder_ / save_staging_leaf(all_files.size(), archive_name));
                ExclusiveOutputFile out_file(out_path, OutputCreateMode::CreateNew, "temporary resource file");
                std::ostream& out = out_file.stream();
                if (res.data_size > 0 || res.packed_size > 0) {
                    ensure_archive_stream();
                    archive_stream_.clear();
                    copy_resource_payload_to_stream(archive_stream_, out, res, disk_format_, archive_flags());
                }
                out_file.close();
                const FileIdentity staged_identity = capture_regular_file_identity(out_path);
                remember_staged_file(out_path, staged_identity, archive_name, res);
            }
        }

        for (std::size_t i = 0; i < new_files_.size(); ++i) {
            try {
                ensure_same_regular_file_identity(new_files_[i], new_file_identities_.at(i), "Staged resource input validation failed");
            } catch (const std::exception& ex) {
                throw ErfError(ex.what());
            }
            if (!file_exists(new_files_[i])) {
                throw ErfError("Staged resource input file no longer exists: " + path_to_string(new_files_[i]));
            }
            ensure_staged_input_fits_archive_format(new_files_[i]);
            const std::string archive_name = filename_based_resources()
                ? normalize_filename_resource_name(new_names_.at(i), disk_format_)
                : filename_string(std::filesystem::path(new_names_.at(i)));
            const auto staged_dest = (temp_folder_ / save_staging_leaf(all_files.size(), archive_name));
            const FileIdentity staged_dest_identity = copy_file_create_new_limited(new_files_[i], staged_dest, std::numeric_limits<std::uint32_t>::max(), &new_file_identities_.at(i));
            Resource metadata;
            if (i < new_resource_metadata_.size()) {
                metadata = new_resource_metadata_[i];
            }
            remember_staged_file(staged_dest, staged_dest_identity, archive_name, metadata);
        }

        if (all_files.size() != direct_resources.size() ||
            all_files.size() != all_archive_names.size() ||
            all_files.size() != all_resource_metadata.size()) {
            throw ErfError("Internal save payload table mismatch.");
        }
        const bool is_erf_v2 = disk_format_ == ArchiveDiskFormat::ErfV2_0 || disk_format_ == ArchiveDiskFormat::ErfV2_2;
        const bool is_erf_v2_2 = disk_format_ == ArchiveDiskFormat::ErfV2_2;
        const bool is_erf_v3 = disk_format_ == ArchiveDiskFormat::ErfV3_0;
        const bool is_rim = !is_erf_v2 && !is_erf_v3 && (disk_format_ == ArchiveDiskFormat::RimV1 || header_.is_rim());

        std::vector<char> v3_string_table;
        std::vector<std::int32_t> v3_name_offsets;
        std::vector<std::uint64_t> v3_name_hashes;
        std::vector<std::uint32_t> v3_type_hashes;
        if (is_erf_v3) {
            v3_name_offsets.reserve(all_archive_names.size());
            v3_name_hashes.reserve(all_archive_names.size());
            v3_type_hashes.reserve(all_archive_names.size());
            for (std::size_t i = 0; i < all_archive_names.size(); ++i) {
                const std::string archive_name = normalize_archive_path(all_archive_names.at(i));
                const Resource& meta = all_resource_metadata.at(i);
                const bool keep_stripped = meta.v3_filename_stripped && meta.v3_type_hash != 0;
                if (keep_stripped) {
                    v3_name_offsets.push_back(-1);
                    v3_name_hashes.push_back(meta.v3_name_hash);
                    v3_type_hashes.push_back(meta.v3_type_hash);
                    continue;
                }

                validate_erf_v3_resource_name(archive_name);
                if (v3_string_table.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                    throw ErfError("ERF V3.0 string table exceeds the 31-bit name-offset limit.");
                }
                v3_name_offsets.push_back(static_cast<std::int32_t>(v3_string_table.size()));
                v3_name_hashes.push_back(fnv1_64_lower(archive_name));
                const std::uint32_t type_hash = da2_type_hash_from_name(archive_name, meta.v3_type_hash);
                if (type_hash == 0) {
                    throw ErfError("Unable to determine ERF V3.0 type hash for resource: " + archive_name);
                }
                v3_type_hashes.push_back(type_hash);
                v3_string_table.insert(v3_string_table.end(), archive_name.begin(), archive_name.end());
                v3_string_table.push_back('\0');
            }
        }

        if (is_erf_v2) {
            const auto [year, day] = current_erf_build_time();
            header_.filetype = make_header("ERF ");
            header_.version = make_header(is_erf_v2_2 ? "V2.2" : "V2.0");
            header_.entrycount = checked_u32(all_files.size(), is_erf_v2_2 ? "ERF V2.2 entry count" : "ERF V2.0 entry count");
            header_.buildyear = year;
            header_.buildday = day;
            header_.locstringcount = 0;
            header_.locstringsize = 0;
            header_.offsetlocstring = 0;
            header_.offsetkeylist = is_erf_v2_2 ? 56u : 32u;
            header_.offsetreslist = 0;
            if (is_erf_v2_2) {
                ensure_supported_erf_compression_flags(header_.v2_flags, "ERF V2.2");
                header_.v2_module_id = 0;
                header_.v2_password_digest = {};
            }
        } else if (is_erf_v3) {
            header_.filetype = make_header("ERF ");
            header_.version = make_header("V3.0");
            header_.entrycount = checked_u32(all_files.size(), "ERF V3.0 entry count");
            header_.v3_string_table_size = checked_u32(v3_string_table.size(), "ERF V3.0 string table size");
            header_.v3_flags = 0;
            header_.v3_module_id = 0;
            header_.v3_password_digest = {};
        } else if (is_rim) {
            ensure_rim_resid_fits_word(all_files.size(), "RIM entry count");
            header_.entrycount = checked_u32(all_files.size(), "RIM entry count");
            header_.offsetkeylist = 120;
        } else {
            const auto [year, day] = current_erf_build_time();
            header_.locstringcount = checked_u32(locstrings_.size(), "Localized string count");
            std::uint64_t loc_size = 0;
            for (const auto& loc : locstrings_) {
                loc_size += loc.text.size();
                loc_size += 8;
            }
            header_.locstringsize = checked_u32(loc_size, "Localized string section size");
            header_.entrycount = checked_u32(all_files.size(), "ERF entry count");
            header_.buildyear = year;
            header_.buildday = day;
            header_.offsetlocstring = 160;
            header_.offsetkeylist = checked_add_u32(header_.offsetlocstring, header_.locstringsize, "ERF key-list offset");
            header_.offsetreslist = checked_add_u32(
                header_.offsetkeylist,
                checked_mul_u32(header_.entrycount, resref32_ ? 40u : 24u, "ERF key-list byte size"),
                "ERF resource-list offset");
        }


        FileIdentity output_identity{};
        ExclusiveOutputFile out_file(output_temp, OutputCreateMode::CreateNew, "temporary output archive");
        output_identity = out_file.identity();
        output_guard.activate(output_identity);
        std::ostream& out = out_file.stream();

        auto unpacked_payload_size = [&](std::size_t index) -> std::uint64_t {
            if (direct_resources.at(index) != nullptr) return direct_resources.at(index)->data_size;
            return all_file_identities.at(index).size;
        };
        auto write_payload = [&](std::ostream& destination, std::size_t index) -> std::uint64_t {
            if (const Resource* source = direct_resources.at(index)) {
                ensure_archive_stream();
                archive_stream_.clear();
                copy_resource_payload_to_stream(archive_stream_, destination, *source, disk_format_, archive_flags());
                return source->data_size;
            }
            return write_file_payload(destination, all_files.at(index), &all_file_identities.at(index));
        };

        if (is_erf_v2) {
            write_erf_v2_magic(out, is_erf_v2_2 ? '2' : '0');
            write_u32(out, header_.entrycount);
            write_u32(out, header_.buildyear);
            write_u32(out, header_.buildday);
            write_u32(out, header_.v2_unknown);
            if (is_erf_v2_2) {
                write_u32(out, header_.v2_flags);
                write_u32(out, header_.v2_module_id);
                write_byte_array(out, header_.v2_password_digest);
            }
        } else if (is_erf_v3) {
            write_erf_v3_magic(out);
            write_u32(out, header_.v3_string_table_size);
            write_u32(out, header_.entrycount);
            write_u32(out, header_.v3_flags);
            write_u32(out, header_.v3_module_id);
            write_byte_array(out, header_.v3_password_digest);
        } else {
            write_char_array(out, header_.filetype);
            write_char_array(out, header_.version);
            if (is_rim) {
                write_u32(out, header_.rim_unknown);
                write_u32(out, header_.entrycount);
                write_u32(out, header_.offsetkeylist);
                write_byte_array(out, header_.rim_reserved);
            } else {
                write_u32(out, header_.locstringcount);
                write_u32(out, header_.locstringsize);
                write_u32(out, header_.entrycount);
                write_u32(out, header_.offsetlocstring);
                write_u32(out, header_.offsetkeylist);
                write_u32(out, header_.offsetreslist);
                write_u32(out, header_.buildyear);
                write_u32(out, header_.buildday);
                write_u32(out, header_.locstringstrref);
                write_byte_array(out, header_.erf_reserved);
            }
        }

        if (!is_rim && !is_erf_v2 && !is_erf_v3) {
            seekp_from_u32_offset(out, header_.offsetlocstring);
            for (const auto& loc : locstrings_) {
                write_u32(out, loc.language_id);
                write_u32(out, loc.size());
                if (!loc.text.empty()) {
                    checked_direct_write(out, loc.text.data(), static_cast<std::streamsize>(loc.text.size()), "localized string bytes");
                }
            }
        }

        if (is_erf_v2) {
            const std::uint32_t header_size = is_erf_v2_2 ? 56u : 32u;
            const std::uint32_t toc_entry_size = is_erf_v2_2 ? 76u : 72u;
            const char* version_label = is_erf_v2_2 ? "ERF V2.2" : "ERF V2.0";
            const bool compress_v2_payloads = is_erf_v2_2 && erf_compression_scheme(header_.v2_flags) != 0;
            std::uint32_t toc_offset = header_size;
            std::uint32_t data_offset = checked_add_u32(
                header_size,
                checked_mul_u32(header_.entrycount, toc_entry_size,
                                is_erf_v2_2 ? "ERF V2.2 table of contents byte size" : "ERF V2.0 table of contents byte size"),
                is_erf_v2_2 ? "ERF V2.2 data offset" : "ERF V2.0 data offset");
            for (std::size_t i = 0; i < all_files.size(); ++i) {
                const auto& file = all_files[i];
                const std::string save_leaf = normalize_archive_leaf(all_archive_names.at(i));
                validate_erf_v2_resource_leaf(save_leaf);

                seekp_from_u32_offset(out, toc_offset);
                write_fixed_utf16le_string(out, save_leaf, 32, "resource filename");
                write_u32(out, data_offset);
                const std::uint32_t packed_size_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), std::string(version_label) + " packed resource size offset");
                write_u32(out, 0);
                std::uint32_t unpacked_size_offset = 0;
                if (is_erf_v2_2) {
                    unpacked_size_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF V2.2 unpacked resource size offset");
                    write_u32(out, 0);
                }
                toc_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF V2.0 TOC offset");

                seekp_from_u32_offset(out, data_offset);
                const std::uint64_t unpacked_size = unpacked_payload_size(i);
                const std::uint64_t stored_size = compress_v2_payloads
                    ? write_file_payload_zlib(out, file, erf_compression_scheme(header_.v2_flags), &all_file_identities.at(i))
                    : write_payload(out, i);
                const std::uint32_t packed_u32 = checked_u32(stored_size, std::string(version_label) + " packed resource size");
                const std::uint32_t unpacked_u32 = checked_u32(unpacked_size, std::string(version_label) + " unpacked resource size");
                data_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), std::string(version_label) + " data offset");

                seekp_from_u32_offset(out, packed_size_offset);
                write_u32(out, is_erf_v2_2 ? packed_u32 : unpacked_u32);
                if (is_erf_v2_2) {
                    seekp_from_u32_offset(out, unpacked_size_offset);
                    write_u32(out, unpacked_u32);
                }
            }
        } else if (is_erf_v3) {
            if (!v3_string_table.empty()) {
                seekp_from_u32_offset(out, 48u);
                checked_direct_write(out, v3_string_table.data(), static_cast<std::streamsize>(v3_string_table.size()), "ERF V3.0 string table");
            }
            std::uint32_t toc_offset = checked_add_u32(48u, header_.v3_string_table_size, "ERF V3.0 TOC offset");
            std::uint32_t data_offset = align_u32(
                checked_add_u32(toc_offset,
                                checked_mul_u32(header_.entrycount, 28u, "ERF V3.0 table of contents byte size"),
                                "ERF V3.0 data offset"),
                4u,
                "ERF V3.0 aligned data offset");

            for (std::size_t i = 0; i < all_files.size(); ++i) {
                const auto& file = all_files[i];
                seekp_from_u32_offset(out, toc_offset);
                write_i32(out, v3_name_offsets.at(i));
                write_u64(out, v3_name_hashes.at(i));
                write_u32(out, v3_type_hashes.at(i));
                write_u32(out, data_offset);
                const std::uint32_t packed_size_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF V3.0 packed-size offset");
                write_u32(out, 0);
                const std::uint32_t unpacked_size_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF V3.0 unpacked-size offset");
                write_u32(out, 0);
                toc_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF V3.0 TOC offset");

                seekp_from_u32_offset(out, data_offset);
                const auto copied = write_payload(out, i);
                const std::uint32_t copied_u32 = checked_u32(copied, "ERF V3.0 resource data size");
                write_zero_padding_to_alignment(out, 4u);
                data_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF V3.0 data offset");

                seekp_from_u32_offset(out, packed_size_offset);
                write_u32(out, copied_u32);
                seekp_from_u32_offset(out, unpacked_size_offset);
                write_u32(out, copied_u32);
            }
        } else if (is_rim) {
            std::uint32_t key_offset = header_.offsetkeylist;
            std::uint32_t data_offset = checked_add_u32(
                header_.offsetkeylist,
                checked_mul_u32(header_.entrycount, resref32_ ? 48u : 32u, "RIM key/resource table byte size"),
                "RIM data offset");
            for (std::size_t i = 0; i < all_files.size(); ++i) {
                const auto& file = all_files[i];
                const std::string save_leaf = filename_string(std::filesystem::path(all_archive_names.at(i)));
                const std::string ext_with_dot = extension_string(std::filesystem::path(save_leaf));
                const std::string name = resource_stem_from_text(save_leaf);
                std::string ext = ext_with_dot.empty() ? std::string() : ext_with_dot.substr(1);
                ext = ascii_lower(ext);

                Resource res;
                res.extended = resref32_;
                res.resref = name;
                res.resid = static_cast<std::uint32_t>(i);
                res.restype = Resource::string_to_res_type(ext, resource_type_profile_);

                seekp_from_u32_offset(out, key_offset);
                if (resref32_) {
                    write_char_array(out, res.raw_resref32());
                } else {
                    write_char_array(out, res.raw_resref());
                }
                write_u16(out, res.restype);
                write_u16(out, res.reserved_erf);
                write_u16(out, static_cast<std::uint16_t>(i));
                write_u16(out, static_cast<std::uint16_t>(res.reserved_rim));
                key_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "RIM key offset");

                seekp_from_u32_offset(out, data_offset);
                res.data_offset = data_offset;
                const auto copied = write_payload(out, i);
                res.data_size = checked_u32(copied, "Resource data size");
                data_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "RIM data offset");

                seekp_from_u32_offset(out, key_offset);
                write_u32(out, res.data_offset);
                write_u32(out, res.data_size);
                key_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "RIM key offset");
            }
        } else {
            std::uint32_t key_offset = header_.offsetkeylist;
            std::uint32_t res_offset = header_.offsetreslist;
            std::uint32_t data_offset = checked_add_u32(
                header_.offsetreslist,
                checked_mul_u32(header_.entrycount, 8u, "ERF resource-list byte size"),
                "ERF data offset");
            for (std::size_t i = 0; i < all_files.size(); ++i) {
                const auto& file = all_files[i];
                const std::string save_leaf = filename_string(std::filesystem::path(all_archive_names.at(i)));
                const std::string ext_with_dot = extension_string(std::filesystem::path(save_leaf));
                const std::string name = resource_stem_from_text(save_leaf);
                std::string ext = ext_with_dot.empty() ? std::string() : ext_with_dot.substr(1);
                ext = ascii_lower(ext);

                Resource res;
                res.extended = resref32_;
                res.resref = name;
                res.resid = static_cast<std::uint32_t>(i);
                res.restype = Resource::string_to_res_type(ext, resource_type_profile_);

                seekp_from_u32_offset(out, key_offset);
                if (resref32_) {
                    write_char_array(out, res.raw_resref32());
                } else {
                    write_char_array(out, res.raw_resref());
                }
                write_u32(out, res.resid);
                write_u16(out, res.restype);
                write_u16(out, res.reserved_erf);
                key_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF key offset");

                seekp_from_u32_offset(out, data_offset);
                res.data_offset = data_offset;
                const auto copied = write_payload(out, i);
                res.data_size = checked_u32(copied, "Resource data size");
                data_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF data offset");

                seekp_from_u32_offset(out, res_offset);
                write_u32(out, res.data_offset);
                write_u32(out, res.data_size);
                res_offset = checked_u32(static_cast<std::uintmax_t>(out.tellp()), "ERF resource offset");
            }
        }
        out_file.close();

        if (archive_stream_.is_open()) {
            archive_stream_.close();
        }
#if defined(_WIN32)
        close_loaded_archive_lock(archive_lock_handle_);
#endif
        archive_stream_reference_dangling_ = false;
        for (std::size_t i=0;i<new_files_.size();++i) {
            if (!same_regular_file_revision(new_files_[i],new_file_identities_.at(i)))
                throw ErfError("Staged resource changed; add it again before saving: " + path_to_string(new_files_[i]));
        }
        ensure_save_target_does_not_overwrite_staged_inputs(filename, new_files_);
        if (!same_regular_file_identity(output_temp, output_identity)) {
            throw ErfError("Temporary archive output was replaced before final archive replacement: " + path_to_string(output_temp));
        }
        replace_file_atomically(output_temp, output_identity, filename, save_target_state);
        output_guard.release();

        temp_guard.cleanup_now_noexcept();
        load(filename);
    } catch (...) {
        if (had_header_before_save) {
            header_ = header_before_save;
        }
        archive_stream_reference_dangling_ = false;
        if (!newfile_ && !filename_.empty() && !archive_stream_.is_open() && file_exists(filename_)) {
#if defined(_WIN32)
            archive_lock_handle_ = open_loaded_archive_lock(filename_);
#endif
            open_binary_input_stream(archive_stream_, filename_);
            if (!archive_stream_) {
                archive_stream_.close();
            }
        }
        throw;
    }
}

void ErfArchive::add_resource(const std::filesystem::path& filename, bool replace, std::string save_as) {
    ensure_loaded_for_operation("Unable to add resource to file, no ERF file is open!");

    if (save_as.empty()) {
        save_as = filename_string(filename);
    }

    // Fail before mutating the in-memory archive model. The save path already
    // fails closed if staged inputs disappear, but AddResource replacement could
    // otherwise delete an existing resource from the loaded model before finding
    // out that the replacement path is missing, non-regular, or too large for
    // the 32-bit archive payload field.
    ensure_staged_input_fits_archive_format(filename);
    const std::filesystem::path staged_source = stable_staged_input_path(filename);
    FileIdentity staged_identity{};
    try {
        staged_identity = capture_regular_file_identity(staged_source);
    } catch (const std::exception& ex) {
        throw ErfError(std::string("Unable to bind staged resource input file: ") + path_to_string(staged_source) + ": " + ex.what());
    }

    if (filename_based_resources()) {
        const std::string save_name = normalize_filename_resource_name(save_as, disk_format_);
        if (disk_format_ == ArchiveDiskFormat::ErfV2_0 || disk_format_ == ArchiveDiskFormat::ErfV2_2) {
            validate_erf_v2_resource_leaf(save_name);
        } else if (disk_format_ == ArchiveDiskFormat::ErfV3_0) {
            validate_erf_v3_resource_name(save_name);
        }

        const std::string target_key = normalized_filename_resource_key(save_name, disk_format_);
        Resource replacement_metadata;
        bool have_replacement_metadata = false;
        for (auto it = resources_.begin(); it != resources_.end(); ++it) {
            if (normalized_filename_resource_key(resource_filename_for_archive(*it, resource_type_profile_), disk_format_) == target_key) {
                if (!replace) {
                    throw ErfError("Cannot add resource \"" + save_name + "\"! A file with this name already exists in the ERF!");
                }
                replacement_metadata = *it;
                have_replacement_metadata = true;
                resources_.erase(it);
                --header_.entrycount;
                break;
            }
        }
        for (std::size_t i = 0; i < new_names_.size(); ++i) {
            if (normalized_filename_resource_key(new_names_[i], disk_format_) == target_key) {
                if (!replace) {
                    throw ErfError("Cannot add resource \"" + save_name + "\"! A file with this name has already been added to the ERF!");
                }
                new_files_.erase(new_files_.begin() + static_cast<std::ptrdiff_t>(i));
                new_file_identities_.erase(new_file_identities_.begin() + static_cast<std::ptrdiff_t>(i));
                new_names_.erase(new_names_.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < new_resource_metadata_.size()) {
                    new_resource_metadata_.erase(new_resource_metadata_.begin() + static_cast<std::ptrdiff_t>(i));
                }
                break;
            }
        }

        Resource metadata;
        if (have_replacement_metadata && disk_format_ == ArchiveDiskFormat::ErfV3_0 && replacement_metadata.v3_filename_stripped) {
            metadata = replacement_metadata;
        }
        metadata.filename = save_name;
        metadata.resref = resource_stem_from_text(filename_string(std::filesystem::path(save_name)));
        metadata.restype = type_from_filename_extension_allow_unknown(save_name, resource_type_profile_);
        if (disk_format_ == ArchiveDiskFormat::ErfV3_0) {
            std::uint64_t parsed_hash = 0;
            std::uint32_t parsed_type_hash = 0;
            if (!metadata.v3_filename_stripped && parse_da2_synthetic_resource_name(save_name, parsed_hash, parsed_type_hash)) {
                metadata.v3_filename_stripped = true;
                metadata.v3_name_hash = parsed_hash;
                metadata.v3_type_hash = parsed_type_hash;
            }
            if (metadata.v3_type_hash == 0) {
                metadata.v3_type_hash = da2_type_hash_from_name(save_name, 0);
            }
            if (metadata.restype == kUnknownType) {
                metadata.restype = da2_restype_from_type_hash(metadata.v3_type_hash);
            }
        }

        new_files_.push_back(staged_source);
        new_file_identities_.push_back(staged_identity);
        new_names_.push_back(save_name);
        new_resource_metadata_.push_back(metadata);
        dirty_ = true;
        return;
    }

    const std::string save_leaf = normalize_archive_leaf(save_as);
    const std::string ext_with_dot = extension_string(save_leaf);
    std::string name = ascii_lower(resource_stem_from_text(save_leaf));
    std::string ext = ext_with_dot.empty() ? std::string() : ext_with_dot.substr(1);
    ext = ascii_lower(ext);
    const std::uint16_t type = Resource::string_to_res_type(ext, resource_type_profile_);
    const std::string archive_resref = string_to_resref(name, resref32_);

    if (type == kUnknownType) {
        throw ErfError("Cannot add resource \"" + name + "\"! Unsupported file type \"" + ext + "\" encountered!");
    }
    if (archive_resref.empty()) {
        throw ErfError("Cannot add resource \"" + name + "." + ext + "\"! The resource name sanitizes to an empty ResRef.");
    }

    if (!archive_resref.empty()) {
        for (std::size_t i = 0; i < resources_.size(); ++i) {
            if (ascii_lower(resources_[i].resref) == archive_resref && resources_[i].restype == type) {
                if (!replace) {
                    throw ErfError("Cannot add resource \"" + name + "." + ext + "\"! A file with this name already exists in the ERF!");
                }
                delete_resource(archive_resref, type);
                break;
            }
        }
    }

    for (std::size_t i = 0; i < new_names_.size(); ++i) {
        if (staged_logical_name_matches(std::filesystem::path(new_names_[i]), archive_resref, type, resref32_, resource_type_profile_)) {
            if (!replace) {
                throw ErfError("Cannot add resource \"" + name + "." + ext + "\"! A file with this name has already been added to the ERF!");
            }
            new_files_.erase(new_files_.begin() + static_cast<std::ptrdiff_t>(i));
            new_file_identities_.erase(new_file_identities_.begin() + static_cast<std::ptrdiff_t>(i));
            new_names_.erase(new_names_.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < new_resource_metadata_.size()) {
                new_resource_metadata_.erase(new_resource_metadata_.begin() + static_cast<std::ptrdiff_t>(i));
            }
            break;
        }
    }

    new_files_.push_back(staged_source);
    new_file_identities_.push_back(staged_identity);
    new_names_.push_back(save_leaf);
    new_resource_metadata_.push_back(Resource{});
    dirty_ = true;
}


std::vector<std::uint8_t> ErfArchive::read_resource(const std::string& resref,
                                                    std::uint16_t res_type) {
    ensure_loaded_for_operation("Unable to read resource from file, no ERF file is open!");

    const Resource* resource = find_resource(resref, res_type);
    if (resource == nullptr) {
        throw ErfError("Resource \"" + resref + "." +
                       Resource::res_type_to_string(res_type, resource_type_profile_) +
                       "\" could not be found in the ERF file. Unable to extract it!");
    }

    ensure_archive_stream();
    archive_stream_.clear();
    try {
        auto bytes = read_resource_payload_bytes(
            archive_stream_, *resource, disk_format_, archive_flags());
        archive_stream_.discard_cache();
        if (!reader_backed_ && !same_regular_file_revision(filename_, loaded_file_identity_)) {
            throw ErfError("Archive changed during extraction; discard this result and reopen the archive.");
        }
        return bytes;
    } catch (...) {
        archive_stream_.discard_cache();
        throw;
    }
}

std::vector<std::uint8_t> ErfArchive::read_resource(std::size_t index) {
    ensure_loaded_for_operation("Unable to read resource from file, no ERF file is open!");
    if (index >= resources_.size()) {
        throw ErfError("Cannot read resource! Array index out of bounds!");
    }

    ensure_archive_stream();
    archive_stream_.clear();
    try {
        auto bytes = read_resource_payload_bytes(
            archive_stream_, resources_[index], disk_format_, archive_flags());
        archive_stream_.discard_cache();
        if (!reader_backed_ && !same_regular_file_revision(filename_, loaded_file_identity_)) {
            throw ErfError("Archive changed during extraction; discard this result and reopen the archive.");
        }
        return bytes;
    } catch (...) {
        archive_stream_.discard_cache();
        throw;
    }
}

std::vector<std::uint8_t> ErfArchive::read_resource_by_name(
    const std::string& resource_name) {
    ensure_loaded_for_operation("Unable to read resource from file, no ERF file is open!");

    const Resource* resource = find_resource_by_name(resource_name);
    if (resource == nullptr) {
        throw ErfError("Resource \"" + normalize_archive_leaf(resource_name) +
                       "\" could not be found in the ERF file. Unable to extract it!");
    }

    ensure_archive_stream();
    archive_stream_.clear();
    try {
        auto bytes = read_resource_payload_bytes(
            archive_stream_, *resource, disk_format_, archive_flags());
        archive_stream_.discard_cache();
        if (!reader_backed_ && !same_regular_file_revision(filename_, loaded_file_identity_)) {
            throw ErfError("Archive changed during extraction; discard this result and reopen the archive.");
        }
        return bytes;
    } catch (...) {
        archive_stream_.discard_cache();
        throw;
    }
}

std::optional<std::size_t> ErfArchive::staged_index_for_name(const std::string& name) const {
    if (filename_based_resources()) {
        const auto key = normalized_filename_resource_key(name, disk_format_);
        for (std::size_t i=0;i<new_names_.size();++i)
            if (normalized_filename_resource_key(new_names_[i], disk_format_) == key) return i;
    } else {
        const auto leaf = normalize_archive_leaf(name);
        const auto ext = extension_string(leaf);
        const auto type = Resource::string_to_res_type(ext.empty() ? std::string{} : ext.substr(1), resource_type_profile_);
        const auto resref = string_to_resref(ascii_lower(resource_stem_from_text(leaf)), resref32_);
        for (std::size_t i=0;i<new_names_.size();++i)
            if (staged_logical_name_matches(std::filesystem::path(new_names_[i]), resref, type, resref32_, resource_type_profile_)) return i;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> ErfArchive::read_current_resource(const std::string& name) {
    ensure_loaded_for_operation("No archive is open.");
    if (const auto pending = staged_index_for_name(name)) {
        const auto& path = new_files_.at(*pending);
        const auto& identity = new_file_identities_.at(*pending);
        if (!same_regular_file_revision(path, identity))
            throw ErfError("Staged input changed; add the replacement again: " + path_to_string(path));
        if (identity.size > std::numeric_limits<std::size_t>::max() ||
            identity.size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            throw ErfError("Resource is too large for an in-memory editor.");
        std::ifstream input(path, std::ios::binary);
        std::vector<std::uint8_t> result(static_cast<std::size_t>(identity.size));
        if (!input || (!result.empty() && !input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()))))
            throw ErfError("Unable to read staged resource: " + path_to_string(path));
        if (!same_regular_file_revision(path, identity))
            throw ErfError("Staged input changed during reading; discard the result and add it again.");
        return result;
    }
    if (filename_based_resources()) return read_resource_by_name(name);
    const auto leaf=normalize_archive_leaf(name), ext=extension_string(leaf);
    return read_resource(resource_stem_from_text(leaf), Resource::string_to_res_type(ext.empty()?std::string{}:ext.substr(1), resource_type_profile_));
}

void ErfArchive::extract_current_resource(const std::string& name, const std::filesystem::path& output) {
    ensure_loaded_for_operation("No archive is open.");
    if (output.empty()) throw ErfError("Choose an extraction destination.");
    if (const auto pending = staged_index_for_name(name)) {
        const auto& source=new_files_.at(*pending);
        const auto identity=new_file_identities_.at(*pending);
        if (!same_regular_file_revision(source, identity)) throw ErfError("Staged input changed; add it again before extracting.");
        ensure_extract_target_does_not_overwrite_inputs(output, filename_, new_files_);
        const auto target=capture_replacement_target_state(output);
        const auto temp=make_unique_output_temp_path(output);
        ScopedTempOutputFile guard(temp);
        const auto tempIdentity=copy_file_create_new_limited(source,temp,std::numeric_limits<std::uint32_t>::max(),&identity);
        guard.activate(tempIdentity);
        if (!same_regular_file_revision(source, identity)) throw ErfError("Staged input changed during extraction.");
        ensure_extract_target_does_not_overwrite_inputs(output, filename_, new_files_);
        replace_file_atomically(temp,tempIdentity,output,target);
        guard.release();
        return;
    }
    if (filename_based_resources()) { get_resource_by_name(name,output); return; }
    const auto leaf=normalize_archive_leaf(name),ext=extension_string(leaf);
    get_resource(resource_stem_from_text(leaf),Resource::string_to_res_type(ext.empty()?std::string{}:ext.substr(1),resource_type_profile_),output);
}

void ErfArchive::get_resource(const std::string& resref, std::uint16_t res_type, std::filesystem::path filename) {
    ensure_loaded_for_operation("Unable to get resource from file, no ERF file is open!");

    const Resource* res = find_resource(resref, res_type);
    if (res == nullptr) {
        throw ErfError("Resource \"" + resref + "." + Resource::res_type_to_string(res_type, resource_type_profile_) + "\" could not be found in the ERF file. Unable to extract it!");
    }

    if (filename.empty()) {
        std::string out_name = res->resref;
        const std::string ext = Resource::res_type_to_string(res->restype, resource_type_profile_);
        if (!ext.empty()) {
            out_name += "." + ext;
        }
        filename = default_temp_root() / out_name;
    }

    ensure_extract_target_does_not_overwrite_inputs(filename, filename_, new_files_);
    const ReplacementTargetState extract_target_state = capture_replacement_target_state(filename);

    const std::filesystem::path output_temp = make_unique_output_temp_path(filename);
    ScopedTempOutputFile output_guard(output_temp);
    FileIdentity output_identity{};
    {
        ExclusiveOutputFile out_file(output_temp, OutputCreateMode::CreateNew, "temporary output resource file");
        output_identity = out_file.identity();
        output_guard.activate(output_identity);
        std::ostream& out = out_file.stream();
        if (res->data_size > 0 || ((disk_format_ == ArchiveDiskFormat::ErfV2_2 || disk_format_ == ArchiveDiskFormat::ErfV3_0) && res->packed_size > 0)) {
            ensure_archive_stream();
            archive_stream_.clear();
            copy_resource_payload_to_stream(archive_stream_, out, *res, disk_format_, archive_flags());
        }
        out_file.close();
    }
    ensure_extract_target_does_not_overwrite_inputs(filename, filename_, new_files_);
    if (!same_regular_file_identity(output_temp, output_identity)) {
        throw ErfError("Temporary extraction output was replaced before final destination replacement: " + path_to_string(output_temp));
    }
    replace_file_atomically(output_temp, output_identity, filename, extract_target_state);
    output_guard.release();
}



void ErfArchive::get_resource(std::size_t index, std::filesystem::path filename) {
    ensure_loaded_for_operation("Unable to get resource from file, no ERF file is open!");
    if (index >= resources_.size()) {
        throw ErfError("Resource index is out of range. Unable to extract it!");
    }

    const Resource& res = resources_[index];
    if (filename.empty()) {
        filename = default_temp_root() / resource_filename_for_archive(res, resource_type_profile_);
    }

    ensure_extract_target_does_not_overwrite_inputs(filename, filename_, new_files_);
    const ReplacementTargetState extract_target_state = capture_replacement_target_state(filename);
    const std::filesystem::path output_temp = make_unique_output_temp_path(filename);
    ScopedTempOutputFile output_guard(output_temp);
    FileIdentity output_identity{};
    {
        ExclusiveOutputFile out_file(output_temp, OutputCreateMode::CreateNew, "temporary output resource file");
        output_identity = out_file.identity();
        output_guard.activate(output_identity);
        std::ostream& out = out_file.stream();
        if (res.data_size > 0 || ((disk_format_ == ArchiveDiskFormat::ErfV2_2 || disk_format_ == ArchiveDiskFormat::ErfV3_0) && res.packed_size > 0)) {
            ensure_archive_stream();
            archive_stream_.clear();
            copy_resource_payload_to_stream(archive_stream_, out, res, disk_format_, archive_flags());
        }
        out_file.close();
    }
    replace_file_atomically(output_temp, output_identity, filename, extract_target_state);
    output_guard.release();
}

void ErfArchive::get_resource_by_name(const std::string& resource_name, std::filesystem::path filename) {
    ensure_loaded_for_operation("Unable to get resource from file, no ERF file is open!");

    const Resource* res = find_resource_by_name(resource_name);
    if (res == nullptr) {
        throw ErfError("Resource \"" + normalize_archive_leaf(resource_name) + "\" could not be found in the ERF file. Unable to extract it!");
    }

    if (filename.empty()) {
        filename = default_temp_root() / resource_filename_for_archive(*res, resource_type_profile_);
    }

    ensure_extract_target_does_not_overwrite_inputs(filename, filename_, new_files_);
    const ReplacementTargetState extract_target_state = capture_replacement_target_state(filename);

    const std::filesystem::path output_temp = make_unique_output_temp_path(filename);
    ScopedTempOutputFile output_guard(output_temp);
    FileIdentity output_identity{};
    {
        ExclusiveOutputFile out_file(output_temp, OutputCreateMode::CreateNew, "temporary output resource file");
        output_identity = out_file.identity();
        output_guard.activate(output_identity);
        std::ostream& out = out_file.stream();
        if (res->data_size > 0 || ((disk_format_ == ArchiveDiskFormat::ErfV2_2 || disk_format_ == ArchiveDiskFormat::ErfV3_0) && res->packed_size > 0)) {
            ensure_archive_stream();
            archive_stream_.clear();
            copy_resource_payload_to_stream(archive_stream_, out, *res, disk_format_, archive_flags());
        }
        out_file.close();
    }
    ensure_extract_target_does_not_overwrite_inputs(filename, filename_, new_files_);
    if (!same_regular_file_identity(output_temp, output_identity)) {
        throw ErfError("Temporary extraction output was replaced before final destination replacement: " + path_to_string(output_temp));
    }
    replace_file_atomically(output_temp, output_identity, filename, extract_target_state);
    output_guard.release();
}


void ErfArchive::delete_resource(const std::string& resref, std::uint16_t res_type) {
    ensure_loaded_for_operation("Unable to delete resource from file, no ERF file is open!");

    const std::string target = ascii_lower(resref);
    for (auto it = resources_.begin(); it != resources_.end(); ++it) {
        if (ascii_lower(it->resref) == target && it->restype == res_type) {
            resources_.erase(it);
            // calls dec(Header.entrycount) unconditionally.
            --header_.entrycount;
            dirty_ = true;
            return;
        }
    }

    if (!new_files_.empty()) {
        const std::string logical_resref = strip_staged_marker_for_delete(resref);
        for (std::size_t i = 0; i < new_names_.size(); ++i) {
            if (staged_logical_name_matches(std::filesystem::path(new_names_[i]), logical_resref, res_type, resref32_, resource_type_profile_)) {
                new_files_.erase(new_files_.begin() + static_cast<std::ptrdiff_t>(i));
                new_file_identities_.erase(new_file_identities_.begin() + static_cast<std::ptrdiff_t>(i));
                new_names_.erase(new_names_.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < new_resource_metadata_.size()) {
                    new_resource_metadata_.erase(new_resource_metadata_.begin() + static_cast<std::ptrdiff_t>(i));
                }
                dirty_ = true;
                return;
            }
        }
    }

    throw ErfError("Unable to delete resource from file, resource \"" + resref + "\" not found!");
}

void ErfArchive::delete_resource_by_name(const std::string& resource_name) {
    ensure_loaded_for_operation("Unable to delete resource from file, no ERF file is open!");

    const std::string target = normalized_filename_resource_key(resource_name, disk_format_);
    for (auto it = resources_.begin(); it != resources_.end(); ++it) {
        if (normalized_filename_resource_key(resource_filename_for_archive(*it, resource_type_profile_), disk_format_) == target) {
            resources_.erase(it);
            --header_.entrycount;
            dirty_ = true;
            return;
        }
    }

    if (!new_files_.empty()) {
        for (std::size_t i = 0; i < new_names_.size(); ++i) {
            if (normalized_filename_resource_key(new_names_[i], disk_format_) == target) {
                new_files_.erase(new_files_.begin() + static_cast<std::ptrdiff_t>(i));
                new_file_identities_.erase(new_file_identities_.begin() + static_cast<std::ptrdiff_t>(i));
                new_names_.erase(new_names_.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < new_resource_metadata_.size()) {
                    new_resource_metadata_.erase(new_resource_metadata_.begin() + static_cast<std::ptrdiff_t>(i));
                }
                dirty_ = true;
                return;
            }
        }
    }

    throw ErfError("Unable to delete resource from file, resource \"" + normalize_archive_leaf(resource_name) + "\" not found!");
}


bool ErfArchive::resource_exists(const std::string& filename, bool check_new) const {
    ensure_loaded_for_operation("Unable to get resource from file, no ERF file is open!");

    if (filename_based_resources()) {
        return resource_exists_by_name(filename, check_new);
    }

    const std::string ext_with_dot = extension_string(filename);
    std::string resref = ascii_lower(resource_stem_from_text(filename));
    resref = string_to_resref(resref, resref32_);
    const std::uint16_t type = Resource::string_to_res_type(ext_with_dot, resource_type_profile_);

    for (const auto& res : resources_) {
        if (ascii_lower(res.resref) == resref && res.restype == type) {
            return true;
        }
    }

    if (check_new) {
        for (std::size_t i = 0; i < new_names_.size(); ++i) {
            if (staged_logical_name_matches(std::filesystem::path(new_names_[i]), resref, type, resref32_, resource_type_profile_)) {
                return true;
            }
        }
    }

    return false;
}

bool ErfArchive::resource_exists_by_name(const std::string& resource_name, bool check_new) const {
    ensure_loaded_for_operation("Unable to get resource from file, no ERF file is open!");

    const std::string target = normalized_filename_resource_key(resource_name, disk_format_);
    for (const auto& res : resources_) {
        if (normalized_filename_resource_key(resource_filename_for_archive(res, resource_type_profile_), disk_format_) == target) {
            return true;
        }
    }

    if (check_new) {
        for (std::size_t i = 0; i < new_names_.size(); ++i) {
            if (normalized_filename_resource_key(new_names_[i], disk_format_) == target) {
                return true;
            }
        }
    }

    return false;
}

std::string ErfArchive::file_type() const {
    if (!has_header_) {
        // file_type dereferences f_header; calling it without a header
        // is an invalid object-state access, not an empty-string result.
        throw ErfError("Cannot get archive file type before a header is loaded.");
    }
    std::string out;
    out.reserve(4);
    for (std::size_t i = 0; i < header_.filetype.size(); ++i) {
        if (i < header_.filetype.size() - 1 || header_.filetype[i] != ' ') {
            out.push_back(header_.filetype[i]);
        }
    }
    return out;
}

std::string ErfArchive::version() const {
    if (!has_header_) {
        throw ErfError("Cannot get archive version before a header is loaded.");
    }
    std::string out(header_.version.begin(), header_.version.end());
    while (!out.empty() && (out.back() == '\0' || out.back() == ' ')) out.pop_back();
    return out;
}

std::uint32_t ErfArchive::archive_flags() const noexcept {
    if (disk_format_ == ArchiveDiskFormat::ErfV2_2) {
        return header_.v2_flags;
    }
    if (disk_format_ == ArchiveDiskFormat::ErfV3_0) {
        return header_.v3_flags;
    }
    return 0;
}

std::uint32_t ErfArchive::compression_scheme() const noexcept {
    return erf_compression_scheme(archive_flags());
}

std::uint32_t ErfArchive::encryption_scheme() const noexcept {
    return erf_encryption_scheme(archive_flags());
}

std::size_t ErfArchive::count_new() const {
    if (!new_lists_allocated_) {
        // Reset(true) frees new-resource lists and does not recreate them.
        // new-resource count then dereferences a nil string list. Represent
        // that source-observable invalid state instead of returning a fabricated 0.
        throw std::runtime_error("new-resource count accessed after Reset(true)");
    }
    return new_files_.size();
}

const Resource& ErfArchive::resource(std::size_t index) const {
    if (index >= resources_.size()) {
        throw ErfError("Cannot get resource info! Array index out of bounds!");
    }
    return resources_[index];
}

const Resource* ErfArchive::find_resource(const std::string& resref, std::uint16_t res_type) const {
    const std::string target = ascii_lower(resref);
    for (const auto& res : resources_) {
        if (ascii_lower(res.resref) == target && res.restype == res_type) {
            return &res;
        }
    }
    return nullptr;
}

Resource* ErfArchive::find_resource(const std::string& resref, std::uint16_t res_type) {
    const std::string target = ascii_lower(resref);
    for (auto& res : resources_) {
        if (ascii_lower(res.resref) == target && res.restype == res_type) {
            return &res;
        }
    }
    return nullptr;
}

const Resource* ErfArchive::find_resource_by_name(const std::string& resource_name) const {
    const std::string target = normalized_filename_resource_key(resource_name, disk_format_);
    for (const auto& res : resources_) {
        if (normalized_filename_resource_key(resource_filename_for_archive(res, resource_type_profile_), disk_format_) == target) {
            return &res;
        }
    }
    return nullptr;
}

Resource* ErfArchive::find_resource_by_name(const std::string& resource_name) {
    const std::string target = normalized_filename_resource_key(resource_name, disk_format_);
    for (auto& res : resources_) {
        if (normalized_filename_resource_key(resource_filename_for_archive(res, resource_type_profile_), disk_format_) == target) {
            return &res;
        }
    }
    return nullptr;
}


void ErfArchive::ensure_loaded_for_operation(const char* message) const {
    if (!loaded_) {
        throw ErfError(message);
    }
}

void ErfArchive::ensure_archive_stream() {
    if (!reader_backed_ && loaded_file_identity_.valid &&
        !same_regular_file_revision(filename_, loaded_file_identity_)) {
        throw ErfError("Archive changed after it was loaded; reopen the archive before extracting resources.");
    }
    if (archive_stream_.is_open()) return;
    if (reader_backed_) {
        throw ErfError("Unable to get resource from the range-backed archive, its reader is unavailable!");
    }
    if (filename_.empty() || !loaded_file_identity_.valid) {
        // Resource lookup checks only whether the existing file stream field
        // is nil. Preserve that fail-closed behavior when no stable native
        // archive identity is available for a safe reopen.
        throw ErfError("Unable to get resource from file, the file could not be read!");
    }
    if (!same_regular_file_revision(filename_, loaded_file_identity_)) {
        throw ErfError("Archive changed after it was loaded; reopen the archive before extracting resources.");
    }
#if defined(_WIN32)
    archive_lock_handle_ = open_loaded_archive_lock(filename_);
    if (archive_lock_handle_ == nullptr) {
        throw ErfError("Unable to reopen archive file \"" + path_to_string(filename_) + "\".");
    }
#endif
    open_binary_input_stream(archive_stream_, filename_);
    if (!archive_stream_.is_open()) {
#if defined(_WIN32)
        close_loaded_archive_lock(archive_lock_handle_);
#endif
        const std::string detail = archive_stream_.last_error();
        throw ErfError("Unable to reopen archive file \"" + path_to_string(filename_) +
                       "\"" + (detail.empty() ? std::string(".") : std::string(": ") + detail));
    }
    if (!same_regular_file_revision(filename_, loaded_file_identity_)) {
        archive_stream_.close();
#if defined(_WIN32)
        close_loaded_archive_lock(archive_lock_handle_);
#endif
        throw ErfError("Archive changed while it was being reopened; reopen the archive before extracting resources.");
    }
}

std::array<char, 4> archive_type_to_header(ArchiveType type) {
    switch (type) {
        case ArchiveType::MOD: return make_header("MOD ");
        case ArchiveType::HAK: return make_header("HAK ");
        case ArchiveType::ERF: return make_header("ERF ");
        // K1/K2 CExoEncapsulatedFile::LoadHeader routes .sav and .nwm through
        // the MOD-family branch: resource type 0x0809/0x080E, file header "MOD ".
        case ArchiveType::SAV: return make_header("MOD ");
        case ArchiveType::NWM: return make_header("MOD ");
        case ArchiveType::RIM: return make_header("RIM ");
        case ArchiveType::ERF_V2: return make_header("ERF ");
        case ArchiveType::ERF_V2_2: return make_header("ERF ");
        case ArchiveType::ERF_V2_2_UNCOMPRESSED: return make_header("ERF ");
        case ArchiveType::ERF_V3: return make_header("ERF ");
    }
    return make_header("ERF ");
}

ArchiveType archive_type_from_extension(const std::filesystem::path& filename) {
    const std::string ext = ascii_upper(extension_string(filename));
    if (ext == ".MOD") return ArchiveType::MOD;
    if (ext == ".HAK") return ArchiveType::HAK;
    if (ext == ".SAV") return ArchiveType::SAV;
    if (ext == ".NWM") return ArchiveType::NWM;
    if (ext == ".RIM") return ArchiveType::RIM;
    if (ext == ".RIMP") return ArchiveType::ERF_V3;
    if (ext == ".CRF") return ArchiveType::ERF_V3;
    return ArchiveType::ERF;
}

std::string archive_type_to_string(ArchiveType type) {
    switch (type) {
        case ArchiveType::MOD: return "MOD";
        case ArchiveType::HAK: return "HAK";
        case ArchiveType::ERF: return "ERF";
        case ArchiveType::SAV: return "SAV";
        case ArchiveType::NWM: return "NWM";
        case ArchiveType::RIM: return "RIM";
        case ArchiveType::ERF_V2: return "ERF_V2";
        case ArchiveType::ERF_V2_2: return "ERF_V2_2";
        case ArchiveType::ERF_V2_2_UNCOMPRESSED: return "ERF_V2_2_UNCOMPRESSED";
        case ArchiveType::ERF_V3: return "ERF_V3";
    }
    return "ERF";
}

} // namespace neoshared::erf
