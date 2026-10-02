#include <neoshared/model/Lighting.hpp>
#include <neoshared/model/Emitter.hpp>
#include "neoshared/model/Model.hpp"

#include "neoshared/PathUtf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace neoshared::model {
namespace {

constexpr std::uint32_t kK1ModelToken0 = 4273776u;
constexpr std::uint32_t kK2ModelToken0 = 4285200u;
constexpr std::uint32_t kK1ModelToken1 = 4216096u;
constexpr std::uint32_t kK2ModelToken1 = 4216320u;
constexpr std::size_t kWrapperSize = 12u;
constexpr std::size_t kModelHeaderSize = 196u;
constexpr std::size_t kAnimationHeaderSize = 136u;
constexpr std::size_t kAnimationEventSize = 36u;
constexpr std::size_t kNodeHeaderSize = 80u;
constexpr std::size_t kControllerSize = 16u;
constexpr std::size_t kK1MeshHeaderSize = 332u;
constexpr std::size_t kK2MeshHeaderSize = 340u;
constexpr std::size_t kFaceSize = 32u;
constexpr std::size_t kAabbNodeSize = 40u;
constexpr std::size_t kK2DanglyHeaderSize = 24u;
constexpr std::size_t kReferenceHeaderSize = 36u;
constexpr std::size_t kLightHeaderSize = 92u;
constexpr std::size_t kEmitterHeaderSize = 224u;

// K2 MdlNodeSkin32 fields are relative to the beginning of the triangle-mesh
// extension (the base-node header precedes it). These offsets were confirmed
// against the 64-bit conversion constructor and ExecuteSkinTransforms.
constexpr std::size_t kK2SkinWeightMdxOffset = 352u;
constexpr std::size_t kK2SkinBoneMdxOffset = 356u;
constexpr std::size_t kK2SkinInverseQuaternionArray = 368u;
constexpr std::size_t kK2SkinInverseTranslationArray = 380u;
constexpr std::size_t kK2SkinBoneConstantsArray = 392u;
constexpr std::size_t kK2SkinBoneMapOffset = 404u;
constexpr std::size_t kK2SkinBoneMapCount = 16u;
constexpr std::size_t kArrayDefinitionSize = 12u;

constexpr std::uint32_t kMdxVertex = 0x00000001u;
constexpr std::uint32_t kMdxTex0 = 0x00000002u;
constexpr std::uint32_t kMdxTex1 = 0x00000004u;
constexpr std::uint32_t kMdxNormal = 0x00000020u;

bool isModelToken(std::uint32_t token) noexcept {
    return token == kK1ModelToken0 || token == kK2ModelToken0;
}

std::string sourcePrefix(const std::string& label) {
    return label.empty() ? std::string{} : label + ": ";
}

class ByteView final {
public:
    ByteView(const std::uint8_t* data, std::size_t size, std::string label)
        : data_(data), size_(size), label_(std::move(label)) {}

    std::size_t size() const noexcept { return size_; }
    bool contains(std::size_t offset, std::size_t length) const noexcept {
        return offset <= size_ && length <= size_ - offset;
    }

    void require(std::size_t offset, std::size_t length, const char* what) const {
        if (!contains(offset, length)) {
            std::ostringstream text;
            text << sourcePrefix(label_) << what << " is outside the bounded input (offset "
                 << offset << ", size " << length << ", input " << size_ << ")";
            throw ModelError(text.str());
        }
    }

    std::uint8_t u8(std::size_t offset, const char* what = "byte") const {
        require(offset, 1u, what);
        return data_[offset];
    }

    std::uint16_t u16(std::size_t offset, const char* what = "uint16") const {
        require(offset, 2u, what);
        return static_cast<std::uint16_t>(data_[offset]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset + 1u]) << 8u);
    }

    std::int16_t i16(std::size_t offset, const char* what = "int16") const {
        return static_cast<std::int16_t>(u16(offset, what));
    }

    std::uint32_t u32(std::size_t offset, const char* what = "uint32") const {
        require(offset, 4u, what);
        return static_cast<std::uint32_t>(data_[offset]) |
               (static_cast<std::uint32_t>(data_[offset + 1u]) << 8u) |
               (static_cast<std::uint32_t>(data_[offset + 2u]) << 16u) |
               (static_cast<std::uint32_t>(data_[offset + 3u]) << 24u);
    }

    std::int32_t i32(std::size_t offset, const char* what = "int32") const {
        return static_cast<std::int32_t>(u32(offset, what));
    }

    float f32(std::size_t offset, const char* what = "float") const {
        const auto bits = u32(offset, what);
        float result{};
        static_assert(sizeof(result) == sizeof(bits), "32-bit float required");
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    std::uint32_t f32Bits(std::size_t offset, const char* what = "float word") const {
        return u32(offset, what);
    }

    Vec2 vec2(std::size_t offset, const char* what) const {
        require(offset, 8u, what);
        return {f32(offset, what), f32(offset + 4u, what)};
    }

    Vec3 vec3(std::size_t offset, const char* what) const {
        require(offset, 12u, what);
        return {f32(offset, what), f32(offset + 4u, what), f32(offset + 8u, what)};
    }

    std::string fixedString(std::size_t offset, std::size_t length, const char* what) const {
        require(offset, length, what);
        std::size_t count = 0u;
        while (count < length && data_[offset + count] != 0u) ++count;
        std::string result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = data_[offset + i];
            result.push_back(value >= 32u && value < 127u ? static_cast<char>(value) : '?');
        }
        return result;
    }

    std::string cString(std::size_t offset, std::size_t maximum, const char* what) const {
        require(offset, 1u, what);
        const auto available = std::min(maximum, size_ - offset);
        std::size_t count = 0u;
        while (count < available && data_[offset + count] != 0u) ++count;
        if (count == available) {
            throw ModelError(sourcePrefix(label_) + std::string(what) + " is not null-terminated within its limit");
        }
        std::string result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = data_[offset + i];
            result.push_back(value >= 32u && value < 127u ? static_cast<char>(value) : '?');
        }
        return result;
    }

private:
    const std::uint8_t* data_{};
    std::size_t size_{};
    std::string label_;
};

bool finite(float value) noexcept { return std::isfinite(value); }

float wordAsFloat(std::uint32_t word) noexcept {
    float result{};
    static_assert(sizeof(result) == sizeof(word), "32-bit float required");
    std::memcpy(&result, &word, sizeof(result));
    return result;
}

Vec3 safeVec3(Vec3 value, std::vector<std::string>& warnings, const std::string& description) {
    if (finite(value.x) && finite(value.y) && finite(value.z)) return value;
    warnings.push_back(description + " contains a non-finite value; zero was substituted.");
    if (!finite(value.x)) value.x = 0.0f;
    if (!finite(value.y)) value.y = 0.0f;
    if (!finite(value.z)) value.z = 0.0f;
    return value;
}

Quaternion safeQuaternion(Quaternion value, std::vector<std::string>& warnings,
                          const std::string& description) {
    if (!finite(value.x) || !finite(value.y) || !finite(value.z) || !finite(value.w)) {
        warnings.push_back(description + " contains a non-finite quaternion; identity was substituted.");
        return {};
    }
    const double lengthSquared = static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z +
        static_cast<double>(value.w) * value.w;
    if (lengthSquared < 1.0e-16) {
        warnings.push_back(description + " contains a zero-length quaternion; identity was substituted.");
        return {};
    }
    const auto reciprocal = static_cast<float>(1.0 / std::sqrt(lengthSquared));
    value.x *= reciprocal;
    value.y *= reciprocal;
    value.z *= reciprocal;
    value.w *= reciprocal;
    return value;
}

