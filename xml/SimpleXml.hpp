#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace neoxml {

struct Node {
    std::string name;
    std::map<std::string, std::string> attributes;
    std::string text;
    std::vector<Node> children;

    const Node* firstChild(const std::string& childName) const noexcept;
    std::vector<const Node*> childrenNamed(const std::string& childName) const;
    std::string attribute(const std::string& key, const std::string& fallback = {}) const;
};

class XmlError : public std::runtime_error {
public:
    explicit XmlError(const std::string& message) : std::runtime_error(message) {}
};

Node parse(const std::string& text);
std::string escapeText(const std::string& text);
std::string escapeAttribute(const std::string& text);

} // namespace neoxml
