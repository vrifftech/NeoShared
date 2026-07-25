#include "SimpleXml.hpp"

#include <cctype>
#include <charconv>
#include <sstream>
#include <string_view>
#include <system_error>

namespace neoxml {
namespace {

bool isNameStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == ':';
}

bool isNameChar(char ch) {
    return isNameStart(ch) || std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '.';
}

void appendUtf8(std::string& out, unsigned value) {
    if (value <= 0x7Fu) {
        out.push_back(static_cast<char>(value));
    } else if (value <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((value >> 6u) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else if (value <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((value >> 12u) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else if (value <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | ((value >> 18u) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((value >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else {
        throw XmlError("XML character reference is outside the Unicode range.");
    }
}

std::string decodeEntities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch != '&') {
            out.push_back(ch);
            continue;
        }
        const std::size_t semi = text.find(';', i + 1);
        if (semi == std::string_view::npos) {
            throw XmlError("Unterminated XML entity reference.");
        }
        const std::string entity(text.substr(i + 1, semi - i - 1));
        if (entity == "amp") out.push_back('&');
        else if (entity == "lt") out.push_back('<');
        else if (entity == "gt") out.push_back('>');
        else if (entity == "quot") out.push_back('"');
        else if (entity == "apos") out.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            unsigned value = 0;
            const char* first = entity.data() + 1;
            const char* last = entity.data() + entity.size();
            int base = 10;
            if (first < last && (*first == 'x' || *first == 'X')) {
                ++first;
                base = 16;
            }
            const auto result = std::from_chars(first, last, value, base);
            if (result.ec != std::errc{} || result.ptr != last) {
                throw XmlError("Malformed XML character reference: &" + entity + ";");
            }
            appendUtf8(out, value);
        } else {
            throw XmlError("Unsupported XML entity reference: &" + entity + ";");
        }
        i = semi;
    }
    return out;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Node parseDocument() {
        skipMisc();
        Node root = parseElement(0);
        skipMisc();
        if (pos_ != text_.size()) {
            throw XmlError("Unexpected trailing data after XML document root.");
        }
        return root;
    }

private:
    bool startsWith(std::string_view prefix) const {
        return pos_ + prefix.size() <= text_.size() && text_.substr(pos_, prefix.size()) == prefix;
    }

    void skipWs() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    void skipUntil(std::string_view terminator, const char* what) {
        const std::size_t end = text_.find(terminator, pos_);
        if (end == std::string_view::npos) {
            throw XmlError(std::string("Unterminated ") + what + ".");
        }
        pos_ = end + terminator.size();
    }

    void skipMisc() {
        while (true) {
            skipWs();
            if (startsWith("<?")) {
                pos_ += 2;
                skipUntil("?>", "XML processing instruction");
            } else if (startsWith("<!--")) {
                pos_ += 4;
                skipUntil("-->", "XML comment");
            } else {
                break;
            }
        }
    }

    char get() {
        if (pos_ >= text_.size()) throw XmlError("Unexpected end of XML document.");
        return text_[pos_++];
    }

    void expect(char ch) {
        if (get() != ch) {
            throw XmlError(std::string("Expected '") + ch + "' in XML document.");
        }
    }

    std::string parseName() {
        if (pos_ >= text_.size() || !isNameStart(text_[pos_])) {
            throw XmlError("Expected XML element or attribute name.");
        }
        const std::size_t begin = pos_++;
        while (pos_ < text_.size() && isNameChar(text_[pos_])) ++pos_;
        return std::string(text_.substr(begin, pos_ - begin));
    }

    std::string parseAttributeValue() {
        skipWs();
        const char quote = get();
        if (quote != '"' && quote != '\'') {
            throw XmlError("Expected quoted XML attribute value.");
        }
        const std::size_t begin = pos_;
        while (pos_ < text_.size() && text_[pos_] != quote) ++pos_;
        if (pos_ >= text_.size()) throw XmlError("Unterminated XML attribute value.");
        const auto value = decodeEntities(text_.substr(begin, pos_ - begin));
        ++pos_;
        return value;
    }

    std::string parseCData() {
        pos_ += 9; // <![CDATA[
        const std::size_t end = text_.find("]]>", pos_);
        if (end == std::string_view::npos) throw XmlError("Unterminated XML CDATA section.");
        std::string out(text_.substr(pos_, end - pos_));
        pos_ = end + 3;
        return out;
    }

    static constexpr std::size_t kMaximumNestingDepth = 256;

    Node parseElement(std::size_t depth) {
        if (depth >= kMaximumNestingDepth) {
            throw XmlError("XML document exceeds the maximum nesting depth of " +
                           std::to_string(kMaximumNestingDepth) + ".");
        }
        expect('<');
        if (pos_ < text_.size() && text_[pos_] == '/') {
            throw XmlError("Unexpected XML closing tag.");
        }
        Node node;
        node.name = parseName();
        while (true) {
            skipWs();
            if (startsWith("/>")) {
                pos_ += 2;
                return node;
            }
            if (startsWith(">")) {
                ++pos_;
                break;
            }
            const std::string key = parseName();
            skipWs();
            expect('=');
            node.attributes[key] = parseAttributeValue();
        }

        while (true) {
            if (pos_ >= text_.size()) throw XmlError("Unexpected end of XML element: " + node.name);
            if (startsWith("</")) {
                pos_ += 2;
                const std::string closing = parseName();
                if (closing != node.name) {
                    throw XmlError("Mismatched XML closing tag: expected </" + node.name + "> but found </" + closing + ">.");
                }
                skipWs();
                expect('>');
                return node;
            }
            if (startsWith("<!--")) {
                pos_ += 4;
                skipUntil("-->", "XML comment");
                continue;
            }
            if (startsWith("<![CDATA[")) {
                node.text += parseCData();
                continue;
            }
            if (startsWith("<?")) {
                pos_ += 2;
                skipUntil("?>", "XML processing instruction");
                continue;
            }
            if (startsWith("<")) {
                node.children.push_back(parseElement(depth + 1));
                continue;
            }
            const std::size_t begin = pos_;
            while (pos_ < text_.size() && text_[pos_] != '<') ++pos_;
            node.text += decodeEntities(text_.substr(begin, pos_ - begin));
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

} // namespace

const Node* Node::firstChild(const std::string& childName) const noexcept {
    for (const auto& child : children) {
        if (child.name == childName) return &child;
    }
    return nullptr;
}

std::vector<const Node*> Node::childrenNamed(const std::string& childName) const {
    std::vector<const Node*> out;
    for (const auto& child : children) {
        if (child.name == childName) out.push_back(&child);
    }
    return out;
}

std::string Node::attribute(const std::string& key, const std::string& fallback) const {
    const auto it = attributes.find(key);
    return it == attributes.end() ? fallback : it->second;
}

Node parse(const std::string& text) {
    return Parser(text).parseDocument();
}

std::string escapeText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '\r': out += " "; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string escapeAttribute(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        case '\r': out += " "; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

} // namespace neoxml