Vec3 subtract(const Vec3& left, const Vec3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 add(const Vec3& left, const Vec3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 cross(const Vec3& left, const Vec3& right) noexcept {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

Vec3 normalize(const Vec3& value) noexcept {
    const double lengthSquared = static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z;
    if (lengthSquared < 1.0e-24 || !std::isfinite(lengthSquared)) return {0.0f, 0.0f, 1.0f};
    const auto reciprocal = static_cast<float>(1.0 / std::sqrt(lengthSquared));
    return {value.x * reciprocal, value.y * reciprocal, value.z * reciprocal};
}

std::string normalizedResourceName(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    std::size_t first = 0u;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
    if (first != 0u) value.erase(0u, first);
    std::string folded = value;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    if (folded == "null" || folded == "none") return {};
    return value;
}

std::vector<std::uint8_t> readBoundedFile(const std::filesystem::path& path, std::size_t maximum) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw ModelError("Unable to inspect " + neoshared::pathToUtf8(path) + ": " + error.message());
    if (size > maximum) {
        throw ModelError("Refusing to read a model file larger than the configured limit: " +
                         neoshared::pathToUtf8(path));
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw ModelError("Model file is too large for this process: " + neoshared::pathToUtf8(path));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw ModelError("Unable to open model file: " + neoshared::pathToUtf8(path));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
            throw ModelError("Model file changed or could not be read completely: " + neoshared::pathToUtf8(path));
        }
    }
    return bytes;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    return value;
}

std::optional<std::filesystem::path> caseInsensitiveSibling(
    const std::filesystem::path& source, const std::string& extension) {
    auto exact = source;
    exact.replace_extension(extension);
    std::error_code error;
    if (std::filesystem::is_regular_file(exact, error) && !error) return exact;
    error.clear();
    const auto directory = source.parent_path().empty() ? std::filesystem::path{"."} : source.parent_path();
    const auto desired = lowercase(neoshared::pathToUtf8(source.stem()) + extension);
    std::optional<std::filesystem::path> match;
    for (std::filesystem::directory_iterator iterator(directory, error), end; !error && iterator != end;
         iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        if (lowercase(neoshared::pathToUtf8(iterator->path().filename())) != desired) continue;
        if (match) return std::nullopt; // Ambiguous on a case-sensitive host.
        match = iterator->path();
    }
    return match;
}

class Parser final {
public:
    Parser(const std::vector<std::uint8_t>& mdlBytes,
           const std::vector<std::uint8_t>& mdxBytes,
           std::string sourceLabel,
           const ParseLimits& limits)
        : mdlBytes_(mdlBytes), mdxBytes_(mdxBytes), label_(std::move(sourceLabel)), limits_(limits),
          whole_(mdlBytes_.data(), mdlBytes_.size(), label_) {}

    Model parse() {
        if (mdlBytes_.size() > limits_.maximumMdlBytes) {
            throw ModelError(sourcePrefix(label_) + "MDL exceeds the configured input limit");
        }
        if (mdxBytes_.size() > limits_.maximumMdxBytes) {
            throw ModelError(sourcePrefix(label_) + "MDX exceeds the configured input limit");
        }
        if (mdlBytes_.size() < 4u) throw ModelError(sourcePrefix(label_) + "MDL is truncated");

        std::size_t payloadOffset = 0u;
        if (mdlBytes_.size() >= kWrapperSize + 4u && isModelToken(whole_.u32(kWrapperSize))) {
            payloadOffset = kWrapperSize;
            const auto declaredMdl = whole_.u32(4u, "declared MDL size");
            const auto declaredMdx = whole_.u32(8u, "declared MDX size");
            if (declaredMdl != 0u && declaredMdl > mdlBytes_.size() - kWrapperSize) {
                throw ModelError(sourcePrefix(label_) + "MDL wrapper declares more payload bytes than are present");
            }
            if (declaredMdl != 0u && declaredMdl != mdlBytes_.size() - kWrapperSize) {
                model_.warnings.push_back("The MDL wrapper size differs from the supplied payload length.");
            }
            if (declaredMdx != 0u && mdxBytes_.empty()) {
                model_.warnings.push_back("The MDL declares a companion MDX payload, but no MDX resource was supplied.");
            } else if (declaredMdx != 0u && declaredMdx != mdxBytes_.size()) {
                model_.warnings.push_back("The MDL wrapper's MDX size differs from the supplied companion length.");
            }
        } else if (!isModelToken(whole_.u32(0u))) {
            throw ModelError(sourcePrefix(label_) + "Unrecognized binary MDL layout token or ASCII MDL input");
        }

        payload_ = std::make_unique<ByteView>(mdlBytes_.data() + payloadOffset,
                                              mdlBytes_.size() - payloadOffset, label_);
        parseHeader();
        loadNames();
        if (rootNodeOffset_ == 0u || rootNodeOffset_ == 0xFFFFFFFFu) {
            throw ModelError(sourcePrefix(label_) + "MDL has no valid root node offset");
        }
        parseNode(rootNodeOffset_, std::nullopt, 0u);
        if (declaredNodeCount_ != 0u && model_.nodes.size() != declaredNodeCount_) {
            model_.warnings.push_back("The reachable node count (" + std::to_string(model_.nodes.size()) +
                                      ") differs from the model header (" +
                                      std::to_string(declaredNodeCount_) + ").");
        }
        parseAnimations();
        calculateWorldTransformsAndBounds();
        return std::move(model_);
    }

private:
    void parseHeader() {
        payload_->require(0u, kModelHeaderSize, "model header");
        const auto token0 = payload_->u32(0u, "model layout token 0");
        const auto token1 = payload_->u32(4u, "model layout token 1");
        if (token0 == kK1ModelToken0 && token1 == kK1ModelToken1) {
            model_.game = GameVersion::Kotor1;
        } else if (token0 == kK2ModelToken0 && token1 == kK2ModelToken1) {
            model_.game = GameVersion::Kotor2;
        } else {
            std::ostringstream message;
            message << sourcePrefix(label_) << "Unsupported model layout tokens 0x" << std::hex
                    << token0 << "/0x" << token1;
            throw ModelError(message.str());
        }
        model_.name = payload_->fixedString(8u, 32u, "model name");
        rootNodeOffset_ = payload_->u32(40u, "root node offset");
        declaredNodeCount_ = payload_->u32(44u, "declared node count");
        model_.classification = payload_->u16(80u, "model classification");
        model_.subclassification = payload_->u8(82u, "model subclassification");
        model_.fog = payload_->u8(83u, "model fog flag") != 0u;
        animationsOffset_ = payload_->u32(88u, "animation-offset array");
        model_.declaredAnimationCount = payload_->u32(92u, "animation count");
        model_.animationScale = payload_->f32(132u, "animation scale");
        if (!finite(model_.animationScale)) {
            model_.warnings.push_back("The animation scale is non-finite; 1 was substituted.");
            model_.animationScale = 1.0f;
        }
        model_.supermodel = normalizedResourceName(payload_->fixedString(136u, 32u, "supermodel name"));
        namesOffset_ = payload_->u32(184u, "name-offset table");
        namesCount_ = payload_->u32(188u, "name count");
        if (declaredNodeCount_ > limits_.maximumNodes || namesCount_ > limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) + "MDL declares more nodes/names than the configured limit");
        }
        if (model_.declaredAnimationCount > limits_.maximumAnimations) {
            throw ModelError(sourcePrefix(label_) + "MDL declares more animations than the configured limit");
        }
    }

    void loadNames() {
        names_.clear();
        names_.reserve(namesCount_);
        if (namesCount_ == 0u) return;
        if (namesOffset_ == 0u || namesOffset_ == 0xFFFFFFFFu ||
            namesCount_ > (payload_->size() - std::min<std::size_t>(namesOffset_, payload_->size())) / 4u) {
            model_.warnings.push_back("The node-name offset table is unavailable; node IDs will be displayed instead.");
            return;
        }
        payload_->require(namesOffset_, static_cast<std::size_t>(namesCount_) * 4u, "name-offset table");
        for (std::uint32_t index = 0u; index < namesCount_; ++index) {
            const auto offset = payload_->u32(namesOffset_ + static_cast<std::size_t>(index) * 4u,
                                              "node-name offset");
            if (offset == 0u || offset == 0xFFFFFFFFu || offset >= payload_->size()) {
                names_.emplace_back();
                continue;
            }
            try {
                names_.push_back(payload_->cString(offset, limits_.maximumNameLength, "node name"));
            } catch (const ModelError&) {
                names_.emplace_back();
                model_.warnings.push_back("Node-name entry " + std::to_string(index) + " is malformed.");
            }
        }
    }

    static ControllerValueKind controllerValueKind(std::uint8_t rawFlags) noexcept {
        switch (rawFlags & 0x0fu) {
        case 1u: return ControllerValueKind::Float;
        case 2u: return ControllerValueKind::PackedQuaternion;
        case 3u: return ControllerValueKind::Vector;
        case 4u: return ControllerValueKind::Quaternion;
        default: return ControllerValueKind::Unknown;
        }
    }

    static ControllerInterpolation controllerInterpolation(std::uint8_t rawFlags) noexcept {
        switch (rawFlags & 0xf0u) {
        case 0x00u: return ControllerInterpolation::Linear;
        case 0x10u: return ControllerInterpolation::Bezier;
        default: return ControllerInterpolation::Unknown;
        }
    }

    static std::size_t controllerWordsPerKey(const Controller& controller) noexcept {
        const bool bezier = controller.interpolation == ControllerInterpolation::Bezier;
        switch (controller.valueKind) {
        case ControllerValueKind::Float: return bezier ? 3u : 1u;
        case ControllerValueKind::PackedQuaternion: return 1u;
        case ControllerValueKind::Vector: return bezier ? 9u : 3u;
        case ControllerValueKind::Quaternion: return 4u;
        case ControllerValueKind::Unknown: return 0u;
        }
        return 0u;
    }

    void parseControllers(std::size_t nodeOffset, Node& node, const std::string& description) {
        const auto controllerOffset = payload_->u32(nodeOffset + 56u, "controller array offset");
        const auto controllerCount = payload_->u32(nodeOffset + 60u, "controller count");
        const auto dataOffset = payload_->u32(nodeOffset + 68u, "controller-data offset");
        const auto dataCount = payload_->u32(nodeOffset + 72u, "controller-data count");

        if (controllerCount > limits_.maximumControllers ||
            totalControllers_ > limits_.maximumControllers - controllerCount) {
            throw ModelError(sourcePrefix(label_) + "Controller count exceeds the configured limit");
        }
        if (dataCount > limits_.maximumControllerValues ||
            totalControllerValues_ > limits_.maximumControllerValues - dataCount) {
            throw ModelError(sourcePrefix(label_) + "Controller-data count exceeds the configured limit");
        }
        totalControllers_ += controllerCount;
        totalControllerValues_ += dataCount;

        if (dataCount != 0u) {
            if (dataOffset == 0u || dataOffset == 0xffffffffu ||
                !payload_->contains(dataOffset, static_cast<std::size_t>(dataCount) * 4u)) {
                model_.warnings.push_back(description +
                    " declares controller data outside the MDL payload; its controllers were ignored.");
                return;
            }
            node.controllerDataWords.reserve(dataCount);
            for (std::uint32_t index = 0u; index < dataCount; ++index) {
                node.controllerDataWords.push_back(payload_->f32Bits(
                    static_cast<std::size_t>(dataOffset) + static_cast<std::size_t>(index) * 4u,
                    "controller-data word"));
            }
        }

        if (controllerCount == 0u) return;
        if (controllerOffset == 0u || controllerOffset == 0xffffffffu ||
            !payload_->contains(controllerOffset,
                                static_cast<std::size_t>(controllerCount) * kControllerSize)) {
            model_.warnings.push_back(description +
                " declares a controller array outside the MDL payload; its controllers were ignored.");
            return;
        }

        node.controllers.reserve(controllerCount);
        for (std::uint32_t index = 0u; index < controllerCount; ++index) {
            const auto offset = static_cast<std::size_t>(controllerOffset) +
                                static_cast<std::size_t>(index) * kControllerSize;
            Controller controller;
            controller.type = payload_->u32(offset, "controller type");
            controller.sourceOffset = payload_->i16(offset + 4u, "controller source offset");
            controller.rowCount = payload_->u16(offset + 6u, "controller row count");
            controller.timeIndex = payload_->u16(offset + 8u, "controller time index");
            controller.dataIndex = payload_->u16(offset + 10u, "controller value index");
            controller.rawFlags = payload_->u8(offset + 12u, "controller encoding");
            controller.valueKind = controllerValueKind(controller.rawFlags);
            controller.interpolation = controllerInterpolation(controller.rawFlags);

            bool valid = true;
            const auto rows = static_cast<std::size_t>(controller.rowCount);
            const auto timeIndex = static_cast<std::size_t>(controller.timeIndex);
            if (rows != 0u &&
                (timeIndex > node.controllerDataWords.size() ||
                 rows > node.controllerDataWords.size() - timeIndex)) {
                valid = false;
            }
            const auto wordsPerKey = controllerWordsPerKey(controller);
            if (rows != 0u && wordsPerKey != 0u) {
                const auto dataIndex = static_cast<std::size_t>(controller.dataIndex);
                if (rows > std::numeric_limits<std::size_t>::max() / wordsPerKey ||
                    dataIndex > node.controllerDataWords.size() ||
                    rows * wordsPerKey > node.controllerDataWords.size() - dataIndex) {
                    valid = false;
                }
            }
            if (!valid) {
                model_.warnings.push_back(description + " controller " + std::to_string(index) +
                    " references values outside its bounded controller-data array and was skipped.");
                continue;
            }

            if (rows > 1u) {
                float previous = wordAsFloat(node.controllerDataWords[timeIndex]);
                bool ordered = finite(previous);
                for (std::size_t key = 1u; key < rows && ordered; ++key) {
                    const auto current = wordAsFloat(node.controllerDataWords[timeIndex + key]);
                    ordered = finite(current) && current >= previous;
                    previous = current;
                }
                if (!ordered) {
                    model_.warnings.push_back(description + " controller " + std::to_string(index) +
                        " has non-finite or unsorted key times; evaluation will clamp safely.");
                }
            }
            node.controllers.push_back(controller);
        }
    }

    std::optional<std::pair<std::size_t, std::size_t>> readArrayDefinition(
        std::size_t definition, std::size_t stride, std::size_t maximumCount,
        const std::string& description) {
        if (!payload_->contains(definition, kArrayDefinitionSize)) {
            model_.warnings.push_back(description + " has no complete array definition.");
            return std::nullopt;
        }
        const auto dataOffset = payload_->u32(definition, "array data offset");
        const auto count = payload_->u32(definition + 4u, "array count");
        const auto capacity = payload_->u32(definition + 8u, "array capacity");
        if (count > maximumCount) {
            throw ModelError(sourcePrefix(label_) + description +
                             " count exceeds the configured limit");
        }
        if (capacity != 0u && count > capacity) {
            model_.warnings.push_back(description +
                " has a count larger than its capacity; the array was ignored.");
            return std::nullopt;
        }
        if (count == 0u) return std::pair<std::size_t, std::size_t>{0u, 0u};
        if (dataOffset == 0u || dataOffset == 0xffffffffu ||
            stride > std::numeric_limits<std::size_t>::max() / count ||
            !payload_->contains(dataOffset, static_cast<std::size_t>(count) * stride)) {
            model_.warnings.push_back(description +
                " points outside the bounded MDL payload; the array was ignored.");
            return std::nullopt;
        }
        return std::pair<std::size_t, std::size_t>{
            static_cast<std::size_t>(dataOffset), static_cast<std::size_t>(count)};
    }

    void parseReferenceNode(std::size_t nodeOffset, std::size_t nodeIndex) {
        ReferenceNodeData data;
        const auto extension = nodeOffset + kNodeHeaderSize;
        if (!payload_->contains(extension, kReferenceHeaderSize)) {
            model_.warnings.push_back("Reference node " + model_.nodes[nodeIndex].name +
                                      " has a truncated 36-byte extension.");
            model_.nodes[nodeIndex].reference = std::move(data);
            return;
        }
        data.model = normalizedResourceName(
            payload_->fixedString(extension, 32u, "reference model ResRef"));
        data.reattachable = payload_->u32(extension + 32u, "reference reattachable flag") != 0u;
        model_.nodes[nodeIndex].reference = std::move(data);
    }

    void parseLightNode(std::size_t nodeOffset, std::size_t nodeIndex) {
        LightNodeData data;
        if (model_.game != GameVersion::Kotor2) {
            model_.warnings.push_back("Light node " + model_.nodes[nodeIndex].name +
                " uses a K1 extension that has not been verified against the supplied binaries.");
            model_.nodes[nodeIndex].light = std::move(data);
            return;
        }
        // Defaults from the supplied K2 MdlNodeLight constructor. Keep these
        // local to the K2 parser so unverified K1 light nodes retain their
        // pre-existing neutral fallback state.
        data.dynamicType = 1;
        data.dynamic = true;
        data.affectDynamic = true;
        data.castsShadow = true;
        data.priority = 5;
        data.fading = true;
        const auto extension = nodeOffset + kNodeHeaderSize;
        if (!payload_->contains(extension, kLightHeaderSize)) {
            model_.warnings.push_back("Light node " + model_.nodes[nodeIndex].name +
                                      " has a truncated K2 light extension.");
            model_.nodes[nodeIndex].light = std::move(data);
            return;
        }
        data.flareRadius = payload_->f32(extension, "light flare radius");
        if (!finite(data.flareRadius)) data.flareRadius = 0.0f;

        // The first array definition at +4 is a serialized engine-side
        // texture-object handle list. Authored texture names are the fifth
        // array at +52, as confirmed by the K2 conversion constructor and
        // texturenames ASCII parse destination.
        const auto textures = readArrayDefinition(
            extension + 52u, 32u, limits_.maximumVertices,
            "Light node " + model_.nodes[nodeIndex].name + " texture-name array");
        if (textures) {
            data.textureNames.reserve(textures->second);
            for (std::size_t index = 0u; index < textures->second; ++index) {
                data.textureNames.push_back(normalizedResourceName(payload_->fixedString(
                    textures->first + index * 32u, 32u, "light flare texture name")));
            }
        }
        const auto sizes = readArrayDefinition(
            extension + 16u, 4u, limits_.maximumVertices,
            "Light node " + model_.nodes[nodeIndex].name + " flare-size array");
        if (sizes) {
            data.flareSizes.reserve(sizes->second);
            for (std::size_t index = 0u; index < sizes->second; ++index) {
                auto value = payload_->f32(sizes->first + index * 4u, "light flare size");
                data.flareSizes.push_back(finite(value) ? value : 0.0f);
            }
        }
        const auto positions = readArrayDefinition(
            extension + 28u, 4u, limits_.maximumVertices,
            "Light node " + model_.nodes[nodeIndex].name + " flare-position array");
        if (positions) {
            data.flarePositions.reserve(positions->second);
            for (std::size_t index = 0u; index < positions->second; ++index) {
                auto value = payload_->f32(positions->first + index * 4u,
                                           "light flare position");
                data.flarePositions.push_back(finite(value) ? value : 0.0f);
            }
        }
        const auto shifts = readArrayDefinition(
            extension + 40u, 12u, limits_.maximumVertices,
            "Light node " + model_.nodes[nodeIndex].name + " flare-color array");
        if (shifts) {
            data.flareColorShifts.reserve(shifts->second);
            for (std::size_t index = 0u; index < shifts->second; ++index) {
                data.flareColorShifts.push_back(safeVec3(
                    payload_->vec3(shifts->first + index * 12u, "light flare color shift"),
                    model_.warnings, "Light node " + model_.nodes[nodeIndex].name +
                        " flare color shift"));
            }
        }

        // These source offsets follow the complete MdlNodeLight32 ->
        // MdlNodeLight conversion constructor and matching ASCII fields.
        data.priority = payload_->i32(extension + 64u, "light priority");
        data.ambientOnly = payload_->u32(extension + 68u, "ambient-only flag") != 0u;
        data.dynamicType = payload_->i32(extension + 72u, "dynamic-light type");
        data.dynamic = data.dynamicType != 0;
        data.affectDynamic = payload_->u32(extension + 76u, "affect-dynamic flag") != 0u;
        data.castsShadow = payload_->u32(extension + 80u, "light shadow flag") != 0u;
        data.generateFlare = payload_->u32(extension + 84u, "generate-flare flag") != 0u;
        data.fading = payload_->u32(extension + 88u, "fading-light flag") != 0u;
        model_.nodes[nodeIndex].light = std::move(data);
    }

    void parseEmitterNode(std::size_t nodeOffset, std::size_t nodeIndex) {
        EmitterNodeData data;
        const auto extension = nodeOffset + kNodeHeaderSize;
        if (!payload_->contains(extension, kEmitterHeaderSize)) {
            model_.warnings.push_back("Emitter node " + model_.nodes[nodeIndex].name +
                (model_.game == GameVersion::Kotor2 ? " has a truncated 224-byte K2 extension." :
                                                     " has a truncated 224-byte K1 extension."));
            model_.nodes[nodeIndex].emitter = std::move(data);
            return;
        }
        for (std::size_t index = 0u; index < data.rawWords.size(); ++index) {
            data.rawWords[index] = payload_->u32(extension + index * 4u,
                                                 "emitter extension word");
        }
        const auto safeEmitterFloat = [&](std::size_t relative, const char* what) {
            const auto value = payload_->f32(extension + relative, what);
            return finite(value) ? value : 0.0f;
        };
        data.deadSpace = safeEmitterFloat(0u, "emitter dead space");
        data.blastRadius = safeEmitterFloat(4u, "emitter blast radius");
        data.blastLength = safeEmitterFloat(8u, "emitter blast length");
        data.branchCount = payload_->u32(extension + 12u, "lightning branch count");
        data.controlPointSmoothing = payload_->u32(extension + 16u, "lightning control smoothing") != 0;
        data.xGrid = payload_->u32(extension + 20u, "emitter X grid");
        data.yGrid = payload_->u32(extension + 24u, "emitter Y grid");
        data.spawnType = payload_->u32(extension + 28u, "emitter spawn type");
        data.updateMode = normalizedResourceName(
            payload_->fixedString(extension + 32u, 32u, "emitter update mode"));
        data.renderMode = normalizedResourceName(
            payload_->fixedString(extension + 64u, 32u, "emitter render mode"));
        data.blendMode = normalizedResourceName(
            payload_->fixedString(extension + 96u, 32u, "emitter blend mode"));
        data.texture = normalizedResourceName(
            payload_->fixedString(extension + 128u, 32u, "emitter texture"));
        data.chunkName = normalizedResourceName(
            payload_->fixedString(extension + 160u, 16u, "emitter chunk name"));
        data.twoSidedTexture = payload_->u32(extension + 176u,
                                             "emitter two-sided texture flag") != 0u;
        data.loop = payload_->u32(extension + 180u, "emitter loop flag") != 0u;
        data.renderOrder = payload_->u16(extension + 184u, "emitter render order");
        data.frameBlending = payload_->u8(extension + 186u,
                                          "emitter frame-blending flag") != 0u;
        data.depthTexture = normalizedResourceName(
            payload_->fixedString(extension + 187u, 32u, "emitter depth texture"));
        if (model_.game == GameVersion::Kotor1) {
            model_.warnings.push_back("Emitter node " + model_.nodes[nodeIndex].name +
                " uses the binary-verified 224-byte K1 extension; mode-specific runtime parity is reported by the renderer.");
        }
        model_.nodes[nodeIndex].emitter = std::move(data);
    }

    void parseSpecializedNode(std::size_t nodeOffset, std::size_t nodeIndex) {
        const auto flags = model_.nodes[nodeIndex].flags;
        if ((flags & kNodeLight) != 0u) parseLightNode(nodeOffset, nodeIndex);
        if ((flags & kNodeEmitter) != 0u) parseEmitterNode(nodeOffset, nodeIndex);
        if ((flags & kNodeCamera) != 0u) model_.nodes[nodeIndex].camera = CameraNodeData{};
        if ((flags & kNodeTrigger) != 0u) {
            TriggerNodeData data;
            data.binaryVerified = model_.game == GameVersion::Kotor2;
            // K2 MdlNodeTrigger::InternalParseField is a no-op and
            // InternalCreateInstance returns null. K1 is retained only as an
            // unverified marker rather than inheriting those K2 semantics.
            model_.nodes[nodeIndex].trigger = data;
        }
        if ((flags & kNodeReference) != 0u) parseReferenceNode(nodeOffset, nodeIndex);
        if ((flags & kNodeLightsaber) != 0u)
            model_.nodes[nodeIndex].lightsaber = LightsaberNodeData{};
    }

    std::size_t parseNode(std::uint32_t offset, std::optional<std::size_t> parent, std::size_t depth) {
        if (depth > limits_.maximumRecursionDepth) {
            throw ModelError(sourcePrefix(label_) + "Node hierarchy exceeds the configured depth limit");
        }
        const auto existing = nodeByOffset_.find(offset);
        if (existing != nodeByOffset_.end()) {
            // The first traversal establishes the canonical parent. Re-parenting a
            // previously parsed node can introduce self/cyclic world transforms.
            model_.warnings.push_back("The node hierarchy reuses or cycles through offset " +
                                      std::to_string(offset) + "; it was parsed once.");
            return existing->second;
        }
        if (model_.nodes.size() >= limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) + "Node count exceeds the configured limit");
        }
        payload_->require(offset, kNodeHeaderSize, "node header");

        Node node;
        node.flags = payload_->u16(offset, "node flags");
        node.id = payload_->u16(offset + 4u, "node ID");
        node.parent = parent;
        node.name = node.id < names_.size() ? names_[node.id] : std::string{};
        if (node.name.empty()) node.name = "node_" + std::to_string(node.id);
        node.position = safeVec3(payload_->vec3(offset + 16u, "node position"), model_.warnings,
                                 "Node " + node.name);
        node.orientation = safeQuaternion(
            {payload_->f32(offset + 32u, "node quaternion x"),
             payload_->f32(offset + 36u, "node quaternion y"),
             payload_->f32(offset + 40u, "node quaternion z"),
             payload_->f32(offset + 28u, "node quaternion w")},
            model_.warnings, "Node " + node.name);
        parseControllers(offset, node, "Node " + node.name);
        const auto childOffset = payload_->u32(offset + 44u, "child-offset array");
        const auto childCount = payload_->u32(offset + 48u, "child count");
        if (childCount > limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) + "Node child count exceeds the configured limit");
        }

        const auto nodeIndex = model_.nodes.size();
        model_.nodes.push_back(std::move(node));
        nodeByOffset_.emplace(offset, nodeIndex);

        parseSpecializedNode(offset, nodeIndex);
        if ((model_.nodes[nodeIndex].flags & kNodeMesh) != 0u)
            parseMesh(offset + kNodeHeaderSize, nodeIndex);

        if (childCount != 0u) {
            if (childOffset == 0u || childOffset == 0xFFFFFFFFu) {
                model_.warnings.push_back("Node " + model_.nodes[nodeIndex].name +
                                          " declares children but has no child-offset array.");
            } else {
                payload_->require(childOffset, static_cast<std::size_t>(childCount) * 4u,
                                  "child-offset array");
                for (std::uint32_t index = 0u; index < childCount; ++index) {
                    const auto child = payload_->u32(childOffset + static_cast<std::size_t>(index) * 4u,
                                                     "child node offset");
                    if (child == 0u || child == 0xFFFFFFFFu) {
                        model_.warnings.push_back("Node " + model_.nodes[nodeIndex].name +
                                                  " contains an empty child offset.");
                        continue;
                    }
                    const auto childIndex = parseNode(child, nodeIndex, depth + 1u);
                    if (model_.nodes[childIndex].parent != nodeIndex) {
                        model_.warnings.push_back("Node " + model_.nodes[nodeIndex].name +
                                                  " references a node owned by another parent; "
                                                  "the non-canonical link was ignored.");
                        continue;
                    }
                    if (std::find(model_.nodes[nodeIndex].children.begin(),
                                  model_.nodes[nodeIndex].children.end(), childIndex) ==
                        model_.nodes[nodeIndex].children.end()) {
                        model_.nodes[nodeIndex].children.push_back(childIndex);
                    }
                }
            }
        }
        return nodeIndex;
    }

    std::size_t parseAnimationNode(std::uint32_t offset, Animation& animation,
                                   std::unordered_map<std::uint32_t, std::size_t>& nodeByOffset,
                                   std::optional<std::size_t> parent, std::size_t depth) {
        if (depth > limits_.maximumRecursionDepth) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation node hierarchy exceeds the configured depth limit");
        }
        const auto existing = nodeByOffset.find(offset);
        if (existing != nodeByOffset.end()) {
            model_.warnings.push_back("Animation " + animation.name +
                " reuses or cycles through node offset " + std::to_string(offset) +
                "; it was parsed once.");
            return existing->second;
        }
        if (animation.nodes.size() >= limits_.maximumNodes ||
            totalAnimationNodes_ >= limits_.maximumAnimationNodes) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation node count exceeds the configured limit");
        }
        payload_->require(offset, kNodeHeaderSize, "animation node header");

        Node node;
        node.flags = payload_->u16(offset, "animation node flags");
        node.id = payload_->u16(offset + 4u, "animation node ID");
        node.parent = parent;
        node.name = node.id < names_.size() ? names_[node.id] : std::string{};
        if (node.name.empty()) node.name = "node_" + std::to_string(node.id);
        node.position = safeVec3(payload_->vec3(offset + 16u, "animation node position"),
                                 model_.warnings,
                                 "Animation " + animation.name + " node " + node.name);
        node.orientation = safeQuaternion(
            {payload_->f32(offset + 32u, "animation node quaternion x"),
             payload_->f32(offset + 36u, "animation node quaternion y"),
             payload_->f32(offset + 40u, "animation node quaternion z"),
             payload_->f32(offset + 28u, "animation node quaternion w")},
            model_.warnings, "Animation " + animation.name + " node " + node.name);
        parseControllers(offset, node, "Animation " + animation.name + " node " + node.name);

        const auto childOffset = payload_->u32(offset + 44u, "animation child-offset array");
        const auto childCount = payload_->u32(offset + 48u, "animation child count");
        if (childCount > limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation node child count exceeds the configured limit");
        }

        const auto nodeIndex = animation.nodes.size();
        animation.nodes.push_back(std::move(node));
        ++totalAnimationNodes_;
        nodeByOffset.emplace(offset, nodeIndex);

        if (childCount != 0u) {
            if (childOffset == 0u || childOffset == 0xffffffffu ||
                !payload_->contains(childOffset, static_cast<std::size_t>(childCount) * 4u)) {
                model_.warnings.push_back("Animation " + animation.name + " node " +
                    animation.nodes[nodeIndex].name +
                    " declares children outside the MDL payload.");
            } else {
                for (std::uint32_t index = 0u; index < childCount; ++index) {
                    const auto child = payload_->u32(
                        static_cast<std::size_t>(childOffset) + static_cast<std::size_t>(index) * 4u,
                        "animation child node offset");
                    if (child == 0u || child == 0xffffffffu) {
                        model_.warnings.push_back("Animation " + animation.name + " node " +
                            animation.nodes[nodeIndex].name + " contains an empty child offset.");
                        continue;
                    }
                    const auto childIndex = parseAnimationNode(
                        child, animation, nodeByOffset, nodeIndex, depth + 1u);
                    if (animation.nodes[childIndex].parent != nodeIndex) {
                        model_.warnings.push_back("Animation " + animation.name + " node " +
                            animation.nodes[nodeIndex].name +
                            " references a node owned by another parent; the link was ignored.");
                        continue;
                    }
                    if (std::find(animation.nodes[nodeIndex].children.begin(),
                                  animation.nodes[nodeIndex].children.end(), childIndex) ==
                        animation.nodes[nodeIndex].children.end()) {
                        animation.nodes[nodeIndex].children.push_back(childIndex);
                    }
                }
            }
        }
        return nodeIndex;
    }

    void parseAnimation(std::uint32_t offset, std::size_t index) {
        if (offset == 0u || offset == 0xffffffffu ||
            !payload_->contains(offset, kAnimationHeaderSize)) {
            model_.warnings.push_back("Animation entry " + std::to_string(index) +
                                      " has an invalid header offset and was skipped.");
            return;
        }

        Animation animation;
        animation.name = payload_->fixedString(offset + 8u, 32u, "animation name");
        if (animation.name.empty()) animation.name = "animation_" + std::to_string(index);
        const auto rootOffset = payload_->u32(offset + 40u, "animation root-node offset");
        const auto declaredNodes = payload_->u32(offset + 44u, "animation node count");
        if (declaredNodes > limits_.maximumNodes) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation declares more nodes than the configured limit");
        }
        animation.length = payload_->f32(offset + 80u, "animation length");
        animation.transitionTime = payload_->f32(offset + 84u, "animation transition time");
        animation.root = normalizedResourceName(
            payload_->fixedString(offset + 88u, 32u, "animation root name"));
        if (!finite(animation.length) || animation.length < 0.0f) {
            model_.warnings.push_back("Animation " + animation.name +
                " has an invalid length; zero was substituted.");
            animation.length = 0.0f;
        }
        if (!finite(animation.transitionTime) || animation.transitionTime < 0.0f) {
            model_.warnings.push_back("Animation " + animation.name +
                " has an invalid transition time; zero was substituted.");
            animation.transitionTime = 0.0f;
        }

        const auto eventOffset = payload_->u32(offset + 120u, "animation event array");
        const auto eventCount = payload_->u32(offset + 124u, "animation event count");
        if (eventCount > limits_.maximumAnimationEvents ||
            totalAnimationEvents_ > limits_.maximumAnimationEvents - eventCount) {
            throw ModelError(sourcePrefix(label_) +
                             "Animation event count exceeds the configured limit");
        }
        totalAnimationEvents_ += eventCount;
        if (eventCount != 0u) {
            if (eventOffset == 0u || eventOffset == 0xffffffffu ||
                !payload_->contains(eventOffset,
                                    static_cast<std::size_t>(eventCount) * kAnimationEventSize)) {
                model_.warnings.push_back("Animation " + animation.name +
                    " declares events outside the MDL payload; they were ignored.");
            } else {
                animation.events.reserve(eventCount);
                for (std::uint32_t eventIndex = 0u; eventIndex < eventCount; ++eventIndex) {
                    const auto eventRecord = static_cast<std::size_t>(eventOffset) +
                        static_cast<std::size_t>(eventIndex) * kAnimationEventSize;
                    AnimationEvent event;
                    event.time = payload_->f32(eventRecord, "animation event time");
                    event.name = payload_->fixedString(eventRecord + 4u, 32u,
                                                       "animation event name");
                    if (!finite(event.time)) {
                        model_.warnings.push_back("Animation " + animation.name + " event " +
                            std::to_string(eventIndex) +
                            " has a non-finite time and was skipped.");
                        continue;
                    }
                    animation.events.push_back(std::move(event));
                }
            }
        }

        if (rootOffset != 0u && rootOffset != 0xffffffffu) {
            std::unordered_map<std::uint32_t, std::size_t> nodeByOffset;
            parseAnimationNode(rootOffset, animation, nodeByOffset, std::nullopt, 0u);
        } else if (declaredNodes != 0u) {
            model_.warnings.push_back("Animation " + animation.name +
                                      " declares nodes but has no valid root-node offset.");
        }
        if (declaredNodes != 0u && animation.nodes.size() != declaredNodes) {
            model_.warnings.push_back("Animation " + animation.name + " has " +
                std::to_string(animation.nodes.size()) + " reachable node(s), but its header declares " +
                std::to_string(declaredNodes) + ".");
        }
        model_.animations.push_back(std::move(animation));
    }

    void parseAnimations() {
        const auto count = static_cast<std::size_t>(model_.declaredAnimationCount);
        if (count == 0u) return;
        if (animationsOffset_ == 0u || animationsOffset_ == 0xffffffffu ||
            !payload_->contains(animationsOffset_, count * 4u)) {
            model_.warnings.push_back(
                "The model declares animations but its animation-offset array is unavailable.");
            return;
        }
        model_.animations.reserve(count);
        std::unordered_set<std::uint32_t> seen;
        for (std::size_t index = 0u; index < count; ++index) {
            const auto offset = payload_->u32(static_cast<std::size_t>(animationsOffset_) + index * 4u,
                                              "animation offset");
            if (!seen.insert(offset).second) {
                model_.warnings.push_back("Animation entry " + std::to_string(index) +
                                          " repeats an earlier animation offset and was skipped.");
                continue;
            }
            parseAnimation(offset, index);
        }
        if (model_.animations.size() != count) {
            model_.warnings.push_back("Parsed " + std::to_string(model_.animations.size()) +
                " animation(s) from " + std::to_string(count) + " declared entries.");
        }
    }

    bool validMdxChannel(std::uint32_t offset, std::uint32_t rowSize, std::size_t bytes) const noexcept {
        return offset != 0xFFFFFFFFu && offset <= rowSize && bytes <= rowSize - offset;
    }

    void postProcessLightsaber(std::size_t nodeIndex, const Mesh& mesh) {
        auto& node = model_.nodes[nodeIndex];
        if (!node.lightsaber) return;
        auto& lightsaber = *node.lightsaber;
        if (model_.game != GameVersion::Kotor2) {
            model_.warnings.push_back("Lightsaber node " + node.name +
                " uses a K1 postprocess path that is not established by the supplied K2 binary; "
                "its authored source mesh was retained.");
            return;
        }
        if (!mesh.hasVertexColors || mesh.vertices.empty()) {
            model_.warnings.push_back("Lightsaber node " + node.name +
                " has no readable MDX selector colors; its authored source mesh was retained.");
            return;
        }

        std::array<std::size_t, LightsaberNodeData::pieceVertices> frontSources{};
        std::array<std::size_t, LightsaberNodeData::pieceVertices> backSources{};
        for (std::size_t code = 0u; code < LightsaberNodeData::pieceVertices; ++code) {
            const auto front = std::find_if(mesh.vertices.begin(), mesh.vertices.end(),
                [code](const Vertex& vertex) { return vertex.color[0] == code; });
            const auto back = std::find_if(mesh.vertices.begin(), mesh.vertices.end(),
                [code](const Vertex& vertex) { return vertex.color[2] == code; });
            if (front == mesh.vertices.end() || back == mesh.vertices.end()) {
                model_.warnings.push_back("Lightsaber node " + node.name +
                    " is missing red/blue selector " + std::to_string(code) +
                    " required by K2 MdlNodeLightsaber::InternalPostProcess; "
                    "its authored source mesh was retained.");
                return;
            }
            frontSources[code] = static_cast<std::size_t>(front - mesh.vertices.begin());
            backSources[code] = static_cast<std::size_t>(back - mesh.vertices.begin());
        }

        lightsaber.positions.resize(LightsaberNodeData::generatedVertexCount);
        lightsaber.originalPositions.assign(LightsaberNodeData::generatedVertexCount + 1u, {});
        lightsaber.normals.resize(LightsaberNodeData::generatedVertexCount);
        lightsaber.texcoords.resize(LightsaberNodeData::generatedVertexCount);
        const auto copySource = [&](std::size_t destination, std::size_t source) {
            const auto& vertex = mesh.vertices[source];
            lightsaber.positions[destination] = vertex.position;
            lightsaber.originalPositions[destination] = vertex.position;
            lightsaber.normals[destination] = vertex.normal;
            lightsaber.texcoords[destination] = vertex.texcoord0;
        };
        const auto copyGenerated = [&](std::size_t destination, std::size_t source) {
            lightsaber.positions[destination] = lightsaber.positions[source];
            lightsaber.originalPositions[destination] = lightsaber.positions[source];
            lightsaber.normals[destination] = lightsaber.normals[source];
            lightsaber.texcoords[destination] = lightsaber.texcoords[source];
        };

        for (std::size_t code = 0u; code < LightsaberNodeData::pieceVertices; ++code)
            copySource(LightsaberNodeData::frontPieceOffset + code, frontSources[code]);

        auto destination = LightsaberNodeData::frontSegmentOffset;
        for (std::size_t segment = 0u; segment < LightsaberNodeData::frontSegments; ++segment)
            for (std::size_t vertex = 0u; vertex < LightsaberNodeData::segmentVertices; ++vertex)
                copyGenerated(destination++, LightsaberNodeData::frontPieceOffset + vertex);

        for (std::size_t code = 0u; code < LightsaberNodeData::pieceVertices; ++code)
            copySource(LightsaberNodeData::backPieceOffset + code, backSources[code]);

        // The Android K2 routine deliberately takes the first vertex from the
        // front piece and vertices 1..3 from the back piece for every back segment.
        const std::array<std::size_t, LightsaberNodeData::segmentVertices> backTemplate{{
            LightsaberNodeData::frontPieceOffset,
            LightsaberNodeData::backPieceOffset + 1u,
            LightsaberNodeData::backPieceOffset + 2u,
            LightsaberNodeData::backPieceOffset + 3u,
        }};
        destination = LightsaberNodeData::backSegmentOffset;
        for (std::size_t segment = 0u; segment < LightsaberNodeData::backSegments; ++segment)
            for (const auto source : backTemplate) copyGenerated(destination++, source);

        lightsaber.postprocessed = destination == LightsaberNodeData::generatedVertexCount;
    }

    void parseAabbTree(std::size_t meshOffset, std::size_t nodeIndex,
                       const Mesh& mesh) {
        if ((model_.nodes[nodeIndex].flags & kNodeAabb) == 0u) return;

        AabbNodeData data;
        data.valid = true;
        const auto nodeName = model_.nodes[nodeIndex].name;
        if (model_.game != GameVersion::Kotor2) {
            data.valid = false;
            model_.warnings.push_back("AABB node " + nodeName +
                " uses a K1 tree layout that cannot be inferred from the supplied K2 binary.");
            model_.nodes[nodeIndex].aabb = std::move(data);
            return;
        }

        if (!payload_->contains(meshOffset + kK2MeshHeaderSize, sizeof(std::uint32_t))) {
            data.valid = false;
            model_.warnings.push_back("AABB node " + nodeName +
                                      " has no bounded K2 tree-root field.");
            model_.nodes[nodeIndex].aabb = std::move(data);
            return;
        }

        const auto rootOffset = payload_->u32(meshOffset + kK2MeshHeaderSize,
                                              "AABB root offset");
        if (rootOffset == 0u || rootOffset == 0xffffffffu) {
            data.valid = false;
            model_.warnings.push_back("AABB node " + nodeName +
                                      " has no runtime tree root.");
            model_.nodes[nodeIndex].aabb = std::move(data);
            return;
        }

        struct ParseIssues {
            std::size_t truncated{};
            std::size_t cycles{};
            std::size_t shared{};
            std::size_t missingChildren{};
            std::size_t invalidFaces{};
            std::size_t invalidSplits{};
            std::size_t reversedBounds{};
            std::size_t nonFiniteBounds{};
        } issues;
        std::unordered_map<std::uint32_t, std::size_t> byOffset;
        std::unordered_set<std::uint32_t> active;

        const auto validSplit = [](std::uint32_t mask) noexcept {
            return mask != 0u && (mask & ~kAabbDirectionMask) == 0u &&
                   (mask & (mask - 1u)) == 0u;
        };
        const auto childOffsetValid = [](std::uint32_t offset) noexcept {
            return offset != 0u && offset != 0xffffffffu;
        };

        std::function<std::optional<std::size_t>(std::uint32_t, std::size_t)> parseNode;
        parseNode = [&](std::uint32_t offset, std::size_t depth)
            -> std::optional<std::size_t> {
            if (depth > limits_.maximumRecursionDepth ||
                data.nodes.size() >= limits_.maximumAabbNodes) {
                throw ModelError(sourcePrefix(label_) + "AABB tree on " + nodeName +
                                 " exceeds the configured tree limit");
            }
            if (active.find(offset) != active.end()) {
                ++issues.cycles;
                data.valid = false;
                return std::nullopt;
            }
            const auto existing = byOffset.find(offset);
            if (existing != byOffset.end()) {
                ++issues.shared;
                data.valid = false;
                return existing->second;
            }
            if (!payload_->contains(offset, kAabbNodeSize)) {
                ++issues.truncated;
                data.valid = false;
                return std::nullopt;
            }

            AabbTreeNode node;
            node.sourceOffset = offset;
            node.minimum = payload_->vec3(offset, "AABB minimum");
            node.maximum = payload_->vec3(offset + 12u, "AABB maximum");
            for (auto* value : {&node.minimum.x, &node.minimum.y, &node.minimum.z,
                                &node.maximum.x, &node.maximum.y, &node.maximum.z}) {
                if (!finite(*value)) {
                    *value = 0.0f;
                    ++issues.nonFiniteBounds;
                    data.valid = false;
                }
            }
            if (node.minimum.x > node.maximum.x ||
                node.minimum.y > node.maximum.y ||
                node.minimum.z > node.maximum.z) {
                ++issues.reversedBounds;
                data.valid = false;
            }
            const auto firstOffset = payload_->u32(offset + 24u, "AABB first child");
            const auto secondOffset = payload_->u32(offset + 28u, "AABB second child");
            node.faceIndex = payload_->i32(offset + 32u, "AABB face index");
            node.splitMask = payload_->u32(offset + 36u, "AABB split mask");

            const auto index = data.nodes.size();
            data.nodes.push_back(node);
            byOffset.emplace(offset, index);
            active.emplace(offset);

            if (node.faceIndex == -1) {
                if (!validSplit(node.splitMask)) {
                    ++issues.invalidSplits;
                    data.valid = false;
                }
                if (!childOffsetValid(firstOffset) || !childOffsetValid(secondOffset)) {
                    ++issues.missingChildren;
                    data.valid = false;
                }
                if (childOffsetValid(firstOffset)) {
                    if (const auto child = parseNode(firstOffset, depth + 1u))
                        data.nodes[index].firstChild = static_cast<std::int32_t>(*child);
                }
                if (childOffsetValid(secondOffset)) {
                    if (const auto child = parseNode(secondOffset, depth + 1u))
                        data.nodes[index].secondChild = static_cast<std::int32_t>(*child);
                }
            } else if (node.faceIndex < 0 ||
                       static_cast<std::size_t>(node.faceIndex) >= mesh.faces.size()) {
                ++issues.invalidFaces;
                data.valid = false;
            }

            active.erase(offset);
            return index;
        };

        data.root = parseNode(rootOffset, 0u);
        if (!data.root) data.valid = false;
        model_.nodes[nodeIndex].aabb = std::move(data);

        const auto issueText = [&](std::size_t count, const char* description) {
            if (count == 0u) return;
            model_.warnings.push_back("AABB node " + nodeName + " contains " +
                std::to_string(count) + " " + description + ".");
        };
        issueText(issues.truncated, "tree record(s) outside the bounded MDL");
        issueText(issues.cycles, "cyclic child link(s)");
        issueText(issues.shared, "reused child link(s)");
        issueText(issues.missingChildren, "internal node(s) with missing children");
        issueText(issues.invalidFaces, "leaf node(s) with invalid face indexes");
        issueText(issues.invalidSplits, "internal node(s) with invalid split masks");
        issueText(issues.reversedBounds, "node(s) with reversed bounds");
        issueText(issues.nonFiniteBounds,
                  "non-finite bound component(s); zero was substituted");
    }

    void parseMesh(std::size_t offset, std::size_t nodeIndex) {
        if (model_.meshes.size() >= limits_.maximumMeshes) {
            throw ModelError(sourcePrefix(label_) + "Mesh count exceeds the configured limit");
        }
        const auto headerSize = model_.game == GameVersion::Kotor1 ? kK1MeshHeaderSize : kK2MeshHeaderSize;
        payload_->require(offset, headerSize, "triangle-mesh header");

        const auto facesOffset = payload_->u32(offset + 8u, "face array offset");
        auto faceCount = payload_->u32(offset + 12u, "face count");
        const auto headerVertexCount = payload_->u16(offset + 304u, "vertex count");
        if (faceCount > limits_.maximumFaces) {
            throw ModelError(sourcePrefix(label_) + "Mesh face count exceeds the configured limit");
        }
        if (faceCount != 0u) {
            payload_->require(facesOffset, static_cast<std::size_t>(faceCount) * kFaceSize, "face array");
        }

        Mesh mesh;
        mesh.nodeIndex = nodeIndex;
        mesh.rawMeshHeader.reserve(headerSize);
        for (std::size_t byte = 0u; byte < headerSize; ++byte)
            mesh.rawMeshHeader.push_back(payload_->u8(offset + byte, "mesh header byte"));
        const auto materialFloat = [&](std::size_t at, const char* name) {
            const auto value = payload_->f32(offset + at, name);
            if (finite(value)) return value;
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                " has non-finite " + name + "; zero was substituted (raw bytes retained).");
            return 0.0f;
        };
        mesh.uvAnimation.enabledRaw = payload_->u32(offset + 232u, "animate UV flag");
        mesh.uvAnimation.direction = {materialFloat(236u, "UV direction X"),
                                      materialFloat(240u, "UV direction Y")};
        mesh.uvAnimation.jitter = materialFloat(244u, "UV jitter");
        mesh.uvAnimation.jitterSpeed = materialFloat(248u, "UV jitter speed");
        mesh.textureCount = payload_->u16(offset + 306u, "texture count");
        mesh.rotateTexture = payload_->u8(offset + 309u, "rotate texture flag") != 0u;
        mesh.beaming = payload_->u8(offset + 312u, "beaming flag") != 0u;
        if (model_.game == GameVersion::Kotor2) {
            // Both 64-bit K2 conversion constructors copy these exact widths.
            // The hologram flag is a BYTE, not the surrounding padded dword.
            mesh.k2Material = K2MeshMaterialFields{
                payload_->u8(offset + 314u, "dirt enabled flag"),
                payload_->i16(offset + 316u, "dirt texture"),
                payload_->i16(offset + 318u, "dirt worldspace"),
                payload_->u8(offset + 320u, "hologram do-not-draw flag")};
        }
        mesh.totalArea = materialFloat(model_.game == GameVersion::Kotor2 ? 324u : 316u,
                                        "mesh total area");
        mesh.texture0 = normalizedResourceName(payload_->fixedString(offset + 88u, 32u, "diffuse texture"));
        mesh.texture1 = normalizedResourceName(payload_->fixedString(offset + 120u, 32u, "secondary texture"));
        mesh.diffuse = safeVec3(payload_->vec3(offset + 60u, "mesh diffuse color"), model_.warnings,
                                "Mesh on " + model_.nodes[nodeIndex].name + " diffuse color");
        mesh.ambient = safeVec3(payload_->vec3(offset + 72u, "mesh ambient color"), model_.warnings,
                                "Mesh on " + model_.nodes[nodeIndex].name + " ambient color");
        mesh.transparencyHint = payload_->u32(offset + 84u, "transparency hint");
        mesh.mdxRowSize = payload_->u32(offset + 252u, "MDX row size");
        mesh.mdxBitmap = payload_->u32(offset + 256u, "MDX channel bitmap");
        const auto mdxVertexOffset = payload_->u32(offset + 260u, "MDX vertex channel offset");
        const auto mdxNormalOffset = payload_->u32(offset + 264u, "MDX normal channel offset");
        const auto mdxTex0Offset = payload_->u32(offset + 272u, "MDX texture-0 channel offset");
        const auto mdxTex1Offset = payload_->u32(offset + 276u, "MDX texture-1 channel offset");
        mesh.mdxColorOffset = payload_->u32(offset + 268u, "MDX color channel offset");
        mesh.mdxTexcoordOffsets = {{mdxTex0Offset, mdxTex1Offset,
            payload_->u32(offset + 280u, "MDX texture-2 channel offset"),
            payload_->u32(offset + 284u, "MDX texture-3 channel offset")}};
        mesh.mdxTangentOffset = payload_->u32(offset + 288u, "MDX tangent-basis offset");
        mesh.lightmapped = payload_->u8(offset + 308u, "lightmap flag") != 0u;
        mesh.backgroundGeometry = payload_->u8(offset + 310u, "background flag") != 0u;
        mesh.castsShadow = payload_->u8(offset + 311u, "shadow flag") != 0u;
        mesh.render = payload_->u8(offset + 313u, "render flag") != 0u;
        mesh.skinned = (model_.nodes[nodeIndex].flags & kNodeSkin) != 0u;
        if ((model_.nodes[nodeIndex].flags & kNodeDangly) != 0u) {
            DanglyMeshData dangly;
            if (model_.game != GameVersion::Kotor2) {
                model_.warnings.push_back("Dangly mesh " + model_.nodes[nodeIndex].name +
                    " uses a K1 extension that has not been verified against the supplied binaries.");
            } else if (!payload_->contains(offset + kK2MeshHeaderSize, kK2DanglyHeaderSize)) {
                model_.warnings.push_back("Dangly mesh " + model_.nodes[nodeIndex].name +
                                          " has a truncated K2 extension.");
            } else {
                const auto constraints = readArrayDefinition(
                    offset + kK2MeshHeaderSize, 4u, limits_.maximumVertices,
                    "Dangly mesh " + model_.nodes[nodeIndex].name + " constraint array");
                if (constraints) {
                    dangly.constraints.reserve(constraints->second);
                    for (std::size_t index = 0u; index < constraints->second; ++index) {
                        auto value = payload_->f32(constraints->first + index * 4u,
                                                   "dangly constraint");
                        dangly.constraints.push_back(finite(value) ? value : 0.0f);
                    }
                }
                dangly.displacement = payload_->f32(
                    offset + kK2MeshHeaderSize + 12u, "dangly displacement");
                dangly.tightness = payload_->f32(
                    offset + kK2MeshHeaderSize + 16u, "dangly tightness");
                dangly.period = payload_->f32(
                    offset + kK2MeshHeaderSize + 20u, "dangly period");
                if (!finite(dangly.displacement)) dangly.displacement = 0.0f;
                if (!finite(dangly.tightness)) dangly.tightness = 0.0f;
                if (!finite(dangly.period)) dangly.period = 0.0f;
            }
            mesh.dangly = std::move(dangly);
        }
        if (mesh.skinned && model_.game == GameVersion::Kotor2) {
            if (payload_->contains(offset + kK2SkinWeightMdxOffset, 8u)) {
                mesh.mdxWeightOffset = payload_->u32(
                    offset + kK2SkinWeightMdxOffset, "skin weight MDX channel offset");
                mesh.mdxBoneIndexOffset = payload_->u32(
                    offset + kK2SkinBoneMdxOffset, "skin bone-index MDX channel offset");
            }

            const auto readSkinArray = [&](std::size_t relativeOffset, std::size_t stride,
                                           const char* description)
                -> std::optional<std::pair<std::size_t, std::size_t>> {
                const auto definition = offset + relativeOffset;
                if (!payload_->contains(definition, kArrayDefinitionSize)) {
                    model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                        " has no complete " + description + " array definition.");
                    return std::nullopt;
                }
                const auto dataOffset = payload_->u32(definition, description);
                const auto count = payload_->u32(definition + 4u, description);
                const auto capacity = payload_->u32(definition + 8u, description);
                if (count > limits_.maximumNodes) {
                    throw ModelError(sourcePrefix(label_) + "Skin " + description +
                                     " count exceeds the configured node limit");
                }
                if (capacity != 0u && count > capacity) {
                    model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                        " has a " + description + " count larger than its capacity; the array was ignored.");
                    return std::nullopt;
                }
                if (count == 0u) return std::pair<std::size_t, std::size_t>{0u, 0u};
                if (dataOffset == 0u || dataOffset == 0xffffffffu ||
                    !payload_->contains(dataOffset, static_cast<std::size_t>(count) * stride)) {
                    model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                        " declares " + description + " data outside the MDL payload; the array was ignored.");
                    return std::nullopt;
                }
                return std::pair<std::size_t, std::size_t>{
                    static_cast<std::size_t>(dataOffset), static_cast<std::size_t>(count)};
            };

            const auto inverseQuaternions = readSkinArray(
                kK2SkinInverseQuaternionArray, 16u, "inverse-bind quaternion");
            if (inverseQuaternions) {
                mesh.inverseBindOrientations.reserve(inverseQuaternions->second);
                for (std::size_t index = 0u; index < inverseQuaternions->second; ++index) {
                    const auto record = inverseQuaternions->first + index * 16u;
                    // MdlNodeSkin32 stores Quaternion as w,x,y,z.
                    mesh.inverseBindOrientations.push_back(safeQuaternion(
                        {payload_->f32(record + 4u, "inverse-bind quaternion x"),
                         payload_->f32(record + 8u, "inverse-bind quaternion y"),
                         payload_->f32(record + 12u, "inverse-bind quaternion z"),
                         payload_->f32(record, "inverse-bind quaternion w")},
                        model_.warnings, "Skin mesh " + model_.nodes[nodeIndex].name +
                            " inverse-bind quaternion " + std::to_string(index)));
                }
            }

            const auto inverseTranslations = readSkinArray(
                kK2SkinInverseTranslationArray, 12u, "inverse-bind translation");
            if (inverseTranslations) {
                mesh.inverseBindTranslations.reserve(inverseTranslations->second);
                for (std::size_t index = 0u; index < inverseTranslations->second; ++index) {
                    const auto record = inverseTranslations->first + index * 12u;
                    mesh.inverseBindTranslations.push_back(safeVec3(
                        payload_->vec3(record, "inverse-bind translation"), model_.warnings,
                        "Skin mesh " + model_.nodes[nodeIndex].name +
                            " inverse-bind translation " + std::to_string(index)));
                }
            }

            const auto boneConstants = readSkinArray(
                kK2SkinBoneConstantsArray, 4u, "bone-constant index");
            if (boneConstants) {
                mesh.boneConstantIndices.reserve(boneConstants->second);
                for (std::size_t index = 0u; index < boneConstants->second; ++index) {
                    mesh.boneConstantIndices.push_back(payload_->i32(
                        boneConstants->first + index * 4u, "bone-constant index"));
                }
            }

            if (!mesh.inverseBindOrientations.empty() ||
                !mesh.inverseBindTranslations.empty()) {
                if (mesh.inverseBindOrientations.size() ==
                        mesh.inverseBindTranslations.size() &&
                    !mesh.inverseBindOrientations.empty()) {
                    mesh.hasInverseBindPose = true;
                } else {
                    model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                        " has mismatched inverse-bind quaternion and translation counts; CPU deformation was disabled.");
                }
            }

            if (payload_->contains(offset + kK2SkinBoneMapOffset,
                                   kK2SkinBoneMapCount * sizeof(std::int16_t))) {
                for (std::size_t index = 0u; index < mesh.skinBoneMap.size(); ++index) {
                    mesh.skinBoneMap[index] = payload_->i16(
                        offset + kK2SkinBoneMapOffset + index * sizeof(std::int16_t),
                        "skin bone map entry");
                }
                mesh.hasSkinBoneMap = true;
            }
        }
        const auto mdxDataOffset = payload_->u32(
            offset + (model_.game == GameVersion::Kotor1 ? 324u : 332u), "MDX data offset");
        const auto mdlVerticesOffset = payload_->u32(
            offset + (model_.game == GameVersion::Kotor1 ? 328u : 336u), "MDL vertex offset");

        mesh.faces.reserve(faceCount);
        std::uint32_t requiredVertices = headerVertexCount;
        for (std::uint32_t index = 0u; index < faceCount; ++index) {
            const auto faceOffset = static_cast<std::size_t>(facesOffset) + static_cast<std::size_t>(index) * kFaceSize;
            MeshFace face;
            face.normal = payload_->vec3(faceOffset, "face normal");
            face.planeDistance = payload_->f32(faceOffset + 12u, "face plane distance");
            face.surfaceMaterial = payload_->u32(faceOffset + 16u, "face surface material");
            face.adjacentFaces[0] = payload_->i16(faceOffset + 20u, "face adjacent edge 0");
            face.adjacentFaces[1] = payload_->i16(faceOffset + 22u, "face adjacent edge 1");
            face.adjacentFaces[2] = payload_->i16(faceOffset + 24u, "face adjacent edge 2");
            face.indices[0] = payload_->i16(faceOffset + 26u, "face vertex 0");
            face.indices[1] = payload_->i16(faceOffset + 28u, "face vertex 1");
            face.indices[2] = payload_->i16(faceOffset + 30u, "face vertex 2");
            for (const auto vertexIndex : face.indices) {
                if (vertexIndex >= 0) {
                    requiredVertices = std::max(requiredVertices,
                        static_cast<std::uint32_t>(vertexIndex) + 1u);
                }
            }
            mesh.faces.push_back(face);
        }
        if (requiredVertices > limits_.maximumVertices || totalVertices_ > limits_.maximumVertices - requiredVertices) {
            throw ModelError(sourcePrefix(label_) + "Model vertex count exceeds the configured limit");
        }
        totalVertices_ += requiredVertices;
        mesh.vertices.resize(requiredVertices);

        const bool mdxPositions = !mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
            (mesh.mdxBitmap & kMdxVertex) != 0u && validMdxChannel(mdxVertexOffset, mesh.mdxRowSize, 12u);
        const bool mdlPositions = mdlVerticesOffset != 0u && mdlVerticesOffset != 0xFFFFFFFFu &&
            payload_->contains(mdlVerticesOffset, static_cast<std::size_t>(requiredVertices) * 12u);
        if (!mdxPositions && !mdlPositions && requiredVertices != 0u) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                                      " has no readable vertex-position source.");
        }

        bool mdxRowsValid = mdxPositions;
        if (mdxRowsValid && requiredVertices != 0u) {
            const std::uint64_t finalRow = static_cast<std::uint64_t>(mdxDataOffset) +
                static_cast<std::uint64_t>(requiredVertices - 1u) * mesh.mdxRowSize;
            mdxRowsValid = finalRow <= mdxBytes_.size() &&
                mdxVertexOffset <= mdxBytes_.size() - static_cast<std::size_t>(finalRow) &&
                12u <= mdxBytes_.size() - static_cast<std::size_t>(finalRow) - mdxVertexOffset;
            if (!mdxRowsValid && mdlPositions) {
                model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                                          " has truncated MDX rows; MDL positions were used.");
            }
        }
        const bool mdxNormalsReadable = !mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
            (mesh.mdxBitmap & kMdxNormal) != 0u &&
            validMdxChannel(mdxNormalOffset, mesh.mdxRowSize, 12u) &&
            (requiredVertices == 0u || [&] {
                const std::uint64_t finalRow = static_cast<std::uint64_t>(mdxDataOffset) +
                    static_cast<std::uint64_t>(requiredVertices - 1u) * mesh.mdxRowSize;
                return finalRow <= mdxBytes_.size() &&
                    mdxNormalOffset <= mdxBytes_.size() - static_cast<std::size_t>(finalRow) &&
                    12u <= mdxBytes_.size() - static_cast<std::size_t>(finalRow) - mdxNormalOffset;
            }());
        if ((mesh.mdxBitmap & kMdxNormal) != 0u && !mdxNormalsReadable && requiredVertices != 0u) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                                      " has an unreadable MDX normal channel; normals were generated.");
        }

        const bool skinChannelsReadable = mesh.skinned && model_.game == GameVersion::Kotor2 &&
            !mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
            validMdxChannel(mesh.mdxWeightOffset, mesh.mdxRowSize, 16u) &&
            validMdxChannel(mesh.mdxBoneIndexOffset, mesh.mdxRowSize, 8u) &&
            (requiredVertices == 0u || [&] {
                const std::uint64_t finalRow = static_cast<std::uint64_t>(mdxDataOffset) +
                    static_cast<std::uint64_t>(requiredVertices - 1u) * mesh.mdxRowSize;
                if (finalRow > mdxBytes_.size()) return false;
                const auto row = static_cast<std::size_t>(finalRow);
                return mesh.mdxWeightOffset <= mdxBytes_.size() - row &&
                    16u <= mdxBytes_.size() - row - mesh.mdxWeightOffset &&
                    mesh.mdxBoneIndexOffset <= mdxBytes_.size() - row &&
                    8u <= mdxBytes_.size() - row - mesh.mdxBoneIndexOffset;
            }());
        if (skinChannelsReadable) {
            mesh.hasSkinChannels = true;
            mesh.skinInfluences.resize(requiredVertices);
        } else if (mesh.skinned && requiredVertices != 0u) {
            model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                (model_.game == GameVersion::Kotor2
                    ? " has no complete binary-verified MDX weight/bone-index channels."
                    : " uses a K1 skin-channel layout that has not yet been verified against the supplied binaries."));
        }

        const auto channelReadable = [&](std::uint32_t channel, std::size_t bytes) {
            if (mdxBytes_.empty() || mesh.mdxRowSize == 0u || requiredVertices == 0u ||
                !validMdxChannel(channel, mesh.mdxRowSize, bytes)) return false;
            const std::uint64_t last = static_cast<std::uint64_t>(mdxDataOffset) +
                static_cast<std::uint64_t>(requiredVertices - 1u) * mesh.mdxRowSize;
            return last <= mdxBytes_.size() && channel <= mdxBytes_.size() - last &&
                   bytes <= mdxBytes_.size() - last - channel;
        };
        const bool tangentsReadable = channelReadable(mesh.mdxTangentOffset, 36u);
        mesh.hasVertexColors = (mesh.mdxBitmap & 0x40u) != 0u &&
            channelReadable(mesh.mdxColorOffset, 4u);
        const bool uv2Readable = (mesh.mdxBitmap & 0x08u) != 0u &&
            channelReadable(mesh.mdxTexcoordOffsets[2], 8u);
        const bool uv3Readable = (mesh.mdxBitmap & 0x10u) != 0u &&
            channelReadable(mesh.mdxTexcoordOffsets[3], 8u);
        bool invalidTangent = false;
        bool invalidUv = false;
        if (!tangentsReadable && mesh.mdxTangentOffset != 0xffffffffu &&
            (mesh.mdxBitmap & 0x80u) != 0u && requiredVertices != 0u) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                " has an invalid or truncated MDX tangent channel; geometry was retained.");
        }
        ByteView mdx(mdxBytes_.data(), mdxBytes_.size(), label_.empty() ? "MDX" : label_ + " MDX");
        bool warnedNonFiniteSkinWeight = false;
        for (std::uint32_t index = 0u; index < requiredVertices; ++index) {
            auto& vertex = mesh.vertices[index];
            const auto row = static_cast<std::size_t>(mdxDataOffset) +
                             static_cast<std::size_t>(index) * mesh.mdxRowSize;
            if (mdxRowsValid) {
                vertex.position = safeVec3(mdx.vec3(row + mdxVertexOffset, "MDX position"), model_.warnings,
                                           "Mesh vertex position");
            } else if (mdlPositions) {
                vertex.position = safeVec3(payload_->vec3(static_cast<std::size_t>(mdlVerticesOffset) +
                    static_cast<std::size_t>(index) * 12u, "MDL position"), model_.warnings,
                    "Mesh vertex position");
            }
            if (mdxNormalsReadable) {
                vertex.normal = normalize(mdx.vec3(row + mdxNormalOffset, "MDX normal"));
            }
            if (!mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
                (mesh.mdxBitmap & kMdxTex0) != 0u &&
                validMdxChannel(mdxTex0Offset, mesh.mdxRowSize, 8u) &&
                row <= mdxBytes_.size() && mdxTex0Offset <= mdxBytes_.size() - row &&
                8u <= mdxBytes_.size() - row - mdxTex0Offset) {
                vertex.texcoord0 = mdx.vec2(row + mdxTex0Offset, "MDX texture coordinate 0");
            }
            if (!mdxBytes_.empty() && mesh.mdxRowSize != 0u &&
                (mesh.mdxBitmap & kMdxTex1) != 0u &&
                validMdxChannel(mdxTex1Offset, mesh.mdxRowSize, 8u) &&
                row <= mdxBytes_.size() && mdxTex1Offset <= mdxBytes_.size() - row &&
                8u <= mdxBytes_.size() - row - mdxTex1Offset) {
                vertex.texcoord1 = mdx.vec2(row + mdxTex1Offset, "MDX texture coordinate 1");
            }
            if (mesh.hasVertexColors) {
                for (std::size_t c = 0u; c < vertex.color.size(); ++c)
                    vertex.color[c] = mdx.u8(row + mesh.mdxColorOffset + c, "MDX color");
            }
            if (uv2Readable) vertex.texcoord2 = mdx.vec2(
                row + mesh.mdxTexcoordOffsets[2], "MDX texture coordinate 2");
            if (uv3Readable) vertex.texcoord3 = mdx.vec2(
                row + mesh.mdxTexcoordOffsets[3], "MDX texture coordinate 3");
            for (auto* uv : {&vertex.texcoord0, &vertex.texcoord1,
                             &vertex.texcoord2, &vertex.texcoord3}) {
                if (!finite(uv->x) || !finite(uv->y)) {
                    *uv = {}; invalidUv = true;
                }
            }
            if (tangentsReadable) {
                const auto at = row + mesh.mdxTangentOffset;
                auto& basis = vertex.tangentBasis;
                basis.bitangent = mdx.vec3(at, "MDX bitangent");
                basis.tangent = mdx.vec3(at + 12u, "MDX tangent");
                basis.normal = mdx.vec3(at + 24u, "MDX basis normal");
                const auto goodVector = [](const Vec3& v) {
                    const double length2 = static_cast<double>(v.x) * v.x +
                        static_cast<double>(v.y) * v.y + static_cast<double>(v.z) * v.z;
                    return finite(v.x) && finite(v.y) && finite(v.z) &&
                           std::isfinite(length2) && length2 > 1.0e-20;
                };
                const auto good = goodVector(basis.bitangent) && goodVector(basis.tangent) &&
                                  goodVector(basis.normal);
                if (good) {
                    const auto b = normalize(basis.bitangent);
                    const auto t = normalize(basis.tangent);
                    const auto n = normalize(basis.normal);
                    const auto c = cross(t, b);
                    const float determinant = c.x * n.x + c.y * n.y + c.z * n.z;
                    basis.valid = finite(determinant) && std::abs(determinant) > 1.0e-5f;
                }
                if (basis.valid) ++mesh.validTangentVertices;
                else {
                    invalidTangent = true;
                    // Non-finite input must never reach a GPU upload.
                    for (auto* v : {&basis.bitangent, &basis.tangent, &basis.normal})
                        if (!finite(v->x) || !finite(v->y) || !finite(v->z)) *v = {};
                }
            }
            if (skinChannelsReadable) {
                auto& influence = mesh.skinInfluences[index];
                for (std::size_t component = 0u; component < influence.weights.size(); ++component) {
                    auto value = mdx.f32(row + mesh.mdxWeightOffset + component * 4u,
                                         "MDX skin weight");
                    if (!finite(value)) {
                        value = 0.0f;
                        warnedNonFiniteSkinWeight = true;
                    }
                    influence.weights[component] = value;
                    influence.boneIndices[component] = mdx.u16(
                        row + mesh.mdxBoneIndexOffset + component * 2u,
                        "MDX skin bone index");
                }
            }
        }
        if (invalidTangent) model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
            " contains invalid tangent bases; affected vertices need generated tangents.");
        if (invalidUv) model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
            " contains non-finite texture coordinates; zero was substituted.");
        if (warnedNonFiniteSkinWeight) {
            model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                                      " contains non-finite weights; zero was substituted.");
        }

        std::vector<Vec3> generatedNormals(requiredVertices);
        mesh.indices.reserve(mesh.faces.size() * 3u);
        std::size_t skippedFaces = 0u;
        for (const auto& face : mesh.faces) {
            if (face.indices[0] < 0 || face.indices[1] < 0 || face.indices[2] < 0 ||
                static_cast<std::uint32_t>(face.indices[0]) >= requiredVertices ||
                static_cast<std::uint32_t>(face.indices[1]) >= requiredVertices ||
                static_cast<std::uint32_t>(face.indices[2]) >= requiredVertices) {
                ++skippedFaces;
                continue;
            }
            for (const auto index : face.indices)
                mesh.indices.push_back(static_cast<std::uint32_t>(index));
            const auto& a = mesh.vertices[static_cast<std::size_t>(face.indices[0])].position;
            const auto& b = mesh.vertices[static_cast<std::size_t>(face.indices[1])].position;
            const auto& c = mesh.vertices[static_cast<std::size_t>(face.indices[2])].position;
            const auto normal = cross(subtract(b, a), subtract(c, a));
            generatedNormals[static_cast<std::size_t>(face.indices[0])] = add(
                generatedNormals[static_cast<std::size_t>(face.indices[0])], normal);
            generatedNormals[static_cast<std::size_t>(face.indices[1])] = add(
                generatedNormals[static_cast<std::size_t>(face.indices[1])], normal);
            generatedNormals[static_cast<std::size_t>(face.indices[2])] = add(
                generatedNormals[static_cast<std::size_t>(face.indices[2])], normal);
        }
        if (skippedFaces != 0u) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name + " skipped " +
                                      std::to_string(skippedFaces) + " face(s) with invalid indices.");
        }
        if (!mdxNormalsReadable) {
            for (std::size_t index = 0u; index < mesh.vertices.size(); ++index) {
                mesh.vertices[index].normal = normalize(generatedNormals[index]);
            }
        }
        if (mesh.dangly && !mesh.dangly->constraints.empty() &&
            mesh.dangly->constraints.size() != mesh.vertices.size()) {
            model_.warnings.push_back("Dangly mesh " + model_.nodes[nodeIndex].name +
                " has " + std::to_string(mesh.dangly->constraints.size()) +
                " constraints for " + std::to_string(mesh.vertices.size()) +
                " vertices; missing entries are treated as pinned and extras are ignored.");
        }
        if (mesh.skinned && model_.game == GameVersion::Kotor2 &&
            !(mesh.hasSkinChannels && mesh.hasSkinBoneMap && mesh.hasInverseBindPose)) {
            model_.warnings.push_back("Skin mesh " + model_.nodes[nodeIndex].name +
                " does not contain every binary-verified K2 skinning input; its stored geometry will be used as a fallback.");
        }
        if (requiredVertices != headerVertexCount) {
            model_.warnings.push_back("Mesh on " + model_.nodes[nodeIndex].name +
                                      " required more vertices than its header declared; face indices were authoritative.");
        }

        parseAabbTree(offset, nodeIndex, mesh);
        postProcessLightsaber(nodeIndex, mesh);
        model_.nodes[nodeIndex].mesh = model_.meshes.size();
        model_.meshes.push_back(std::move(mesh));
    }

    void calculateWorldTransformsAndBounds() {
        const auto pose = bindPose(model_);
        model_.worldTransforms = pose.worldTransforms;
        model_.bounds = pose.bounds;
    }

    const std::vector<std::uint8_t>& mdlBytes_;
    const std::vector<std::uint8_t>& mdxBytes_;
    std::string label_;
    const ParseLimits& limits_;
    ByteView whole_;
    std::unique_ptr<ByteView> payload_;
    Model model_;
    std::uint32_t rootNodeOffset_{};
    std::uint32_t declaredNodeCount_{};
    std::uint32_t animationsOffset_{};
    std::uint32_t namesOffset_{};
    std::uint32_t namesCount_{};
    std::vector<std::string> names_;
    std::unordered_map<std::uint32_t, std::size_t> nodeByOffset_;
    std::size_t totalVertices_{};
    std::size_t totalAnimationNodes_{};
    std::size_t totalAnimationEvents_{};
    std::size_t totalControllers_{};
    std::size_t totalControllerValues_{};
};

