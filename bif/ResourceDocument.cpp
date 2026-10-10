#include <neoshared/bif/ResourceDocument.hpp>
#include <neoshared/PathUtf8.hpp>
#include <sstream>
#include <stdexcept>
namespace neoshared::bif {
neoshared::ResourceDocument readResourceDocument(const KeyBifArchive& key,
    const LooseArchiveCatalog& loose, const ResourceSelection& selected, const JobControl& job) {
    job.check();
    neoshared::ResourceDocument result;
    if(!key.isOpen()) throw std::runtime_error("Open a KEY/game session before opening a resource.");
    std::filesystem::path source;
    std::uint32_t id{};
    std::size_t memberIndex{};
    std::size_t byteCount{};
    const LooseArchiveInfo* overrideRoot=nullptr;
    bool ready=false;
    result.protectedInputs=key.inputPaths();
    for(const auto& archive:loose.archives()) {
        job.check();
        if(!archive.resolvedPath.empty())result.protectedInputs.push_back(archive.resolvedPath);
    }
    if(selected.source==ResourceSource::KeyBif) {
        if(selected.resourceIndex>=key.resources().size())throw std::out_of_range("Invalid BIF resource selection.");
        const auto& item=key.resources()[selected.resourceIndex];
        if(item.bifIndex>=key.bifs().size())throw std::out_of_range("Invalid BIF owner.");
        const auto& owner=key.bifs()[item.bifIndex];
        source=owner.browserBacked?std::filesystem::path(owner.browserRelativePath):owner.resolvedPath;
        result.fileName=item.fileName();result.type=item.type;id=item.resourceId;ready=item.extractable;memberIndex=item.tableIndex;
        byteCount=item.size;
    } else {
        if(selected.resourceIndex>=loose.resources().size())throw std::out_of_range("Invalid archive resource selection.");
        const auto& item=loose.resources()[selected.resourceIndex];
        if(item.archiveIndex>=loose.archives().size())throw std::out_of_range("Invalid archive owner.");
        const auto& owner=loose.archives()[item.archiveIndex];
        source=owner.browserBacked?std::filesystem::path(owner.browserRelativePath):owner.resolvedPath;
        result.fileName=item.fileName();result.type=item.type;id=item.resourceId;ready=item.extractable;memberIndex=item.archiveResourceIndex;
        byteCount=item.size;
        if(item.overrideFile) {
            source=item.directPath;overrideRoot=&owner;
            result.protectedInputs.push_back(item.directPath);
            if(!owner.overrideIniPath.empty())result.protectedInputs.push_back(owner.overrideIniPath);
        }
    }
    if(!ready)throw std::runtime_error("The selected resource is unavailable or inconsistent. Rescan before opening it.");
    // Length-prefix components: separators inside a path cannot collide with
    // another tuple. Include the archive, not just ResRef or global row index.
    const auto component=[](const std::string& value){return std::to_string(value.size())+":"+value;};
    const auto normalized=[](const std::filesystem::path& p){
        std::error_code ec;auto result=std::filesystem::weakly_canonical(p,ec);
        return neoshared::genericPathToUtf8(ec?p.lexically_normal():result);
    };
    result.identity="neobif:"+component(normalized(key.keyPath()))+
        component(normalized(source))+component(std::to_string(int(selected.source)))+
        component(std::to_string(memberIndex))+component(std::to_string(id))+component(std::to_string(result.type))+
        component(result.fileName);
    result.sourceDescription=neoshared::genericPathToUtf8(source)+" :: "+result.fileName+" (archive snapshot; Save As creates a separate file)";
    if(overrideRoot) {
        result.identity+=component(normalized(overrideRoot->resolvedPath))+
            component(overrideRoot->overrideAlias)+component(std::to_string(overrideRoot->overridePrecedence))+
            component(overrideRoot->overrideIniPath.empty()?std::string{}:normalized(overrideRoot->overrideIniPath));
        result.sourceDescription=neoshared::genericPathToUtf8(source)+" ("+overrideRoot->overrideAlias+
            ", precedence "+std::to_string(overrideRoot->overridePrecedence)+", root "+
            neoshared::genericPathToUtf8(overrideRoot->resolvedPath)+")";
        if(!overrideRoot->overrideIniPath.empty())
            result.sourceDescription+=" ["+neoshared::genericPathToUtf8(overrideRoot->overrideIniPath)+"]";
    }
    std::string error;
    job.update(0,byteCount,"Reading "+result.fileName);
    const ByteSink sink=[&](const std::uint8_t* bytes,std::size_t size,std::string&) {
        result.bytes.insert(result.bytes.end(),bytes,bytes+size);
        job.update(result.bytes.size(),byteCount,"Reading "+result.fileName);
        return true;
    };
    // Loose archives retain their canonical reader's input-revision checks;
    // its operation scope supplies cancellation during reads and decoding.
    const bool ok=selected.source==ResourceSource::KeyBif?
        key.streamResource(selected.resourceIndex,sink,error,job):
        loose.readResource(selected.resourceIndex,result.bytes,error,job);
    if(!ok)throw std::runtime_error(error.empty()?"Unable to read the selected archive resource.":error);
    job.update(result.bytes.size(),byteCount,"Reading "+result.fileName);
    return result;
}

std::optional<neoshared::ResourceDocument> resolveResourceDocument(
    const KeyBifArchive& key, const LooseArchiveCatalog& loose,
    const std::string& resref, std::uint16_t type, const JobControl& job) {
    job.check();
    std::uint16_t sessionType=type;
    if(key.gameProfile()==GameProfile::JadeEmpire) {
        const auto extension=resourceTypeExtension(type,GameProfile::KotOR);
        if(const auto translated=resourceTypeFromExtension(extension,key.gameProfile()))
            sessionType=*translated;
    }
    const auto selected=findResource(key,loose,resref,sessionType);
    job.check();
    if(!selected)return std::nullopt;
    return readResourceDocument(key,loose,*selected,job);
}

neoshared::ResourceResolver captureResourceResolver(
    const KeyBifArchive& key, const LooseArchiveCatalog& loose) {
    struct Snapshot {
        KeyBifArchive key;
        LooseArchiveCatalog loose;
    };
    auto snapshot=std::make_shared<const Snapshot>(Snapshot{key,loose});
    return [snapshot=std::move(snapshot)](const std::string& resref,
                                        std::uint16_t type,const JobControl& job) {
        return resolveResourceDocument(snapshot->key,snapshot->loose,resref,type,job);
    };
}
}
