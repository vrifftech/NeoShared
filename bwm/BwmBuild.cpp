#include <neoshared/bwm/BwmFile.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <unordered_map>

namespace neoshared::bwm {
namespace {

void buildGeometry(BwmFile& mesh,bool tree,const bif::JobControl& job) {
    mesh.nodes.clear();mesh.root=0;
    mesh.faceNormals.clear();mesh.faceDistances.clear();
    std::vector<AabbNode> leaves;
    std::vector<Vector> centers;
    mesh.faceNormals.reserve(mesh.faces.size());mesh.faceDistances.reserve(mesh.faces.size());
    if(tree) {leaves.reserve(mesh.faces.size());centers.reserve(mesh.faces.size());}
    for(std::size_t index=0;index<mesh.faces.size();++index) {
        job.check();
        const auto& face=mesh.faces[index];
        const auto& a=mesh.vertices[face.vertices[0]];
        const auto& b=mesh.vertices[face.vertices[1]];
        const auto& c=mesh.vertices[face.vertices[2]];
        const Vector u{b[0]-a[0],b[1]-a[1],b[2]-a[2]},v{c[0]-a[0],c[1]-a[1],c[2]-a[2]};
        Vector normal{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
        const auto length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
        for(auto& component:normal) component/=length;
        mesh.faceNormals.push_back(normal);
        mesh.faceDistances.push_back(-(normal[0]*a[0]+normal[1]*a[1]+normal[2]*a[2]));
        if(!tree) continue;
        AabbNode leaf;leaf.face=static_cast<std::int32_t>(index);
        leaf.reserved=4;
        Vector center;
        for(std::size_t axis=0;axis<3;++axis) {
            leaf.minimum[axis]=std::min({a[axis],b[axis],c[axis]})-0.01f;
            leaf.maximum[axis]=std::max({a[axis],b[axis],c[axis]})+0.01f;
            center[axis]=(a[axis]+b[axis]+c[axis])/3.0f;
        }
        leaves.push_back(leaf);centers.push_back(center);
    }
    if(!tree || leaves.empty()) return;
    mesh.nodes.reserve(leaves.size()*2-1);
    std::vector<std::size_t> faces(leaves.size());std::iota(faces.begin(),faces.end(),0);
    const std::function<std::int32_t(std::size_t,std::size_t)> branch=[&](std::size_t begin,std::size_t end) {
        job.check();
        const auto index=static_cast<std::int32_t>(mesh.nodes.size());
        auto node=leaves[faces[begin]];
        auto low=centers[faces[begin]],high=low;
        for(auto i=begin+1;i<end;++i) for(std::size_t axis=0;axis<3;++axis) {
            const auto face=faces[i];
            node.minimum[axis]=std::min(node.minimum[axis],leaves[face].minimum[axis]);
            node.maximum[axis]=std::max(node.maximum[axis],leaves[face].maximum[axis]);
            low[axis]=std::min(low[axis],centers[face][axis]);
            high[axis]=std::max(high[axis],centers[face][axis]);
        }
        mesh.nodes.push_back(node);
        if(end-begin==1) return index;
        std::size_t axis=0;
        for(std::size_t i=1;i<3;++i) if(high[i]-low[i]>high[axis]-low[axis]) axis=i;
        const auto middle=begin+(end-begin)/2;
        std::nth_element(faces.begin()+begin,faces.begin()+middle,faces.begin()+end,[&](auto a,auto b) {
            return centers[a][axis]==centers[b][axis]?a<b:centers[a][axis]<centers[b][axis];
        });
        const auto first=branch(begin,middle),second=branch(middle,end);
        auto& parent=mesh.nodes[index];parent.face=-1;parent.splitMask=1u<<axis;
        parent.firstChild=first;parent.secondChild=second;
        return index;
    };
    mesh.root=branch(0,faces.size());
}

} // namespace

void buildAabb(BwmFile& mesh,const bif::JobControl& job) {buildGeometry(mesh,true,job);}

void buildBwm(BwmFile& mesh,const std::set<std::uint32_t>& walkableMaterials,const bif::JobControl& job) {
    if(mesh.type==0) {
        mesh.adjacency.clear();mesh.edges.clear();mesh.perimeters.clear();
        buildGeometry(mesh,false,job);return;
    }
    std::vector<std::size_t> order(mesh.faces.size());std::iota(order.begin(),order.end(),0);
    const auto middle=std::stable_partition(order.begin(),order.end(),[&](auto i) {
        return walkableMaterials.count(mesh.faces[i].material)!=0;
    });
    const auto walkable=static_cast<std::size_t>(middle-order.begin());
    std::vector<Face> sorted;sorted.reserve(order.size());
    std::vector<std::uint32_t> remap(order.size());
    for(auto face:order) {remap[face]=static_cast<std::uint32_t>(sorted.size());sorted.push_back(mesh.faces[face]);}
    mesh.faces=std::move(sorted);
    std::map<std::uint32_t,std::int32_t> transitions;
    for(const auto& edge:mesh.edges) if(edge.transition!=-1)
        transitions[remap[edge.index/3]*3+edge.index%3]=edge.transition;
    mesh.adjacency.assign(walkable,{-1,-1,-1});mesh.edges.clear();mesh.perimeters.clear();
    // Position identities connect split source vertices without rounding or
    // moving the serialized vertex pool.
    std::map<Vector,std::uint32_t> positions;
    std::vector<std::uint32_t> vertexIds;vertexIds.reserve(mesh.vertices.size());
    for(const auto& vertex:mesh.vertices) {
        const auto item=positions.emplace(vertex,static_cast<std::uint32_t>(positions.size()));
        vertexIds.push_back(item.first->second);
    }
    std::unordered_map<std::uint64_t,std::uint32_t> edges;
    edges.reserve(walkable*3);
    for(std::size_t face=0;face<walkable;++face) {
        job.check();
        for(std::uint32_t corner=0;corner<3;++corner) {
            const auto a=vertexIds[mesh.faces[face].vertices[corner]];
            const auto b=vertexIds[mesh.faces[face].vertices[(corner+1)%3]];
            const auto key=(std::uint64_t(std::min(a,b))<<32u)|std::max(a,b);
            const auto index=static_cast<std::uint32_t>(face*3+corner);
            const auto item=edges.emplace(key,index);
            if(!item.second) {
                const auto other=item.first->second;
                mesh.adjacency[face][corner]=static_cast<std::int32_t>(other);
                mesh.adjacency[other/3][other%3]=static_cast<std::int32_t>(index);
                edges.erase(item.first);
            }
        }
    }
    std::vector<bool> visited(walkable*3);
    for(std::uint32_t start=0;start<walkable*3;++start) {
        if(visited[start] || mesh.adjacency[start/3][start%3]!=-1) continue;
        auto edge=start;
        do {
            job.check();
            const auto link=transitions.find(edge);
            mesh.edges.push_back({edge,link==transitions.end()?-1:link->second});visited[edge]=true;
            edge=(edge/3)*3+(edge%3+1)%3;
            while(mesh.adjacency[edge/3][edge%3]!=-1) {
                edge=static_cast<std::uint32_t>(mesh.adjacency[edge/3][edge%3]);
                edge=(edge/3)*3+(edge%3+1)%3;
            }
        } while(edge!=start);
        mesh.perimeters.push_back(static_cast<std::uint32_t>(mesh.edges.size()));
    }
    buildGeometry(mesh,true,job);
}

} // namespace neoshared::bwm