Quaternion normalizedQuaternion(Quaternion value) noexcept {
    if (!finite(value.x) || !finite(value.y) || !finite(value.z) || !finite(value.w)) return {};
    const double lengthSquared = static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z +
        static_cast<double>(value.w) * value.w;
    if (lengthSquared < 1.0e-20 || !std::isfinite(lengthSquared)) return {};
    const auto reciprocal = static_cast<float>(1.0 / std::sqrt(lengthSquared));
    value.x *= reciprocal;
    value.y *= reciprocal;
    value.z *= reciprocal;
    value.w *= reciprocal;
    return value;
}

Quaternion multiplyQuaternion(const Quaternion& left, const Quaternion& right) noexcept {
    return normalizedQuaternion({
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z});
}

Vec3 rotateVector(const Quaternion& orientation, const Vec3& value) noexcept {
    const auto quaternion = normalizedQuaternion(orientation);
    const Vec3 axis{quaternion.x, quaternion.y, quaternion.z};
    const auto twiceCross = cross(axis, value);
    const Vec3 twice{twiceCross.x * 2.0f, twiceCross.y * 2.0f, twiceCross.z * 2.0f};
    const auto correction = cross(axis, twice);
    return {value.x + quaternion.w * twice.x + correction.x,
            value.y + quaternion.w * twice.y + correction.y,
            value.z + quaternion.w * twice.z + correction.z};
}

