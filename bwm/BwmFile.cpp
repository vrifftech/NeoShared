#include <neoshared/bwm/BwmFile.hpp>

#include <cstring>
#include <stdexcept>

namespace neoshared::bwm {
namespace {
std::uint32_t u32(const std::vector<std::uint8_t>& data,std::size_t at) {
    return std::uint32_t(data.at(at))|(std::uint32_t(data.at(at+1))<<8u)|
           (std::uint32_t(data.at(at+2))<<16u)|(std::uint32_t(data.at(at+3))<<24u);
}
float f32(const std::vector<std::uint8_t>& data,std::size_t at) {
    const auto bits=u32(data,at);float result;
    static_assert(sizeof(result)==sizeof(bits),"BWM uses IEEE 32-bit floats");
    std::memcpy(&result,&bits,sizeof(result));return result;
}
Vector vector(const std::vector<std::uint8_t>& data,std::size_t at) {
    return {f32(data,at),f32(data,at+4),f32(data,at+8)};
}
}
BwmFile readBwm(const std::vector<std::uint8_t>& data,const std::string& sourceName,
                const bif::JobControl& job) {
    job.check();
    // CResBWM::OnResourceServiced compares all eight magic bytes. Checked
    // byte access only rejects truncated serialized fields; no recovery data
    // or arbitrary count limits are introduced here.
    constexpr char signature[]="BWM V1.0";
    for(std::size_t i=0;i<8;++i)if(data.at(i)!=static_cast<std::uint8_t>(signature[i]))
        throw std::runtime_error("Invalid BWM signature: "+sourceName);
    BwmFile result;result.type=u32(data,8);result.position=vector(data,60);
    const auto vertexCount=u32(data,72),vertexOffset=u32(data,76);
    const auto faceCount=u32(data,80),faceOffset=u32(data,84),materialOffset=u32(data,88);
    const auto normalOffset=u32(data,92),distanceOffset=u32(data,96);
    const auto nodeCount=u32(data,100),nodeOffset=u32(data,104);
    result.root=static_cast<std::int32_t>(u32(data,108));
    for(std::uint32_t i=0;i<vertexCount;++i) {
        job.check();
        result.vertices.push_back(vector(data,std::size_t(vertexOffset)+std::size_t(i)*12u));
    }
    for(std::uint32_t i=0;i<faceCount;++i) {
        job.check();
        const auto at=std::size_t(faceOffset)+std::size_t(i)*12u;
        result.faces.push_back({{u32(data,at),u32(data,at+4),u32(data,at+8)},
                                u32(data,std::size_t(materialOffset)+std::size_t(i)*4u)});
        // All four CSWCollisionMesh::LoadMeshBinary consumers retain these
        // authored plane arrays; ComputeHeight does not rebuild them.
        result.faceNormals.push_back(vector(data,std::size_t(normalOffset)+std::size_t(i)*12u));
        result.faceDistances.push_back(f32(data,std::size_t(distanceOffset)+std::size_t(i)*4u));
    }
    for(std::uint32_t i=0;i<nodeCount;++i) {
        job.check();
        const auto at=std::size_t(nodeOffset)+std::size_t(i)*44u;
        result.nodes.push_back({vector(data,at),vector(data,at+12),
            static_cast<std::int32_t>(u32(data,at+24)),
            static_cast<std::int32_t>(u32(data,at+32)),
            static_cast<std::int32_t>(u32(data,at+36)),
            static_cast<std::int32_t>(u32(data,at+40))});
    }
    return result;
}
} // namespace neoshared::bwm
