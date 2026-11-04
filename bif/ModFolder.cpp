#include <neoshared/bif/ModFolder.hpp>
#include <neoshared/PathUtf8.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <set>
#include <stdexcept>

namespace neoshared::bif {
namespace {
std::atomic<std::uint64_t> nextSnapshot{0};
std::string lower(std::string value) {for(auto& c:value)if(c>='A'&&c<='Z')c=char(c+32);return value;}
constexpr std::array<std::uint16_t,2> sounds{{4,8}};
bool texture(std::uint16_t type) {return isTextureType(type);}
bool sound(std::uint16_t type) {return std::find(sounds.begin(),sounds.end(),type)!=sounds.end();}
}

std::string ModFolder::moduleName(const std::filesystem::path& path) {
    auto name=lower(pathToUtf8(path.stem()));const auto ext=lower(pathToUtf8(path.extension()));
    if(name=="global"||name=="localization")return {};
    if(ext==".rim") {
        for(const auto* suffix:{"_adx","_a","_s"}) {const auto n=std::char_traits<char>::length(suffix);if(name.size()>n&&name.substr(name.size()-n)==suffix) {name.resize(name.size()-n);break;}}
        return name;
    }
    if(ext==".mod"||ext==".mzd") {if(name.size()>4&&name.substr(name.size()-4)=="_loc")name.resize(name.size()-4);return name;}
    if(ext==".erf"&&name.size()>4&&name.substr(name.size()-4)=="_dlg")return name.substr(0,name.size()-4);
    return {};
}

ModFolder::ModFolder(const std::filesystem::path& root,const JobControl& job):root_(std::filesystem::absolute(root).lexically_normal()) {
    identity_="mod:"+std::to_string(++nextSnapshot)+":"+genericPathToUtf8(root_);
    if(!loose_.openFiles(root_,{},job)||!loose_.addOverrideDirectories({{"MOD FOLDER",root_,{},0,false}},{{"mp3",8}},job,true))throw std::runtime_error(loose_.lastError());
    std::vector<std::filesystem::path> paths;
    for(const auto& item:loose_.resources()) {
        job.check();files_.emplace(Key{lower(item.resref),item.type},item.index);
        paths_.emplace(lower(genericPathToUtf8(item.overrideRelativePath)),item.index);
        if(LooseArchiveCatalog::hasSupportedExtension(item.directPath))paths.push_back(item.directPath);
    }
    if(!packed_.openFiles(root_,paths,job))throw std::runtime_error(packed_.lastError());
    std::map<std::string,std::map<std::string,const LooseArchiveInfo*>> groups;
    for(const auto& archive:packed_.archives()) {
        if(!archive.valid)throw std::runtime_error("Cannot index mod archive: "+genericPathToUtf8(archive.resolvedPath));
        job.check();auto leaf=archive.resolvedPath.filename();const bool compressed=lower(pathToUtf8(leaf.extension()))==".mzd";
        if(compressed)leaf.replace_extension(".mod");
        const auto name=lower(pathToUtf8(leaf));
        if(compressed)archives_[name]=archive.resolvedPath;else archives_.emplace(name,archive.resolvedPath);const auto module=moduleName(archive.resolvedPath);
        if(!module.empty()) {if(compressed)groups[module][name]=&archive;else groups[module].emplace(name,&archive);}
        else {
            Index index;for(const auto n:archive.resourceIndices) {const auto& r=packed_.resources().at(n);index.emplace(Key{lower(r.resref),r.type},n);}
            globals_.push_back(std::move(index));
        }
    }
    for(const auto& [module,archives]:groups)for(const auto& suffix:{".rim","_s.rim","_adx.rim","_a.rim",".mod","_dlg.erf","_loc.mod"})
        if(const auto found=archives.find(module+suffix);found!=archives.end()) {
            if(archives.count(module+".mod")&&(std::string(suffix)==".rim"||std::string(suffix)=="_s.rim"||std::string(suffix)=="_dlg.erf"))continue;
            modules_[module].push_back(found->second->resolvedPath);Index index;
            for(const auto n:found->second->resourceIndices) {const auto& r=packed_.resources().at(n);index.emplace(Key{lower(r.resref),r.type},n);}
            scoped_[module].push_back(std::move(index));
        }
}

bool ModFolder::ownsArchive(const std::filesystem::path& path) const {
    return std::any_of(archives_.begin(),archives_.end(),[&](const auto& entry){return entry.second==path;});
}
std::optional<std::filesystem::path> ModFolder::archive(const std::filesystem::path& name) const {
    const auto found=archives_.find(lower(pathToUtf8(name.filename())));if(found==archives_.end())return {};return found->second;
}
std::optional<ResourceDocument> ModFolder::resolveFile(const std::filesystem::path& relative,const JobControl& job) const {
    const auto found=paths_.find(lower(genericPathToUtf8(relative.lexically_normal())));
    if(found==paths_.end())return {};
    return read(false,found->second,job);
}
std::vector<const ModFolder::Index*> ModFolder::layers(const std::optional<std::string>& module) const {
    std::vector<const Index*> result;
    if(module) {if(const auto found=scoped_.find(lower(*module));found!=scoped_.end())for(const auto& index:found->second)result.push_back(&index);}
    else for(const auto& [name,indices]:scoped_)for(const auto& index:indices)result.push_back(&index);
    for(const auto& index:globals_)result.push_back(&index);
    return result;
}
std::optional<std::pair<bool,std::size_t>> ModFolder::find(const Key& key,const std::optional<std::string>& module) const {
    const auto higherFormat=[&](const Index& index) {
        if(texture(key.second))for(const auto type:textureTypes) {
            if(type==key.second)break;
            if(index.count({key.first,type}))return true;
        }
        return false;
    };
    const auto sameFamily=[&](const Index& index) {
        if(texture(key.second))for(const auto type:textureTypes)if(index.count({key.first,type}))return true;
        if(sound(key.second))for(const auto type:sounds)if(index.count({key.first,type}))return true;
        return false;
    };
    if(higherFormat(files_))return {};
    if(const auto found=files_.find(key);found!=files_.end())return std::pair{false,found->second};
    if(sameFamily(files_))return {};
    for(const auto* index:layers(module)) {
        if(higherFormat(*index))return {};
        if(const auto found=index->find(key);found!=index->end())return std::pair{true,found->second};
        if(sameFamily(*index))return {};
    }
    return {};
}
bool ModFolder::shadows(const std::string& name,std::uint16_t type,const std::optional<std::string>& module) const {
    const auto key=lower(name);if(find({key,type},module))return true;
    if(texture(type))for(const auto format:textureTypes)if(find({key,format},module))return true;
    if(sound(type))for(const auto format:sounds)if(find({key,format},module))return true;
    return false;
}
ResourceDocument ModFolder::read(bool packed,std::size_t index,const JobControl& job) const {
    const auto& catalog=packed?packed_:loose_;const auto& item=catalog.resources().at(index);
    const auto& owner=catalog.archives().at(item.archiveIndex);const auto& path=packed?owner.resolvedPath:item.directPath;
    ResourceDocument result;result.fileName=item.fileName();result.type=item.type;result.sourcePath=path;result.priority=ResourcePriority::Mod;
    result.sourceDescription="Mod folder: "+genericPathToUtf8(path)+(packed?" :: "+item.fileName():std::string{});
    result.identity=identity_+(packed?":archive:":":file:")+std::to_string(index);
    result.protectedInputs={path};std::string error;
    if(!catalog.readResource(index,result.bytes,error,job))throw std::runtime_error(error);
    return result;
}
std::optional<ResourceDocument> ModFolder::resolve(const std::string& name,std::uint16_t type,const std::optional<std::string>& module,const JobControl& job) const {
    job.check();const auto found=find({lower(name),type},module);if(!found)return {};
    auto result=read(found->first,found->second,job);result.module=module.value_or(std::string{});return result;
}
std::optional<ResourceDocument> ModFolder::openResource(const std::string& name,std::uint16_t type,const std::optional<std::string>& module,const JobControl& job) const {
    if(auto result=resolve(name,type,module,job))return result;
    if(texture(type))for(const auto format:textureTypes)if(auto result=resolve(name,format,module,job))return result;
    if(sound(type))for(const auto format:sounds)if(auto result=resolve(name,format,module,job))return result;
    return {};
}
std::vector<const LooseResourceInfo*> ModFolder::resources(const std::optional<std::string>& module) const {
    std::vector<const LooseResourceInfo*> result;std::set<Key> seen;
    for(const auto& [key,index]:files_) {result.push_back(&loose_.resources().at(index));seen.insert(key);}
    for(const auto* index:layers(module))for(const auto& [key,n]:*index)if(seen.insert(key).second)result.push_back(&packed_.resources().at(n));
    return result;
}
ResourceResolver ModFolder::layer(std::shared_ptr<const ModFolder> mod,ResourceResolver lower,std::optional<std::string> module) {
    return [mod=std::move(mod),lower=std::move(lower),module=std::move(module)](const std::string& name,std::uint16_t type,const JobControl& job)->std::optional<ResourceDocument> {
        if(mod&&mod->shadows(name,type,module))return mod->resolve(name,type,module,job);
        return lower?lower(name,type,job):std::nullopt;
    };
}

}