Quaternion packedQuaternion(std::uint32_t packed) noexcept {
    // The game stores x/y in 11-bit fields and z in a 10-bit field. The
    // component ranges and positive-w reconstruction below match
    // NewController::GetQuaternionFromIndexLocation in the supplied binaries.
    Quaternion value;
    value.x = static_cast<float>(packed & 0x7ffu) * (2.0f / 2046.0f) - 1.0f;
    value.y = static_cast<float>((packed >> 11u) & 0x7ffu) * (2.0f / 2046.0f) - 1.0f;
    value.z = static_cast<float>(packed >> 22u) * (2.0f / 1022.0f) - 1.0f;
    const float xyzLengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (xyzLengthSquared <= 1.0f) {
        value.w = std::sqrt(std::max(0.0f, 1.0f - xyzLengthSquared));
        return value;
    }
    const auto length = std::sqrt(xyzLengthSquared);
    if (length > 0.0f && finite(length)) {
        value.x /= length;
        value.y /= length;
        value.z /= length;
    } else {
        value = {};
    }
    value.w = 0.0f;
    return value;
}

float clampedLerp(float left, float right, float alpha) noexcept {
    return left + (right - left) * std::clamp(alpha, 0.0f, 1.0f);
}

Vec3 lerp(const Vec3& left, const Vec3& right, float alpha) noexcept {
    return {clampedLerp(left.x, right.x, alpha),
            clampedLerp(left.y, right.y, alpha),
            clampedLerp(left.z, right.z, alpha)};
}

