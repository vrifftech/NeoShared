#pragma once

#include <neoshared/bif/LooseArchiveCatalog.hpp>
#include <neoshared/ResourceResolver.hpp>
#include <map>
#include <optional>

namespace neoshared::bif {

// One indexed, read-only authoring layer. An absent scope searches all mod
// archives; an empty scope searches globals; a named scope adds that module.
class ModFolder final {
public:
    explicit ModFolder(const std::filesystem::path& root,const JobControl& job={});
    const std::filesystem::path& root() const noexcept {return root_;}
    const std::string& identity() const noexcept {return identity_;}
    const std::map<std::string,std::vector<std::filesystem::path>>& modules() const noexcept {return modules_;}
    bool ownsArchive(const std::filesystem::path& path) const;
    std::optional<std::filesystem::path> archive(const std::filesystem::path& name) const;
    std::optional<ResourceDocument> resolveFile(const std::filesystem::path& relative,const JobControl& job={}) const;
    std::vector<const LooseResourceInfo*> resources(const std::optional<std::string>& module={}) const;
    bool shadows(const std::string& name,std::uint16_t type,const std::optional<std::string>& module={}) const;
    std::optional<ResourceDocument> resolve(const std::string& name,std::uint16_t type,
        const std::optional<std::string>& module={},const JobControl& job={}) const;
    std::optional<ResourceDocument> openResource(const std::string& name,std::uint16_t type,
        const std::optional<std::string>& module={},const JobControl& job={}) const;
    static ResourceResolver layer(std::shared_ptr<const ModFolder> mod,ResourceResolver lower,
        std::optional<std::string> module={});
    static std::string moduleName(const std::filesystem::path& archive);
private:
    using Key=std::pair<std::string,std::uint16_t>;
    using Index=std::map<Key,std::size_t>;
    std::vector<const Index*> layers(const std::optional<std::string>& module) const;
    std::optional<std::pair<bool,std::size_t>> find(const Key&,const std::optional<std::string>&) const;
    ResourceDocument read(bool packed,std::size_t index,const JobControl&) const;
    std::filesystem::path root_;
    std::string identity_;
    LooseArchiveCatalog loose_,packed_;
    Index files_;
    std::map<std::string,std::size_t> paths_;
    std::vector<Index> globals_;
    std::map<std::string,std::vector<Index>> scoped_;
    std::map<std::string,std::vector<std::filesystem::path>> modules_;
    std::map<std::string,std::filesystem::path> archives_;
};

}
