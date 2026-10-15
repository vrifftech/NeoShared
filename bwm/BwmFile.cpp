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
    for(std::size_t i=0;i<2;++i) {
        result.usePoints[i]=vector(data,12+i*12);
        result.doorUsePoints[i]=vector(data,36+i*12);
    }
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
            static_cast<std::int32_t>(u32(data,at+40)),u32(data,at+28)});
    }
    const auto adjacentCount=u32(data,112),adjacentOffset=u32(data,116);
    for(std::size_t i=0;i<adjacentCount;++i) {
        const auto at=adjacentOffset+i*12;
        result.adjacency.push_back({static_cast<std::int32_t>(u32(data,at)),
            static_cast<std::int32_t>(u32(data,at+4)),static_cast<std::int32_t>(u32(data,at+8))});
    }
    const auto edgeCount=u32(data,120),edgeOffset=u32(data,124);
    for(std::size_t i=0;i<edgeCount;++i)
        result.edges.push_back({u32(data,edgeOffset+i*8),static_cast<std::int32_t>(u32(data,edgeOffset+i*8+4))});
    const auto perimeterCount=u32(data,128),perimeterOffset=u32(data,132);
    for(std::size_t i=0;i<perimeterCount;++i) result.perimeters.push_back(u32(data,perimeterOffset+i*4));
    return result;
}

std::vector<std::uint8_t> writeBwm(const BwmFile& mesh,const bif::JobControl& job) {
    std::vector<std::uint8_t> data(136);
    const auto word=[&](std::size_t at,std::uint32_t value) {
        for(unsigned i=0;i<4;++i) data[at+i]=static_cast<std::uint8_t>(value>>(i*8u));
    };
    const auto append=[&](std::uint32_t value) {const auto at=data.size();data.resize(at+4);word(at,value);};
    const auto scalar=[&](float value) {std::uint32_t bits;std::memcpy(&bits,&value,4);append(bits);};
    const auto vec=[&](const Vector& value) {for(auto component:value) scalar(component);};
    const auto offset=[&](std::size_t at,std::size_t count) {word(at,count?static_cast<std::uint32_t>(data.size()):0u);};
    std::memcpy(data.data(),"BWM V1.0",8);word(8,mesh.type);
    for(std::size_t i=0;i<5;++i) {
        const auto& value=i<2?mesh.usePoints[i]:i<4?mesh.doorUsePoints[i-2]:mesh.position;
        for(std::size_t axis=0;axis<3;++axis) {
            std::uint32_t bits;std::memcpy(&bits,&value[axis],4);word(12+i*12+axis*4,bits);
        }
    }
    word(72,static_cast<std::uint32_t>(mesh.vertices.size()));offset(76,mesh.vertices.size());
    for(const auto& vertex:mesh.vertices) {job.check();vec(vertex);}
    word(80,static_cast<std::uint32_t>(mesh.faces.size()));offset(84,mesh.faces.size());
    for(const auto& face:mesh.faces) for(auto index:face.vertices) append(index);
    offset(88,mesh.faces.size());for(const auto& face:mesh.faces) append(face.material);
    offset(92,mesh.faceNormals.size());for(const auto& normal:mesh.faceNormals) vec(normal);
    offset(96,mesh.faceDistances.size());for(auto distance:mesh.faceDistances) scalar(distance);
    word(100,static_cast<std::uint32_t>(mesh.nodes.size()));offset(104,mesh.nodes.size());word(108,mesh.root);
    for(const auto& node:mesh.nodes) {
        job.check();vec(node.minimum);vec(node.maximum);append(node.face);append(node.reserved);
        append(node.splitMask);append(node.firstChild);append(node.secondChild);
    }
    word(112,static_cast<std::uint32_t>(mesh.adjacency.size()));offset(116,mesh.adjacency.size());
    for(const auto& row:mesh.adjacency) for(auto edge:row) append(edge);
    word(120,static_cast<std::uint32_t>(mesh.edges.size()));offset(124,mesh.edges.size());
    for(const auto& edge:mesh.edges) {append(edge.index);append(edge.transition);}
    word(128,static_cast<std::uint32_t>(mesh.perimeters.size()));offset(132,mesh.perimeters.size());
    for(auto end:mesh.perimeters) append(end);
    return data;
}
} // namespace neoshared::bwm