float cubicBezier(float p0, float p1, float p2, float p3, float alpha) noexcept {
    const auto t = std::clamp(alpha, 0.0f, 1.0f);
    const auto inverse = 1.0f - t;
    return inverse * inverse * inverse * p0 + 3.0f * inverse * inverse * t * p1 +
           3.0f * inverse * t * t * p2 + t * t * t * p3;
}

Vec3 cubicBezier(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
                 float alpha) noexcept {
    return {cubicBezier(p0.x, p1.x, p2.x, p3.x, alpha),
            cubicBezier(p0.y, p1.y, p2.y, p3.y, alpha),
            cubicBezier(p0.z, p1.z, p2.z, p3.z, alpha)};
}

Quaternion slerp(Quaternion left, Quaternion right, float alpha) noexcept {
    left = normalizedQuaternion(left);
    right = normalizedQuaternion(right);
    float dot = left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
    if (dot < 0.0f) {
        dot = -dot;
        right.x = -right.x;
        right.y = -right.y;
        right.z = -right.z;
        right.w = -right.w;
    }
    const auto t = std::clamp(alpha, 0.0f, 1.0f);
    if (dot > 0.9995f) {
        return normalizedQuaternion({clampedLerp(left.x, right.x, t),
                                     clampedLerp(left.y, right.y, t),
                                     clampedLerp(left.z, right.z, t),
                                     clampedLerp(left.w, right.w, t)});
    }
    dot = std::clamp(dot, -1.0f, 1.0f);
    const auto angle = std::acos(dot);
    const auto sine = std::sin(angle);
    if (std::fabs(sine) < 1.0e-7f) return left;
    const auto leftWeight = std::sin((1.0f - t) * angle) / sine;
    const auto rightWeight = std::sin(t * angle) / sine;
    return normalizedQuaternion({left.x * leftWeight + right.x * rightWeight,
                                 left.y * leftWeight + right.y * rightWeight,
                                 left.z * leftWeight + right.z * rightWeight,
                                 left.w * leftWeight + right.w * rightWeight});
}

struct KeySample {
    std::size_t first{};
    std::size_t second{};
    float alpha{};
    bool valid{};
};

KeySample controllerKeySample(const Node& node, const Controller& controller, float time) noexcept {
    const auto rows = static_cast<std::size_t>(controller.rowCount);
    const auto start = static_cast<std::size_t>(controller.timeIndex);
    if (rows == 0u || start > node.controllerDataWords.size() ||
        rows > node.controllerDataWords.size() - start) {
        return {};
    }
    if (!finite(time)) time = 0.0f;
    const auto firstTime = wordAsFloat(node.controllerDataWords[start]);
    if (rows == 1u || !finite(firstTime) || time <= firstTime) return {0u, 0u, 0.0f, true};
    float previous = firstTime;
    for (std::size_t key = 1u; key < rows; ++key) {
        const auto current = wordAsFloat(node.controllerDataWords[start + key]);
        if (!finite(current)) continue;
        if (time <= current) {
            const auto denominator = current - previous;
            const auto alpha = denominator > 0.0f && finite(previous)
                ? std::clamp((time - previous) / denominator, 0.0f, 1.0f)
                : 1.0f;
            return {key - 1u, key, alpha, true};
        }
        previous = current;
    }
    return {rows - 1u, rows - 1u, 0.0f, true};
}

std::optional<float> controllerFloat(const Node& node, const Controller& controller,
                                     float time) noexcept {
    if (controller.valueKind != ControllerValueKind::Float) return std::nullopt;
    const auto sample = controllerKeySample(node, controller, time);
    if (!sample.valid) return std::nullopt;
    const auto start = static_cast<std::size_t>(controller.dataIndex);
    if (controller.interpolation == ControllerInterpolation::Bezier) {
        const auto rows = static_cast<std::size_t>(controller.rowCount);
        const auto requiredWords = rows * 3u;
        if (start > node.controllerDataWords.size() ||
            requiredWords > node.controllerDataWords.size() - start) {
            return std::nullopt;
        }

        // NewController::GetFloatValue in both supplied K2 binaries uses a
        // scalar-specific word layout rather than the vector controller's
        // conventional [value, incoming, outgoing] triplets. For an interior
        // key K it reads:
        //   start       = words[3*K - 3]
        //   end         = words[3*K - 1]
        //   control one = start + words[3*K]
        //   control two = end   + words[3*K + 1]
        // Preserve that indexing exactly, including the engine's endpoint
        // behavior, instead of normalizing it into the vector layout.
        if (sample.first == sample.second) {
            const auto value = wordAsFloat(
                node.controllerDataWords[start + sample.first * 3u]);
            return finite(value) ? std::optional<float>{value} : std::nullopt;
        }

        const auto key = sample.second;
        if (key == 0u || key >= rows) return std::nullopt;
        const auto previousBase = start + (key - 1u) * 3u;
        const auto currentBase = start + key * 3u;
        const auto p0 = wordAsFloat(node.controllerDataWords[previousBase]);
        const auto p3 = wordAsFloat(node.controllerDataWords[previousBase + 2u]);
        const auto p1 = p0 + wordAsFloat(node.controllerDataWords[currentBase]);
        const auto p2 = p3 + wordAsFloat(node.controllerDataWords[currentBase + 1u]);
        const auto result = cubicBezier(p0, p1, p2, p3, sample.alpha);
        return finite(result) ? std::optional<float>{result} : std::nullopt;
    }
    if (controller.interpolation != ControllerInterpolation::Linear ||
        start > node.controllerDataWords.size() ||
        sample.second >= node.controllerDataWords.size() - start) {
        return std::nullopt;
    }
    const auto left = wordAsFloat(node.controllerDataWords[start + sample.first]);
    const auto right = wordAsFloat(node.controllerDataWords[start + sample.second]);
    const auto result = clampedLerp(left, right, sample.alpha);
    return finite(result) ? std::optional<float>{result} : std::nullopt;
}

