#include <neoshared/model/Lighting.hpp>

#include <algorithm>
#include <cmath>

namespace neoshared::model {
namespace {
bool finite(Vec3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
float display(float value, float maximum) { return std::isfinite(value) ? std::clamp(value,0.0f,maximum) : 0.0f; }
double distanceSquared(Vec3 a,Vec3 b) {
    const double x=static_cast<double>(a.x)-b.x,y=static_cast<double>(a.y)-b.y,z=static_cast<double>(a.z)-b.z;
    return x*x+y*y+z*z;
}
}
void appendSceneLights(const Model& model,const Pose& pose,const Mat4& base,
                       std::uint64_t instanceId,std::vector<SceneLight>& output) {
    for(std::size_t index=0;index<model.nodes.size();++index) {
        const auto& node=model.nodes[index];
        if(!node.light || (node.flags&kNodeLight)==0 || index>=pose.worldTransforms.size()) continue;
        const auto state=index<pose.nodeLights.size() ? pose.nodeLights[index] : bindNodeLight(node);
        const auto& data=*node.light;
        SceneLight light;
        light.id=(instanceId<<32) | static_cast<std::uint32_t>(index);
        light.position=transformPoint(multiply(base,pose.worldTransforms[index]),{});
        light.radius=display(state.radius,1.0e6f);
        light.shadowRadius=display(state.shadowRadius,1.0e6f);
        light.verticalDisplacement=std::isfinite(state.verticalDisplacement) ? state.verticalDisplacement : 0.0f;
        const float multiplier=display(state.multiplier,64.0f);
        light.color={display(state.color[0],64.0f)*multiplier,
                     display(state.color[1],64.0f)*multiplier,
                     display(state.color[2],64.0f)*multiplier};
        if(!finite(light.position) || light.radius<=0.0f ||
            (light.color.x+light.color.y+light.color.z)<=0.0f) continue;
        light.priority=data.priority; light.dynamicType=data.dynamicType;
        light.ambientOnly=data.ambientOnly; light.affectDynamic=data.affectDynamic;
        light.castsShadow=data.castsShadow && !data.ambientOnly; light.fading=data.fading;
        output.push_back(light);
    }
}
std::vector<std::size_t> selectSceneLights(const std::vector<SceneLight>& lights,
    Vec3 center,float surfaceRadius,bool dynamicSurface,std::size_t maximum) {
    std::vector<std::size_t> result;
    if(!finite(center) || !std::isfinite(surfaceRadius) || maximum==0) return result;
    surfaceRadius=std::max(0.0f,surfaceRadius);
    for(std::size_t i=0;i<lights.size();++i) {
        const auto& l=lights[i];
        if(!finite(l.position) || !finite(l.color) || !std::isfinite(l.radius) || l.radius<=0 ||
           (dynamicSurface && !l.affectDynamic)) continue;
        const double reach=static_cast<double>(l.radius)+surfaceRadius;
        if(distanceSquared(center,l.position)<=reach*reach) result.push_back(i);
    }
    std::stable_sort(result.begin(),result.end(),[&](std::size_t a,std::size_t b) {
        if(lights[a].priority!=lights[b].priority) return lights[a].priority>lights[b].priority;
        return distanceSquared(center,lights[a].position)<distanceSquared(center,lights[b].position);
    });
    if(result.size()>maximum) result.resize(maximum);
    return result;
}
float lightAttenuation(float distance,float radius,bool fading) noexcept {
    if(!std::isfinite(distance) || !std::isfinite(radius) || radius<=0.0f) return 0.0f;
    const float t=std::clamp(std::max(0.0f,distance)/radius,0.0f,1.0f);
    const float edge=1.0f-t*t;
    float value=edge*edge/(1.0f+4.0f*t*t);
    if(fading) { const float f=std::clamp((1.0f-t)*10.0f,0.0f,1.0f);value*=f*f*(3.0f-2.0f*f); }
    return value;
}
bool validFog(const FogSettings& f) noexcept {
    return static_cast<unsigned>(f.mode)<=static_cast<unsigned>(FogMode::ExponentialSquared) &&
        finite(f.color) && f.color.x>=0 && f.color.y>=0 && f.color.z>=0 &&
        std::isfinite(f.start) && std::isfinite(f.end) && std::isfinite(f.density) &&
        f.start>=0 && f.end>f.start && f.density>=0;
}
FogSettings fogForModel(const FogSettings& scene,bool modelFog) noexcept {
    auto result=scene;
    if(result.respectModelFlag && !modelFog) result.mode=FogMode::Disabled;
    return result;
}
float fogTransmittance(const FogSettings& f,float distance) noexcept {
    if(f.mode==FogMode::Disabled || !validFog(f) || !std::isfinite(distance)) return 1.0f;
    const float d=std::max(0.0f,distance);
    if(f.mode==FogMode::Linear) return std::clamp((f.end-d)/(f.end-f.start),0.0f,1.0f);
    const double x=static_cast<double>(f.density)*d;
    return static_cast<float>(std::exp(f.mode==FogMode::ExponentialSquared ? -x*x : -x));
}
} // namespace neoshared::model
