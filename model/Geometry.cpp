#include "neoshared/model/Geometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace neoshared::model {
namespace {
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 mul(Vec3 a, double s) {
    return {static_cast<float>(a.x*s), static_cast<float>(a.y*s), static_cast<float>(a.z*s)};
}
double dot(Vec3 a, Vec3 b) {
    return static_cast<double>(a.x)*b.x + static_cast<double>(a.y)*b.y + static_cast<double>(a.z)*b.z;
}
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
Vec3 unit(Vec3 v, Vec3 fallback = {0,0,1}) {
    const auto n = dot(v,v);
    return finite(v) && n > 1.0e-20 && std::isfinite(n) ? mul(v,1.0/std::sqrt(n)) : fallback;
}
Vec3 fallbackTangent(Vec3 n) {
    const Vec3 axis = std::abs(n.x) < 0.8f ? Vec3{1,0,0} : Vec3{0,1,0};
    return unit(sub(axis,mul(n,dot(n,axis))), {1,0,0});
}
Vec2 safeUv(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y) ? v : Vec2{}; }
double determinant(const Mat4& m) {
    const auto& a = m.values;
    return static_cast<double>(a[0])*(static_cast<double>(a[5])*a[10]-static_cast<double>(a[9])*a[6])
         - static_cast<double>(a[4])*(static_cast<double>(a[1])*a[10]-static_cast<double>(a[9])*a[2])
         + static_cast<double>(a[8])*(static_cast<double>(a[1])*a[6]-static_cast<double>(a[5])*a[2]);
}
}

RenderGeometry prepareRenderGeometry(const Mesh& mesh, const DeformedMesh* deformed) {
    if (mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max())
        throw ModelError("Too many vertices for a 32-bit indexed render buffer");
    const bool posed = deformed && deformed->valid &&
        deformed->positions.size() == mesh.vertices.size() &&
        deformed->normals.size() == mesh.vertices.size();
    const bool preservedBasis = posed && deformed->preserveTangentBasis &&
        deformed->tangents.size() == mesh.vertices.size() &&
        deformed->bitangents.size() == mesh.vertices.size();
    RenderGeometry result;
    result.vertices.resize(mesh.vertices.size());
    std::vector<Vec3> tangents(mesh.vertices.size()), bitangents(mesh.vertices.size());
    for (std::size_t i=0; i<mesh.vertices.size(); ++i) {
        const auto& source = mesh.vertices[i];
        auto& v = result.vertices[i];
        v.position = posed ? deformed->positions[i] : source.position;
        if (!finite(v.position)) v.position = {};
        v.normal = unit(posed ? deformed->normals[i] : source.normal);
        v.texcoord0 = safeUv(source.texcoord0);
        v.texcoord1 = safeUv(source.texcoord1);
        for (std::size_t c=0; c<4; ++c) v.color[c] = static_cast<float>(source.color[c])/255.0f;
    }
    result.indices.reserve(mesh.indices.size());
    for (std::size_t i=0; i+2<mesh.indices.size(); i+=3) {
        const auto a=mesh.indices[i], b=mesh.indices[i+1], c=mesh.indices[i+2];
        if (a>=mesh.vertices.size() || b>=mesh.vertices.size() || c>=mesh.vertices.size() ||
            a==b || a==c || b==c) { ++result.skippedTriangles; continue; }
        result.indices.insert(result.indices.end(), {a,b,c});
        const auto& va=result.vertices[a]; const auto& vb=result.vertices[b]; const auto& vc=result.vertices[c];
        const auto e1=sub(vb.position,va.position), e2=sub(vc.position,va.position);
        const double u1=static_cast<double>(vb.texcoord0.x)-va.texcoord0.x;
        const double v1=static_cast<double>(vb.texcoord0.y)-va.texcoord0.y;
        const double u2=static_cast<double>(vc.texcoord0.x)-va.texcoord0.x;
        const double v2=static_cast<double>(vc.texcoord0.y)-va.texcoord0.y;
        const double d=u1*v2-u2*v1;
        if (!std::isfinite(d) || std::abs(d)<1.0e-12) continue;
        const auto t=mul(sub(mul(e1,v2),mul(e2,v1)),1.0/d);
        const auto bt=mul(sub(mul(e2,u1),mul(e1,u2)),1.0/d);
        if (!finite(t) || !finite(bt)) continue;
        for (const auto index : {a,b,c}) {
            tangents[index]=add(tangents[index],t);
            bitangents[index]=add(bitangents[index],bt);
        }
    }
    if (mesh.indices.size()%3u != 0u) ++result.skippedTriangles;
    for (std::size_t i=0; i<result.vertices.size(); ++i) {
        auto& v=result.vertices[i];
        const auto& authored=mesh.vertices[i].tangentBasis;
        const bool usePreserved = preservedBasis && finite(deformed->tangents[i]) &&
            finite(deformed->bitangents[i]);
        bool useAuthored = usePreserved ||
            (!posed && authored.valid && finite(authored.tangent) && finite(authored.bitangent));
        auto t = usePreserved ? deformed->tangents[i]
                              : (useAuthored ? authored.tangent : tangents[i]);
        auto b = usePreserved ? deformed->bitangents[i]
                              : (useAuthored ? authored.bitangent : bitangents[i]);
        t=sub(t,mul(v.normal,dot(v.normal,t)));
        if (!finite(t) || dot(t,t)<=1.0e-20) {
            useAuthored=false;
            t=sub(tangents[i],mul(v.normal,dot(v.normal,tangents[i])));
            b=bitangents[i];
        }
        v.tangent=unit(t,fallbackTangent(v.normal));
        v.tangentHandedness=finite(b) && dot(cross(v.normal,v.tangent),b)<0.0 ? -1.0f : 1.0f;
        if (useAuthored) ++result.authoredTangents; else ++result.generatedTangents;
    }
    return result;
}

bool makeNormalMatrix(const Mat4& m, std::array<float,9>& out) noexcept {
    out={{1,0,0,0,1,0,0,0,1}};
    const auto& a=m.values;
    for (const auto i : {0,1,2,4,5,6,8,9,10}) if (!std::isfinite(a[i])) return false;
    const double d=determinant(m);
    if (!std::isfinite(d) || std::abs(d)<1.0e-20) return false;
    const double x=a[0], y=a[4], z=a[8], p=a[1], q=a[5], r=a[9], s=a[2], t=a[6], u=a[10];
    const double values[9]={(q*u-r*t)/d,(z*t-y*u)/d,(y*r-z*q)/d,
                            (r*s-p*u)/d,(x*u-z*s)/d,(z*p-x*r)/d,
                            (p*t-q*s)/d,(y*s-x*t)/d,(x*q-y*p)/d};
    for (const auto v : values) if (!std::isfinite(v) || std::abs(v)>std::numeric_limits<float>::max()) return false;
    for (std::size_t i=0;i<9;++i) out[i]=static_cast<float>(values[i]);
    return true;
}
float transformHandedness(const Mat4& m) noexcept {
    const auto d=determinant(m); return std::isfinite(d) && d<0.0 ? -1.0f : 1.0f;
}
} // namespace neoshared::model