std::optional<Vec3> controllerVector(const Node& node, const Controller& controller,
                                     float time) noexcept {
    if (controller.valueKind != ControllerValueKind::Vector) return std::nullopt;
    const auto sample = controllerKeySample(node, controller, time);
    if (!sample.valid) return std::nullopt;
    const auto start = static_cast<std::size_t>(controller.dataIndex);
    const auto readVector = [&](std::size_t word) -> std::optional<Vec3> {
        if (word > node.controllerDataWords.size() ||
            3u > node.controllerDataWords.size() - word) return std::nullopt;
        Vec3 result{wordAsFloat(node.controllerDataWords[word]),
                    wordAsFloat(node.controllerDataWords[word + 1u]),
                    wordAsFloat(node.controllerDataWords[word + 2u])};
        if (!finite(result.x) || !finite(result.y) || !finite(result.z)) return std::nullopt;
        return result;
    };
    if (controller.interpolation == ControllerInterpolation::Bezier) {
        const auto firstBase = start + sample.first * 9u;
        const auto secondBase = start + sample.second * 9u;
        const auto p0 = readVector(firstBase);
        if (!p0) return std::nullopt;
        if (sample.first == sample.second) return p0;
        const auto firstOut = readVector(firstBase + 6u);
        const auto p3 = readVector(secondBase);
        const auto secondIn = readVector(secondBase + 3u);
        if (!firstOut || !p3 || !secondIn) return std::nullopt;
        return cubicBezier(*p0, add(*p0, *firstOut), add(*p3, *secondIn), *p3,
                           sample.alpha);
    }
    if (controller.interpolation != ControllerInterpolation::Linear) return std::nullopt;
    const auto left = readVector(start + sample.first * 3u);
    const auto right = readVector(start + sample.second * 3u);
    if (!left || !right) return std::nullopt;
    return lerp(*left, *right, sample.alpha);
}

std::optional<Quaternion> controllerQuaternion(const Node& node, const Controller& controller,
                                               float time) noexcept {
    if (controller.valueKind != ControllerValueKind::PackedQuaternion &&
        controller.valueKind != ControllerValueKind::Quaternion) return std::nullopt;
    if (controller.interpolation != ControllerInterpolation::Linear) return std::nullopt;
    const auto sample = controllerKeySample(node, controller, time);
    if (!sample.valid) return std::nullopt;
    const auto start = static_cast<std::size_t>(controller.dataIndex);
    const auto read = [&](std::size_t key) -> std::optional<Quaternion> {
        if (controller.valueKind == ControllerValueKind::PackedQuaternion) {
            if (start > node.controllerDataWords.size() || key >= node.controllerDataWords.size() - start)
                return std::nullopt;
            return packedQuaternion(node.controllerDataWords[start + key]);
        }
        const auto word = start + key * 4u;
        if (word > node.controllerDataWords.size() ||
            4u > node.controllerDataWords.size() - word) return std::nullopt;
        return normalizedQuaternion({wordAsFloat(node.controllerDataWords[word]),
                                     wordAsFloat(node.controllerDataWords[word + 1u]),
                                     wordAsFloat(node.controllerDataWords[word + 2u]),
                                     wordAsFloat(node.controllerDataWords[word + 3u])});
    };
    const auto left = read(sample.first);
    const auto right = read(sample.second);
    if (!left || !right) return std::nullopt;
    return slerp(*left, *right, sample.alpha);
}

void applyMaterialControllers(const Node& node, float time,
                              MaterialValueSource source,
                              NodeMaterialState& material) noexcept {
    // Callers establish the target part is a mesh. Animation-node flags are
    // not the discriminator: Control resolves offsets from the target part.
    for (const auto& controller : node.controllers) {
        if (controller.type == kControllerAlpha) {
            if (const auto value = controllerFloat(node, controller, time);
                value && finite(*value)) {
                material.alpha = *value;
                material.alphaSource = source;
            }
        } else if (controller.type == kControllerSelfIllumColor) {
            if (const auto value = controllerVector(node, controller, time);
                value && finite(value->x) && finite(value->y) && finite(value->z)) {
                material.selfIllumination = {{value->x, value->y, value->z}};
                material.selfIlluminationSource = source;
            }
        }
    }
}

void applyLightControllers(const Node& node,float time,MaterialValueSource source,
                           NodeLightState& light) noexcept {
    for(const auto& controller:node.controllers) {
        if(controller.type==kControllerLightColor) {
            if(const auto v=controllerVector(node,controller,time);
               v && finite(v->x) && finite(v->y) && finite(v->z)) {
                light.color={{v->x,v->y,v->z}};light.sources[0]=source;
            }
            continue;
        }
        float* field=nullptr;std::size_t slot=0;
        switch(controller.type) {
        case kControllerLightRadius: field=&light.radius;slot=1;break;
        case kControllerLightShadowRadius: field=&light.shadowRadius;slot=2;break;
        case kControllerLightVerticalDisplacement: field=&light.verticalDisplacement;slot=3;break;
        case kControllerLightMultiplier: field=&light.multiplier;slot=4;break;
        default: break;
        }
        if(field) if(const auto v=controllerFloat(node,controller,time);v && finite(*v)) {
            *field=*v;light.sources[slot]=source;
        }
    }
}

void applyEmitterControllers(const Node& node, float time, MaterialValueSource source,
                             NodeEmitterState& emitter) noexcept {
    for (const auto& controller : node.controllers) {
        for (const auto& binding : kEmitterScalarBindings) {
            if (controller.type != binding.id) continue;
            if (const auto value=controllerFloat(node,controller,time); value && finite(*value)) {
                emitter.*(binding.field)=*value;
                emitter.sources[static_cast<std::size_t>(binding.property)]=source;
            }
            break;
        }
        std::array<float,3>* field=nullptr;
        EmitterProperty property=EmitterProperty::ColorStart;
        switch (controller.type) {
        case kControllerEmitterColorStart: field=&emitter.colorStart; break;
        case kControllerEmitterColorMid: field=&emitter.colorMid; property=EmitterProperty::ColorMid; break;
        case kControllerEmitterColorEnd: field=&emitter.colorEnd; property=EmitterProperty::ColorEnd; break;
        default: break;
        }
        if (field) if (const auto v=controllerVector(node,controller,time);
            v && finite(v->x) && finite(v->y) && finite(v->z)) {
            *field={{v->x,v->y,v->z}};
            emitter.sources[static_cast<std::size_t>(property)]=source;
        }
    }
}

void calculatePoseTransformsAndBounds(const Model& model, Pose& pose) {
    const auto nodeCount = model.nodes.size();
    pose.worldTransforms.assign(nodeCount, identityMatrix());
    std::vector<Vec3> boneWorldPositions(nodeCount);
    std::vector<Quaternion> boneWorldOrientations(nodeCount);

    for (std::size_t index = 0u; index < nodeCount; ++index) {
        const auto localPosition = index < pose.positions.size()
            ? pose.positions[index] : model.nodes[index].position;
        const auto localOrientation = index < pose.orientations.size()
            ? normalizedQuaternion(pose.orientations[index])
            : normalizedQuaternion(model.nodes[index].orientation);
        const auto localScale = index < pose.scales.size() ? pose.scales[index] : 1.0f;
        const auto localTransform = nodeTransform(localPosition, localOrientation, localScale);
        const auto parent = model.nodes[index].parent;
        if (parent && *parent < index && *parent < nodeCount) {
            pose.worldTransforms[index] = multiply(pose.worldTransforms[*parent], localTransform);
            boneWorldOrientations[index] = multiplyQuaternion(
                boneWorldOrientations[*parent], localOrientation);
            boneWorldPositions[index] = add(
                boneWorldPositions[*parent],
                rotateVector(boneWorldOrientations[*parent], localPosition));
        } else {
            pose.worldTransforms[index] = localTransform;
            boneWorldOrientations[index] = localOrientation;
            boneWorldPositions[index] = localPosition;
        }
    }

    pose.deformedMeshes.clear();
    pose.deformedMeshes.resize(model.meshes.size());
    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        auto& deformed = pose.deformedMeshes[meshIndex];
        if (model.game != GameVersion::Kotor2 || !mesh.skinned ||
            !mesh.hasSkinChannels || !mesh.hasSkinBoneMap ||
            !mesh.hasInverseBindPose ||
            mesh.skinInfluences.size() != mesh.vertices.size()) {
            continue;
        }

        const auto boneCount = std::min({boneWorldPositions.size(),
                                         boneWorldOrientations.size(),
                                         mesh.inverseBindOrientations.size(),
                                         mesh.inverseBindTranslations.size()});
        if (boneCount == 0u) continue;

        std::vector<Quaternion> skinOrientations(boneCount);
        std::vector<Vec3> skinTranslations(boneCount);
        for (std::size_t bone = 0u; bone < boneCount; ++bone) {
            // RenderSkinned composes current world-bone transforms with the
            // model-space inverse bind generated by PartSkin::PostProcess.
            skinOrientations[bone] = multiplyQuaternion(
                boneWorldOrientations[bone], mesh.inverseBindOrientations[bone]);
            skinTranslations[bone] = add(
                boneWorldPositions[bone],
                rotateVector(boneWorldOrientations[bone],
                             mesh.inverseBindTranslations[bone]));
        }

        deformed.positions.resize(mesh.vertices.size());
        deformed.normals.resize(mesh.vertices.size());
        for (std::size_t vertexIndex = 0u; vertexIndex < mesh.vertices.size(); ++vertexIndex) {
            const auto& vertex = mesh.vertices[vertexIndex];
            const auto& influence = mesh.skinInfluences[vertexIndex];
            Vec3 position{};
            Vec3 normal{};
            bool contributed = false;
            for (std::size_t component = 0u; component < influence.weights.size(); ++component) {
                const auto weight = influence.weights[component];
                if (!finite(weight) || weight == 0.0f) continue;
                const auto localBone = static_cast<std::size_t>(influence.boneIndices[component]);
                if (localBone >= mesh.skinBoneMap.size()) continue;
                const auto mappedBone = mesh.skinBoneMap[localBone];
                if (mappedBone < 0 || static_cast<std::size_t>(mappedBone) >= boneCount) continue;
                const auto bone = static_cast<std::size_t>(mappedBone);
                const auto rotatedPosition = rotateVector(skinOrientations[bone], vertex.position);
                const auto skinnedPosition = add(rotatedPosition, skinTranslations[bone]);
                const auto skinnedNormal = rotateVector(skinOrientations[bone], vertex.normal);
                position.x += weight * skinnedPosition.x;
                position.y += weight * skinnedPosition.y;
                position.z += weight * skinnedPosition.z;
                normal.x += weight * skinnedNormal.x;
                normal.y += weight * skinnedNormal.y;
                normal.z += weight * skinnedNormal.z;
                contributed = true;
            }
            if (!contributed) {
                position = vertex.position;
                normal = vertex.normal;
            } else {
                normal = normalize(normal);
            }
            deformed.positions[vertexIndex] = position;
            deformed.normals[vertexIndex] = normal;
        }
        deformed.valid = true;
    }

    Bounds bounds;
    const auto extendBounds = [&bounds](const Vec3& point) {
        if (!bounds.valid) {
            bounds.minimum = bounds.maximum = point;
            bounds.valid = true;
            return;
        }
        bounds.minimum.x = std::min(bounds.minimum.x, point.x);
        bounds.minimum.y = std::min(bounds.minimum.y, point.y);
        bounds.minimum.z = std::min(bounds.minimum.z, point.z);
        bounds.maximum.x = std::max(bounds.maximum.x, point.x);
        bounds.maximum.y = std::max(bounds.maximum.y, point.y);
        bounds.maximum.z = std::max(bounds.maximum.z, point.z);
    };

    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        const auto& deformed = pose.deformedMeshes[meshIndex];
        if (deformed.valid && deformed.positions.size() == mesh.vertices.size()) {
            for (const auto& point : deformed.positions) extendBounds(point);
            continue;
        }
        if (mesh.nodeIndex >= pose.worldTransforms.size()) continue;
        const auto& transform = pose.worldTransforms[mesh.nodeIndex];
        for (const auto& vertex : mesh.vertices)
            extendBounds(transformPoint(transform, vertex.position));
    }
    pose.bounds = bounds;
}

std::optional<std::size_t> animationIndexByName(const Model& model,
                                                const std::string& name) {
    const auto wanted = lowercase(name);
    for (std::size_t index = 0u; index < model.animations.size(); ++index) {
        if (lowercase(model.animations[index].name) == wanted) return index;
    }
    return std::nullopt;
}

std::size_t contiguousHierarchySize(const std::vector<const Model*>& hierarchy) noexcept {
    std::size_t count = 0u;
    while (count < hierarchy.size() && hierarchy[count] != nullptr) ++count;
    return count;
}

float inheritedPositionScale(const std::vector<const Model*>& hierarchy,
                             std::size_t ownerIndex) noexcept {
    float result = 1.0f;
    const auto count = std::min(ownerIndex, contiguousHierarchySize(hierarchy));
    for (std::size_t index = 0u; index < count; ++index)
        result *= hierarchy[index]->animationScale;
    return result;
}

std::vector<bool> controlledAnimationNodes(const Animation& animation,
                                           std::optional<std::size_t>& rootIndex) {
    std::vector<bool> controlled(animation.nodes.size(), true);
    rootIndex.reset();
    if (animation.root.empty() || lowercase(animation.root) == "nullptr") return controlled;

    for (std::size_t index = 0u; index < animation.nodes.size(); ++index) {
        // Animation::PostProcess uses strcmp for this lookup, not stricmp.
        if (animation.nodes[index].name == animation.root) {
            rootIndex = index;
            break;
        }
    }
    if (!rootIndex) return controlled;

    std::fill(controlled.begin(), controlled.end(), false);
    std::vector<std::size_t> pending{*rootIndex};
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (index >= animation.nodes.size() || controlled[index]) continue;
        controlled[index] = true;
        const auto& node = animation.nodes[index];
        for (const auto child : node.children) {
            if (child < animation.nodes.size() && !controlled[child]) pending.push_back(child);
        }
    }
    return controlled;
}

std::vector<std::optional<std::size_t>> synchronizedAnimationNodes(
    const Model& targetModel, const Animation& animation) {
    std::vector<std::optional<std::size_t>> result(animation.nodes.size());
    if (targetModel.nodes.empty() || animation.nodes.empty()) return result;

    const auto firstRoot = [](const auto& nodes) -> std::optional<std::size_t> {
        for (std::size_t index = 0u; index < nodes.size(); ++index) {
            if (!nodes[index].parent) return index;
        }
        return std::nullopt;
    };
    const auto sourceRoot = firstRoot(animation.nodes);
    const auto targetRoot = firstRoot(targetModel.nodes);
    if (!sourceRoot || !targetRoot) return result;

    std::vector<bool> visited(animation.nodes.size(), false);
    std::function<void(std::size_t, std::size_t)> synchronize;
    synchronize = [&](std::size_t sourceIndex, std::size_t targetIndex) {
        if (sourceIndex >= animation.nodes.size() ||
            targetIndex >= targetModel.nodes.size() || visited[sourceIndex]) return;
        visited[sourceIndex] = true;
        result[sourceIndex] = targetIndex;

        const auto& source = animation.nodes[sourceIndex];
        const auto& target = targetModel.nodes[targetIndex];
        for (const auto sourceChild : source.children) {
            if (sourceChild >= animation.nodes.size()) continue;
            const auto wanted = lowercase(animation.nodes[sourceChild].name);
            std::optional<std::size_t> targetChild;
            for (const auto candidate : target.children) {
                if (candidate < targetModel.nodes.size() &&
                    lowercase(targetModel.nodes[candidate].name) == wanted) {
                    targetChild = candidate;
                    break;
                }
            }
            // SynchronizeNodes assigns a fresh animation-only ID when a child
            // does not exist in the model and does not descend that unmatched
            // branch. It cannot map back into a different part of the target.
            if (targetChild) synchronize(sourceChild, *targetChild);
        }
    };

    // SynchronizeTree pairs the two root pointers directly and copies the model
    // root ID before it performs any name comparisons on their children.
    synchronize(*sourceRoot, *targetRoot);
    return result;
}

void appendForwardEvents(std::vector<AnimationEventOccurrence>& result,
                         const Animation& animation, float startTime, float endTime) {
    for (std::size_t index = 0u; index < animation.events.size(); ++index) {
        const auto& event = animation.events[index];
        if (event.time > startTime && event.time <= endTime)
            result.push_back({index, event.time, event.name});
    }
}

void appendReverseEvents(std::vector<AnimationEventOccurrence>& result,
                         const Animation& animation, float startTime, float endTime) {
    for (std::size_t remaining = animation.events.size(); remaining != 0u; --remaining) {
        const auto index = remaining - 1u;
        const auto& event = animation.events[index];
        if (event.time >= endTime && event.time < startTime)
            result.push_back({index, event.time, event.name});
    }
}

} // namespace

Model decodeModelBytes(const std::vector<std::uint8_t>& mdlBytes,
                       const std::vector<std::uint8_t>& mdxBytes,
                       const std::string& sourceLabel,
                       const ParseLimits& limits) {
    return Parser(mdlBytes, mdxBytes, sourceLabel, limits).parse();
}

Model readModelFile(const std::filesystem::path& mdlPath,
                    const std::optional<std::filesystem::path>& mdxPath,
                    const ParseLimits& limits) {
    const auto mdl = readBoundedFile(mdlPath, limits.maximumMdlBytes);
    std::vector<std::uint8_t> mdx;
    std::optional<std::filesystem::path> companion = mdxPath;
    if (!companion) companion = caseInsensitiveSibling(mdlPath, ".mdx");
    if (companion) mdx = readBoundedFile(*companion, limits.maximumMdxBytes);
    return decodeModelBytes(mdl, mdx, neoshared::pathToUtf8(mdlPath), limits);
}

Mat4 identityMatrix() noexcept {
    Mat4 result;
    result.values = {1.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 1.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 1.0f, 0.0f,
                     0.0f, 0.0f, 0.0f, 1.0f};
    return result;
}

Mat4 multiply(const Mat4& left, const Mat4& right) noexcept {
    Mat4 result{};
    for (std::size_t column = 0u; column < 4u; ++column) {
        for (std::size_t row = 0u; row < 4u; ++row) {
            float value = 0.0f;
            for (std::size_t inner = 0u; inner < 4u; ++inner) {
                value += left.values[inner * 4u + row] * right.values[column * 4u + inner];
            }
            result.values[column * 4u + row] = value;
        }
    }
    return result;
}

Mat4 nodeTransform(const Vec3& position, const Quaternion& orientation) noexcept {
    const float xx = orientation.x * orientation.x;
    const float yy = orientation.y * orientation.y;
    const float zz = orientation.z * orientation.z;
    const float xy = orientation.x * orientation.y;
    const float xz = orientation.x * orientation.z;
    const float yz = orientation.y * orientation.z;
    const float wx = orientation.w * orientation.x;
    const float wy = orientation.w * orientation.y;
    const float wz = orientation.w * orientation.z;
    Mat4 result = identityMatrix();
    result.values[0] = 1.0f - 2.0f * (yy + zz);
    result.values[1] = 2.0f * (xy + wz);
    result.values[2] = 2.0f * (xz - wy);
    result.values[4] = 2.0f * (xy - wz);
    result.values[5] = 1.0f - 2.0f * (xx + zz);
    result.values[6] = 2.0f * (yz + wx);
    result.values[8] = 2.0f * (xz + wy);
    result.values[9] = 2.0f * (yz - wx);
    result.values[10] = 1.0f - 2.0f * (xx + yy);
    result.values[12] = position.x;
    result.values[13] = position.y;
    result.values[14] = position.z;
    return result;
}

Mat4 nodeTransform(const Vec3& position, const Quaternion& orientation, float uniformScale) noexcept {
    auto result = nodeTransform(position, orientation);
    if (!finite(uniformScale)) uniformScale = 1.0f;
    for (std::size_t column = 0u; column < 3u; ++column) {
        for (std::size_t row = 0u; row < 3u; ++row) {
            result.values[column * 4u + row] *= uniformScale;
        }
    }
    return result;
}

Vec3 transformPoint(const Mat4& matrix, const Vec3& point) noexcept {
    return {matrix.values[0] * point.x + matrix.values[4] * point.y +
                matrix.values[8] * point.z + matrix.values[12],
            matrix.values[1] * point.x + matrix.values[5] * point.y +
                matrix.values[9] * point.z + matrix.values[13],
            matrix.values[2] * point.x + matrix.values[6] * point.y +
                matrix.values[10] * point.z + matrix.values[14]};
}

Vec3 transformDirection(const Mat4& matrix, const Vec3& direction) noexcept {
    return normalize({matrix.values[0] * direction.x + matrix.values[4] * direction.y +
                          matrix.values[8] * direction.z,
                      matrix.values[1] * direction.x + matrix.values[5] * direction.y +
                          matrix.values[9] * direction.z,
                      matrix.values[2] * direction.x + matrix.values[6] * direction.y +
                          matrix.values[10] * direction.z});
}

std::optional<AabbHit> hitTestAabb(const Model& model, std::size_t nodeIndex,
                                   const Vec3& start, const Vec3& end,
                                   const AabbHitOptions& options) {
    if (nodeIndex >= model.nodes.size() || !finite(options.scale) ||
        !finite(start.x) || !finite(start.y) || !finite(start.z) ||
        !finite(end.x) || !finite(end.y) || !finite(end.z)) {
        return std::nullopt;
    }
    const auto& modelNode = model.nodes[nodeIndex];
    if (!modelNode.aabb || !modelNode.aabb->root || !modelNode.mesh ||
        *modelNode.mesh >= model.meshes.size()) {
        return std::nullopt;
    }
    const auto& tree = *modelNode.aabb;
    const auto& mesh = model.meshes[*modelNode.mesh];
    if (*tree.root >= tree.nodes.size()) return std::nullopt;

    const auto segmentIntersectsBox = [](const Vec3& from, const Vec3& to,
                                         const Vec3& minimum,
                                         const Vec3& maximum) noexcept {
        float first = 0.0f;
        float last = 1.0f;
        const std::array<float, 3> origin{{from.x, from.y, from.z}};
        const std::array<float, 3> target{{to.x, to.y, to.z}};
        const std::array<float, 3> low{{minimum.x, minimum.y, minimum.z}};
        const std::array<float, 3> high{{maximum.x, maximum.y, maximum.z}};
        for (std::size_t axis = 0u; axis < origin.size(); ++axis) {
            if (low[axis] > high[axis]) return false;
            const auto delta = target[axis] - origin[axis];
            if (delta == 0.0f) {
                if (origin[axis] < low[axis] || origin[axis] > high[axis])
                    return false;
                continue;
            }
            auto enter = (low[axis] - origin[axis]) / delta;
            auto leave = (high[axis] - origin[axis]) / delta;
            if (enter > leave) std::swap(enter, leave);
            first = std::max(first, enter);
            last = std::min(last, leave);
            if (first > last) return false;
        }
        return true;
    };

    const auto polygonHit = [](const MeshFace& face,
                               const std::array<Vec3, 3>& vertices,
                               const Vec3& from, const Vec3& to,
                               Vec3& intersection) noexcept {
        const auto dot3 = [](const Vec3& left, const Vec3& right) noexcept {
            return left.x * right.x + left.y * right.y + left.z * right.z;
        };
        if (!finite(face.normal.x) || !finite(face.normal.y) ||
            !finite(face.normal.z) || !finite(face.planeDistance)) {
            return false;
        }
        const auto startDistance = dot3(face.normal, from) + face.planeDistance;
        const auto endDistance = dot3(face.normal, to) + face.planeDistance;
        // polyhit is directed and uses exact comparisons for this plane gate.
        if (startDistance == endDistance || startDistance < 0.0f ||
            endDistance > 0.0f) {
            return false;
        }
        const auto fraction = startDistance / (startDistance - endDistance);
        const auto inverse = 1.0f - fraction;
        intersection = {from.x * inverse + to.x * fraction,
                        from.y * inverse + to.y * fraction,
                        from.z * inverse + to.z * fraction};

        constexpr float insideTolerance = -1.0e-5f;
        for (std::size_t index = 0u; index < vertices.size(); ++index) {
            const auto& current = vertices[index];
            const auto& next = vertices[(index + 1u) % vertices.size()];
            const auto edge = subtract(next, current);
            const auto relative = subtract(intersection, current);
            if (dot3(face.normal, cross(edge, relative)) < insideTolerance)
                return false;
        }
        return true;
    };

    std::uint32_t directionMask = end.x > start.x ? kAabbPositiveX : kAabbNegativeX;
    directionMask |= end.y > start.y ? kAabbPositiveY : kAabbNegativeY;
    directionMask |= end.z > start.z ? kAabbPositiveZ : kAabbNegativeZ;

    Vec3 mutableEnd = end;
    std::optional<AabbHit> result;
    std::size_t acceptedHits = 0u;
    std::vector<bool> active(tree.nodes.size(), false);

    std::function<std::size_t(std::size_t, std::size_t)> traverse;
    traverse = [&](std::size_t treeIndex, std::size_t depth) -> std::size_t {
        if (treeIndex >= tree.nodes.size() || depth > tree.nodes.size() ||
            active[treeIndex]) {
            return 0u;
        }
        const auto& node = tree.nodes[treeIndex];
        const Vec3 minimum{node.minimum.x * options.scale,
                           node.minimum.y * options.scale,
                           node.minimum.z * options.scale};
        const Vec3 maximum{node.maximum.x * options.scale,
                           node.maximum.y * options.scale,
                           node.maximum.z * options.scale};
        if (!segmentIntersectsBox(start, mutableEnd, minimum, maximum)) return 0u;

        if (node.leaf()) {
            if (node.faceIndex < 0 ||
                static_cast<std::size_t>(node.faceIndex) >= mesh.faces.size()) {
                return 0u;
            }
            const auto faceIndex = static_cast<std::size_t>(node.faceIndex);
            const auto& face = mesh.faces[faceIndex];
            const auto surfaceBit = face.surfaceMaterial & 31u;
            if ((options.surfaceMask & (1u << surfaceBit)) == 0u) {
                return 0u;
            }
            std::array<Vec3, 3> vertices{};
            for (std::size_t index = 0u; index < vertices.size(); ++index) {
                if (face.indices[index] < 0 ||
                    static_cast<std::size_t>(face.indices[index]) >= mesh.vertices.size()) {
                    return 0u;
                }
                vertices[index] = mesh.vertices[
                    static_cast<std::size_t>(face.indices[index])].position;
            }
            Vec3 intersection;
            if (!polygonHit(face, vertices, start, mutableEnd, intersection)) return 0u;
            mutableEnd = intersection;
            ++acceptedHits;
            AabbHit hit;
            hit.position = intersection;
            hit.normal = face.normal;
            hit.surfaceMaterial = face.surfaceMaterial;
            hit.surfaceMask = 1u << surfaceBit;
            hit.faceIndex = faceIndex;
            hit.treeNodeIndex = treeIndex;
            hit.intersectionCount = acceptedHits;
            result = hit;
            return 1u;
        }

        active[treeIndex] = true;
        const bool splitMatches = (node.splitMask & directionMask) != 0u;
        const bool firstChildFirst = options.testOpposite ? !splitMatches : splitMatches;
        const auto first = firstChildFirst ? node.firstChild : node.secondChild;
        const auto second = firstChildFirst ? node.secondChild : node.firstChild;
        std::size_t count = 0u;
        if (first >= 0) count += traverse(static_cast<std::size_t>(first), depth + 1u);
        if (second >= 0) count += traverse(static_cast<std::size_t>(second), depth + 1u);
        active[treeIndex] = false;
        return count;
    };

    const auto count = traverse(*tree.root, 0u);
    if (count == 0u || !result) return std::nullopt;
    result->intersectionCount = count;
    const auto direction = subtract(end, start);
    const auto offset = subtract(result->position, start);
    const double denominator = static_cast<double>(direction.x) * direction.x +
        static_cast<double>(direction.y) * direction.y +
        static_cast<double>(direction.z) * direction.z;
    result->segmentFraction = denominator > 0.0
        ? static_cast<float>((static_cast<double>(offset.x) * direction.x +
                              static_cast<double>(offset.y) * direction.y +
                              static_cast<double>(offset.z) * direction.z) / denominator)
        : 0.0f;
    return result;
}

NodeEmitterState bindNodeEmitter(const Node& node) noexcept {
    NodeEmitterState result;
    if ((node.flags & kNodeEmitter)!=0u)
        applyEmitterControllers(node,0.0f,MaterialValueSource::ModelController,result);
    return result;
}

NodeLightState bindNodeLight(const Node& node) noexcept {
    NodeLightState result;
    if((node.flags&kNodeLight)!=0u)
        applyLightControllers(node,0.0f,MaterialValueSource::ModelController,result);
    return result;
}

NodeMaterialState bindNodeMaterial(const Node& node) noexcept {
    NodeMaterialState result;
    if ((node.flags & kNodeMesh) != 0u)
        applyMaterialControllers(node, 0.0f, MaterialValueSource::ModelController, result);
    return result;
}

const char* materialValueSourceName(MaterialValueSource source) noexcept {
    switch (source) {
    case MaterialValueSource::ModelController: return "base-model controller";
    case MaterialValueSource::AnimationController: return "animation controller";
    default: return "default";
    }
}

Pose bindPose(const Model& model) {
    Pose pose;
    pose.positions.reserve(model.nodes.size());
    pose.orientations.reserve(model.nodes.size());
    pose.scales.assign(model.nodes.size(), 1.0f);
    pose.nodeMaterials.reserve(model.nodes.size());
    pose.nodeLights.reserve(model.nodes.size());
    pose.nodeEmitters.reserve(model.nodes.size());
    for (const auto& node : model.nodes) {
        pose.positions.push_back(node.position);
        pose.orientations.push_back(node.orientation);
        pose.nodeMaterials.push_back(bindNodeMaterial(node));
        pose.nodeLights.push_back(bindNodeLight(node));
        pose.nodeEmitters.push_back(bindNodeEmitter(node));
    }
    calculatePoseTransformsAndBounds(model, pose);
    return pose;
}

namespace {

constexpr float kDanglyMinimumElapsed = 0.0125f;
constexpr float kDanglyMaximumElapsed = 0.035f;
constexpr float kDanglyTeleportDistance = 5.0f;
constexpr float kDanglySpringScale = 0.5f;
constexpr float kDanglyDampingScale = 1.5f;
constexpr float kDanglyWindScale = 20.0f;
constexpr float kDanglyConstraintScale = -255.0f;
constexpr float kDanglyCoordinateLimit = 10000000000.0f;

Vec3 scaledVector(const Vec3& value, float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

float vectorLength(const Vec3& value) noexcept {
    const double squared = static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y +
        static_cast<double>(value.z) * value.z;
    return squared > 0.0 && std::isfinite(squared)
        ? static_cast<float>(std::sqrt(squared)) : 0.0f;
}

Quaternion rawQuaternionProduct(const Quaternion& left,
                                const Quaternion& right) noexcept {
    return {
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
}

Quaternion conjugatedQuaternion(const Quaternion& value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

// PartDanglyMesh receives normalized runtime orientations. Avoiding another
// normalization here preserves the direct matrix expansion used by both
// supplied macOS implementations.
Vec3 rotateUnitQuaternion(const Quaternion& orientation,
                          const Vec3& value) noexcept {
    const Vec3 axis{orientation.x, orientation.y, orientation.z};
    const auto twiceCross = scaledVector(cross(axis, value), 2.0f);
    return add(value, add(scaledVector(twiceCross, orientation.w),
                          cross(axis, twiceCross)));
}

Vec3 inverseRotateUnitQuaternion(const Quaternion& orientation,
                                 const Vec3& value) noexcept {
    return rotateUnitQuaternion(conjugatedQuaternion(orientation), value);
}

float dotVector(const Vec3& left, const Vec3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Quaternion quaternionFromRigidMatrix(const Mat4& matrix) noexcept {
    Vec3 x{matrix.values[0], matrix.values[1], matrix.values[2]};
    Vec3 y{matrix.values[4], matrix.values[5], matrix.values[6]};
    const Vec3 sourceZ{matrix.values[8], matrix.values[9], matrix.values[10]};
    x = normalize(x);
    y = subtract(y, scaledVector(x, dotVector(x, y)));
    if (vectorLength(y) <= 1.0e-8f) {
        y = cross(sourceZ, x);
        if (vectorLength(y) <= 1.0e-8f) {
            const Vec3 fallback = std::fabs(x.z) < 0.9f
                ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
            y = cross(fallback, x);
        }
    }
    y = normalize(y);
    Vec3 z = normalize(cross(x, y));
    if (dotVector(z, sourceZ) < 0.0f) {
        y = scaledVector(y, -1.0f);
        z = scaledVector(z, -1.0f);
    }

    const float m00 = x.x;
    const float m01 = y.x;
    const float m02 = z.x;
    const float m10 = x.y;
    const float m11 = y.y;
    const float m12 = z.y;
    const float m20 = x.z;
    const float m21 = y.z;
    const float m22 = z.z;
    const float trace = m00 + m11 + m22;
    Quaternion result;
    if (trace > 0.0f) {
        const float value = std::sqrt(std::max(0.0f, trace + 1.0f)) * 2.0f;
        if (!(value > 1.0e-8f)) return {};
        result.w = 0.25f * value;
        result.x = (m21 - m12) / value;
        result.y = (m02 - m20) / value;
        result.z = (m10 - m01) / value;
    } else if (m00 > m11 && m00 > m22) {
        const float value = std::sqrt(std::max(0.0f, 1.0f + m00 - m11 - m22)) * 2.0f;
        if (!(value > 1.0e-8f)) return {};
        result.w = (m21 - m12) / value;
        result.x = 0.25f * value;
        result.y = (m01 + m10) / value;
        result.z = (m02 + m20) / value;
    } else if (m11 > m22) {
        const float value = std::sqrt(std::max(0.0f, 1.0f + m11 - m00 - m22)) * 2.0f;
        if (!(value > 1.0e-8f)) return {};
        result.w = (m02 - m20) / value;
        result.x = (m01 + m10) / value;
        result.y = 0.25f * value;
        result.z = (m12 + m21) / value;
    } else {
        const float value = std::sqrt(std::max(0.0f, 1.0f + m22 - m00 - m11)) * 2.0f;
        if (!(value > 1.0e-8f)) return {};
        result.w = (m10 - m01) / value;
        result.x = (m02 + m20) / value;
        result.y = (m12 + m21) / value;
        result.z = 0.25f * value;
    }
    return normalizedQuaternion(result);
}

std::vector<Quaternion> danglyWorldOrientations(const Model& model,
                                                 const Pose& pose) {
    std::vector<Quaternion> result(model.nodes.size());
    for (std::size_t index = 0u; index < model.nodes.size(); ++index) {
        const auto local = index < pose.orientations.size()
            ? normalizedQuaternion(pose.orientations[index])
            : normalizedQuaternion(model.nodes[index].orientation);
        const auto parent = model.nodes[index].parent;
        result[index] = parent && *parent < index && *parent < result.size()
            ? multiplyQuaternion(result[*parent], local) : local;
    }
    return result;
}

void initializeDanglyMeshState(const Mesh& mesh, DanglyMeshState& state) {
    state.initialized = true;
    state.previousPartPosition = {};
    state.previousPartOrientation = {};
    state.localPositions.clear();
    state.localPositions.reserve(mesh.vertices.size());
    for (const auto& vertex : mesh.vertices)
        state.localPositions.push_back(vertex.position);
    state.localVelocities.assign(mesh.vertices.size(), Vec3{});
}

void updateDanglyPoseBounds(const Model& model, Pose& pose) {
    Bounds bounds;
    const auto extend = [&bounds](const Vec3& point) {
        if (!bounds.valid) {
            bounds.minimum = bounds.maximum = point;
            bounds.valid = true;
            return;
        }
        bounds.minimum.x = std::min(bounds.minimum.x, point.x);
        bounds.minimum.y = std::min(bounds.minimum.y, point.y);
        bounds.minimum.z = std::min(bounds.minimum.z, point.z);
        bounds.maximum.x = std::max(bounds.maximum.x, point.x);
        bounds.maximum.y = std::max(bounds.maximum.y, point.y);
        bounds.maximum.z = std::max(bounds.maximum.z, point.z);
    };
    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        const auto& deformed = pose.deformedMeshes[meshIndex];
        if (deformed.valid && deformed.positions.size() == mesh.vertices.size()) {
            for (const auto& point : deformed.positions) extend(point);
        } else if (mesh.nodeIndex < pose.worldTransforms.size()) {
            for (const auto& vertex : mesh.vertices)
                extend(transformPoint(pose.worldTransforms[mesh.nodeIndex], vertex.position));
        }
    }
    pose.bounds = bounds;
}

} // namespace

void resetDanglySimulation(const Model& model, const Pose&,
                           DanglySimulationState& state) {
    state.meshes.clear();
    state.meshes.resize(model.meshes.size());
    if (model.game != GameVersion::Kotor2) return;
    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        if (!mesh.dangly || mesh.skinned) continue;
        initializeDanglyMeshState(mesh, state.meshes[meshIndex]);
    }
}

DanglySimulationState makeDanglySimulationState(const Model& model, const Pose& pose) {
    DanglySimulationState result;
    resetDanglySimulation(model, pose, result);
    return result;
}

void simulateDanglyMeshes(const Model& model, Pose& pose,
                          DanglySimulationState& state, float elapsedSeconds,
                          const DanglyEnvironment& environment) {
    if (model.game != GameVersion::Kotor2) return;
    if (state.meshes.size() != model.meshes.size())
        resetDanglySimulation(model, pose, state);
    if (pose.deformedMeshes.size() != model.meshes.size())
        pose.deformedMeshes.resize(model.meshes.size());

    if (!finite(elapsedSeconds)) elapsedSeconds = kDanglyMinimumElapsed;
    const float elapsed = std::clamp(elapsedSeconds,
                                     kDanglyMinimumElapsed,
                                     kDanglyMaximumElapsed);
    const Mat4 objectWorld = environment.objectWorld.value_or(identityMatrix());
    const auto objectOrientation = quaternionFromRigidMatrix(objectWorld);
    const auto worldOrientations = danglyWorldOrientations(model, pose);

    for (std::size_t meshIndex = 0u; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        if (!mesh.dangly || mesh.skinned ||
            mesh.nodeIndex >= pose.worldTransforms.size() ||
            mesh.nodeIndex >= worldOrientations.size()) {
            continue;
        }
        auto& meshState = state.meshes[meshIndex];
        if (!meshState.initialized ||
            meshState.localPositions.size() != mesh.vertices.size() ||
            meshState.localVelocities.size() != mesh.vertices.size()) {
            initializeDanglyMeshState(mesh, meshState);
        }

        const Mat4 partWorld = multiply(objectWorld, pose.worldTransforms[mesh.nodeIndex]);
        const Vec3 currentPosition{partWorld.values[12], partWorld.values[13],
                                   partWorld.values[14]};
        const Quaternion currentOrientation = normalizedQuaternion(
            rawQuaternionProduct(objectOrientation,
                                 worldOrientations[mesh.nodeIndex]));
        const auto movement = subtract(currentPosition, meshState.previousPartPosition);
        const bool discontinuity = vectorLength(movement) > kDanglyTeleportDistance;

        if (discontinuity) {
            // PartDanglyMesh resets the streamed positions but deliberately
            // leaves VertexPrimitiveDangly's velocity array untouched.
            for (std::size_t index = 0u; index < mesh.vertices.size(); ++index)
                meshState.localPositions[index] = mesh.vertices[index].position;
        } else {
            // The original runtime uses a zero vector part as its uninitialized
            // sentinel. Consequently an identity previous orientation refreshes
            // the previous transform on every call; preserve that exact quirk.
            if (meshState.previousPartOrientation.x == 0.0f &&
                meshState.previousPartOrientation.y == 0.0f &&
                meshState.previousPartOrientation.z == 0.0f) {
                meshState.previousPartOrientation = currentOrientation;
                meshState.previousPartPosition = currentPosition;
            }

            const Quaternion oldToCurrent = rawQuaternionProduct(
                conjugatedQuaternion(currentOrientation),
                meshState.previousPartOrientation);
            const Vec3 translationToCurrent = inverseRotateUnitQuaternion(
                currentOrientation,
                subtract(meshState.previousPartPosition, currentPosition));

            Vec3 sceneWind{};
            if (environment.windDisplacement) {
                sceneWind = environment.windDisplacement(currentPosition, elapsed);
                if (!finite(sceneWind.x) || !finite(sceneWind.y) || !finite(sceneWind.z))
                    throw ModelError("Dangly scene returned non-finite wind displacement");
            }
            const Vec3 localWind = scaledVector(
                inverseRotateUnitQuaternion(currentOrientation, sceneWind),
                kDanglyWindScale);
            const auto& data = *mesh.dangly;
            const float springBase = data.tightness * kDanglySpringScale * elapsed;
            const float damping = data.period * kDanglyDampingScale * elapsed;

            for (std::size_t index = 0u; index < mesh.vertices.size(); ++index) {
                const float constraint = index < data.constraints.size()
                    ? data.constraints[index] : 0.0f;
                if (constraint == 0.0f) continue;

                const Vec3 authored = mesh.vertices[index].position;
                Vec3 vertex = add(
                    rotateUnitQuaternion(oldToCurrent,
                                         meshState.localPositions[index]),
                    translationToCurrent);
                const Vec3 transformedVelocity = rotateUnitQuaternion(
                    oldToCurrent, meshState.localVelocities[index]);
                const float spring = constraint * springBase;
                Vec3 velocity = add(
                    transformedVelocity,
                    subtract(scaledVector(subtract(authored, vertex), spring),
                             scaledVector(transformedVelocity, damping)));
                meshState.localVelocities[index] = velocity;

                Vec3 candidate = add(vertex, scaledVector(velocity, elapsed));
                const float maximum =
                    (constraint / kDanglyConstraintScale + 1.0f) * data.displacement;
                candidate = add(candidate, scaledVector(localWind, maximum));

                const Vec3 delta = subtract(candidate, authored);
                if (!finite(delta.x) || !finite(delta.y) || !finite(delta.z) ||
                    delta.x < -kDanglyCoordinateLimit ||
                    delta.y < -kDanglyCoordinateLimit ||
                    delta.z < -kDanglyCoordinateLimit ||
                    delta.x > kDanglyCoordinateLimit ||
                    delta.y > kDanglyCoordinateLimit ||
                    delta.z > kDanglyCoordinateLimit) {
                    candidate = authored;
                } else {
                    if (delta.x > maximum) candidate.x = authored.x + maximum;
                    else if (delta.x < -maximum) candidate.x = authored.x - maximum;
                    if (delta.y > maximum) candidate.y = authored.y + maximum;
                    else if (delta.y < -maximum) candidate.y = authored.y - maximum;
                    if (delta.z > maximum) candidate.z = authored.z + maximum;
                    else if (delta.z < -maximum) candidate.z = authored.z - maximum;
                }
                meshState.localPositions[index] = candidate;
            }
        }

        meshState.previousPartPosition = currentPosition;
        meshState.previousPartOrientation = currentOrientation;

        auto& deformed = pose.deformedMeshes[meshIndex];
        deformed.valid = true;
        deformed.preserveTangentBasis = true;
        deformed.positions.resize(mesh.vertices.size());
        deformed.normals.resize(mesh.vertices.size());
        deformed.tangents.resize(mesh.vertices.size());
        deformed.bitangents.resize(mesh.vertices.size());
        const auto& modelTransform = pose.worldTransforms[mesh.nodeIndex];
        for (std::size_t index = 0u; index < mesh.vertices.size(); ++index) {
            const auto& source = mesh.vertices[index];
            deformed.positions[index] = transformPoint(
                modelTransform, meshState.localPositions[index]);
            // Animate writes only the dynamic position pool. Normals and the
            // authored tangent frame remain static local channels and receive
            // the ordinary part transform at draw time.
            deformed.normals[index] = transformDirection(modelTransform, source.normal);
            if (source.tangentBasis.valid) {
                deformed.tangents[index] = transformDirection(
                    modelTransform, source.tangentBasis.tangent);
                deformed.bitangents[index] = transformDirection(
                    modelTransform, source.tangentBasis.bitangent);
            } else {
                deformed.tangents[index] = {};
                deformed.bitangents[index] = {};
            }
        }
    }

    updateDanglyPoseBounds(model, pose);
}

void simulateDanglyMeshes(const Model& model, Pose& pose,
                          DanglySimulationState& state, float elapsedSeconds,
                          Vec3 windVelocity) {
    DanglyEnvironment environment;
    environment.windDisplacement = [windVelocity](Vec3, float seconds) {
        return scaledVector(windVelocity, seconds);
    };
    simulateDanglyMeshes(model, pose, state, elapsedSeconds, environment);
}

std::optional<AnimationReference> findAnimation(
    const std::vector<const Model*>& hierarchy, const std::string& name) {
    const auto count = contiguousHierarchySize(hierarchy);
    if (count == 0u) return std::nullopt;

    for (std::size_t modelIndex = 0u; modelIndex < count; ++modelIndex) {
        if (const auto animationIndex = animationIndexByName(*hierarchy[modelIndex], name)) {
            return AnimationReference{modelIndex, *animationIndex, modelIndex,
                                      inheritedPositionScale(hierarchy, modelIndex), false};
        }
    }

    // FindAnimation only performs the default fallback after reaching the end
    // of the supermodel chain. An intermediary default is not a fallback.
    const auto terminal = count - 1u;
    if (const auto animationIndex = animationIndexByName(*hierarchy[terminal], "default")) {
        return AnimationReference{terminal, *animationIndex, terminal,
                                  inheritedPositionScale(hierarchy, terminal), true};
    }
    return std::nullopt;
}

std::vector<AnimationReference> enumerateAnimations(
    const std::vector<const Model*>& hierarchy) {
    std::vector<AnimationReference> result;
    std::unordered_set<std::string> seen;
    const auto count = contiguousHierarchySize(hierarchy);
    for (std::size_t modelIndex = 0u; modelIndex < count; ++modelIndex) {
        const auto& model = *hierarchy[modelIndex];
        const auto positionScale = inheritedPositionScale(hierarchy, modelIndex);
        for (std::size_t animationIndex = 0u;
             animationIndex < model.animations.size(); ++animationIndex) {
            const auto key = lowercase(model.animations[animationIndex].name);
            if (!seen.emplace(key).second) continue;
            result.push_back(AnimationReference{modelIndex, animationIndex, modelIndex,
                                                positionScale, false});
        }
    }
    return result;
}

std::vector<AnimationEventOccurrence> animationEventsBetween(
    const Animation& animation, float startTime, float endTime, std::uint32_t flags) {
    std::vector<AnimationEventOccurrence> result;
    constexpr float minimumEventTime = -1000000000.0f;
    constexpr float maximumEventTime = 1000000000.0f;

    if ((flags & kAnimationEventFullRange) != 0u) {
        // This is the literal AnimateEvents behavior: bit 1 replaces the range
        // with finite sentinels and is then cleared. In the observed binaries,
        // combining it with reverse playback produces two empty wrapped ranges.
        startTime = minimumEventTime;
        endTime = maximumEventTime;
        flags &= ~kAnimationEventFullRange;
    }

    if ((flags & kAnimationEventReverse) != 0u) {
        if (endTime > startTime) {
            appendReverseEvents(result, animation, startTime, minimumEventTime);
            appendReverseEvents(result, animation, maximumEventTime, endTime);
        } else {
            appendReverseEvents(result, animation, startTime, endTime);
        }
    } else {
        if (startTime > endTime) {
            appendForwardEvents(result, animation, startTime, maximumEventTime);
            appendForwardEvents(result, animation, minimumEventTime, endTime);
        } else {
            appendForwardEvents(result, animation, startTime, endTime);
        }
    }
    return result;
}

Pose evaluateAnimation(const Model& model, std::size_t animationIndex, float time, bool loop) {
    return evaluateAnimation(model, model, animationIndex, time, 1.0f, loop);
}

Pose evaluateAnimation(const Model& targetModel, const Model& animationModel,
                       std::size_t animationIndex, float time,
                       float positionScale, bool loop) {
    if (animationIndex >= animationModel.animations.size()) {
        throw ModelError("Animation index is outside the parsed model animation array");
    }
    Pose pose = bindPose(targetModel);
    const auto& animation = animationModel.animations[animationIndex];
    if (!finite(time)) time = 0.0f;
    if (!finite(positionScale)) positionScale = 1.0f;
    if (animation.length > 0.0f) {
        if (loop) {
            time = std::fmod(time, animation.length);
            if (time < 0.0f) time += animation.length;
        } else {
            time = std::clamp(time, 0.0f, animation.length);
        }
    } else {
        time = 0.0f;
    }
    pose.time = time;

    std::optional<std::size_t> sourceRoot;
    const auto controlled = controlledAnimationNodes(animation, sourceRoot);
    const auto targetNodes = synchronizedAnimationNodes(targetModel, animation);
    if (sourceRoot && *sourceRoot < targetNodes.size())
        pose.controlledRootNode = targetNodes[*sourceRoot];

    for (std::size_t sourceIndex = 0u; sourceIndex < animation.nodes.size(); ++sourceIndex) {
        if (sourceIndex >= controlled.size() || !controlled[sourceIndex] ||
            sourceIndex >= targetNodes.size() || !targetNodes[sourceIndex]) continue;
        const auto target = *targetNodes[sourceIndex];
        if (target >= targetModel.nodes.size()) continue;
        const auto& animationNode = animation.nodes[sourceIndex];
        if ((targetModel.nodes[target].flags & kNodeEmitter)!=0u)
            applyEmitterControllers(animationNode,time,MaterialValueSource::AnimationController,
                                    pose.nodeEmitters[target]);
        if((targetModel.nodes[target].flags & kNodeLight)!=0u)
            applyLightControllers(animationNode,time,MaterialValueSource::AnimationController,
                                  pose.nodeLights[target]);
        if ((targetModel.nodes[target].flags & kNodeMesh) != 0u) {
            // Apply only to the synchronized, controlled target. Do not scale
            // material values with inherited animationScale and do not inherit
            // alpha down the transform hierarchy. Missing tracks keep the
            // child's base-model value, not the donor model's default.
            applyMaterialControllers(animationNode, time,
                MaterialValueSource::AnimationController, pose.nodeMaterials[target]);
        }

        for (const auto& controller : animationNode.controllers) {
            switch (controller.type) {
            case kControllerPosition:
                if (const auto sampled = controllerVector(animationNode, controller, time)) {
                    auto value = *sampled;
                    value.x *= positionScale;
                    value.y *= positionScale;
                    value.z *= positionScale;
                    pose.positions[target] = value;
                }
                break;
            case kControllerOrientation:
                if (const auto value = controllerQuaternion(animationNode, controller, time))
                    pose.orientations[target] = *value;
                break;
            case kControllerScale:
                if (const auto value = controllerFloat(animationNode, controller, time))
                    pose.scales[target] = *value;
                break;
            default:
                break;
            }
        }
    }
    calculatePoseTransformsAndBounds(targetModel, pose);
    return pose;
}

std::string gameVersionName(GameVersion game) {
    return game == GameVersion::Kotor1 ? "Knights of the Old Republic" : "The Sith Lords";
}

std::string nodeFlagsText(std::uint16_t flags) {
    struct FlagName { std::uint16_t value; const char* name; };
    static constexpr FlagName names[] = {
        {kNodeBase, "base"}, {kNodeLight, "light"}, {kNodeEmitter, "emitter"},
        {kNodeCamera, "camera"}, {kNodeReference, "reference"}, {kNodeMesh, "mesh"},
        {kNodeSkin, "skin"}, {kNodeAnimation, "animation"}, {kNodeDangly, "dangly"},
        {kNodeAabb, "AABB"}, {kNodeTrigger, "trigger"}, {kNodeLightsaber, "lightsaber"},
    };
    std::ostringstream text;
    bool first = true;
    for (const auto& entry : names) {
        if ((flags & entry.value) == 0u) continue;
        if (!first) text << ", ";
        text << entry.name;
        first = false;
    }
    if (first) text << "none";
    return text.str();
}

std::string controllerTypeName(std::uint32_t type) {
    switch (type) {
    case kControllerPosition: return "position";
    case kControllerOrientation: return "orientation";
    case kControllerScale: return "scale";
    default: return "controller_" + std::to_string(type);
    }
}

std::string modelSummary(const Model& model) {
    std::size_t vertices = 0u;
    std::size_t triangles = 0u;
    std::size_t textured = 0u;
    std::size_t skinned = 0u;
    std::size_t deformableSkins = 0u;
    std::size_t dangly = 0u;
    std::size_t lights = 0u;
    std::size_t emitters = 0u;
    std::size_t cameras = 0u;
    std::size_t triggers = 0u;
    std::size_t references = 0u;
    std::size_t lightsabers = 0u;
    std::size_t aabbs = 0u;
    for (const auto& mesh : model.meshes) {
        vertices += mesh.vertices.size();
        triangles += mesh.indices.size() / 3u;
        if (!mesh.texture0.empty()) ++textured;
        if (mesh.skinned) {
            ++skinned;
            if (model.game == GameVersion::Kotor2 && mesh.hasSkinChannels &&
                mesh.hasSkinBoneMap && mesh.hasInverseBindPose) {
                ++deformableSkins;
            }
        }
        if (mesh.dangly) ++dangly;
    }
    for (const auto& node : model.nodes) {
        if (node.light) ++lights;
        if (node.emitter) ++emitters;
        if (node.camera) ++cameras;
        if (node.trigger) ++triggers;
        if (node.reference) ++references;
        if (node.lightsaber) ++lightsabers;
        if (node.aabb) ++aabbs;
    }
    std::ostringstream text;
    text << (model.name.empty() ? "Unnamed model" : model.name) << " — "
         << gameVersionName(model.game) << ", " << model.nodes.size() << " node(s), "
         << model.meshes.size() << " mesh(es), " << vertices << " vertices, "
         << triangles << " triangles, " << textured << " textured mesh(es), "
         << model.animations.size() << " animation(s)";
    if (skinned != 0u) {
        text << ", " << skinned << " skin mesh(es)";
        if (deformableSkins != 0u)
            text << " (" << deformableSkins << " K2 CPU-deformable)";
    }
    if (dangly != 0u) text << ", " << dangly << " dangly mesh(es)";
    if (lights != 0u) text << ", " << lights << " light(s)";
    if (emitters != 0u) text << ", " << emitters << " emitter(s)";
    if (cameras != 0u) text << ", " << cameras << " camera(s)";
    if (triggers != 0u) text << ", " << triggers << " trigger marker(s)";
    if (references != 0u) text << ", " << references << " reference(s)";
    if (lightsabers != 0u) text << ", " << lightsabers << " lightsaber node(s)";
    if (aabbs != 0u) text << ", " << aabbs << " AABB node(s)";
    return text.str();
}

} // namespace neoshared::model
