#include <neoshared/material/Runtime.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace neoshared::material {
namespace {
float finite(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
float bounded(float v,float lo,float hi,float fallback) { return std::clamp(finite(v,fallback),lo,hi); }
double wrap(double v, double length) {
    const auto r=std::fmod(v,length);
    return std::isfinite(r) ? (r<0 ? r+length : r) : 0.0;
}
std::uint32_t hash(std::uint32_t v) { v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16); }
const TxiProperties& props(const TextureBinding& b) {
    static const TxiProperties empty;
    return b.metadata ? b.metadata->txi.properties : empty;
}
}
UvState evaluateUv(const TextureBinding& binding,double seconds,std::array<float,2> meshVelocity) {
    UvState result;
    seconds=std::isfinite(seconds) ? seconds : 0.0;
    const auto& p=props(binding);
    result.clamp=p.clamp.value_or(0)!=0;
    const float velocities[]={finite(meshVelocity[0],0)+finite(p.scrollX.value_or(0),0),
                              finite(meshVelocity[1],0)+finite(p.scrollY.value_or(0),0)};
    for(std::size_t axis=0;axis<2;++axis) {
        const double displacement=seconds*static_cast<double>(velocities[axis]);
        // Clamp needs the signed displacement, not a modulo discontinuity.
        result.scroll[axis]=static_cast<float>(result.clamp ?
            std::clamp(std::isfinite(displacement)?displacement:0.0,-1.0e6,1.0e6) : wrap(displacement,1.0));
    }
    if(!binding.metadata) return result;
    const auto& facts=binding.metadata->facts;
    const auto width=std::max(1u,facts.layerWidth ? facts.layerWidth : facts.width);
    const auto height=std::max(1u,facts.layerHeight ? facts.layerHeight : facts.height);
    result.texel={{1.0f/static_cast<float>(width),1.0f/static_cast<float>(height)}};
    const bool cycle=p.procedureType==ProcedureType::Cycle;
    const bool random=p.procedureType==ProcedureType::Random;
    const auto nx=p.numX.value_or(0),ny=p.numY.value_or(0);
    const double fps=finite(p.fps.value_or(0),0);
    if((!cycle && !random) || nx<=0 || ny<=0 || fps<=0 || facts.cubeMap) return result;
    const auto count=static_cast<std::uint32_t>(nx)*static_cast<std::uint32_t>(ny);
    if(count>65536u) return result; // runtime preview work bound
    if(facts.animated && facts.layerCount!=count) return result;
    if(!facts.animated && (facts.width<static_cast<unsigned>(nx) || facts.height<static_cast<unsigned>(ny) ||
        facts.width%static_cast<unsigned>(nx)!=0 || facts.height%static_cast<unsigned>(ny)!=0)) return result;
    // Reduction before multiplication keeps huge finite times out of integer
    // conversion UB. Negative times wrap mathematically, not via abs().
    const auto step=static_cast<std::uint32_t>(std::floor(wrap(seconds,static_cast<double>(count)/fps)*fps));
    result.frame=step%count;
    if(random) {
        std::uint32_t seed=2166136261u;
        for(const unsigned char c:binding.key) { seed^=c;seed*=16777619u; }
        result.frame=hash(step^seed)%count; // deterministic viewer extension
    }
    if(facts.animated) { result.layer=result.frame; return result; }
    result.atlas=true;
    result.scale={{1.0f/static_cast<float>(nx),1.0f/static_cast<float>(ny)}};
    result.offset={{static_cast<float>(result.frame%static_cast<std::size_t>(nx))*result.scale[0],
        1.0f-static_cast<float>(result.frame/static_cast<std::size_t>(nx)+1u)*result.scale[1]}};
    return result;
}
RuntimeMaterial evaluateMaterial(const Material& m,const SurfaceInputs& surface,double seconds) {
    RuntimeMaterial r;
    const auto& p=props(m.diffuse);
    const auto& b=props(m.bumpMap);
    r.opacity=bounded(surface.opacity,0,1,1)*bounded(surface.instanceOpacity,0,1,1)*
        bounded(p.waterAlpha.value_or(1),0,1,1);
    // Display bound, not an authored format range. Preserve the sampled value
    // in Pose; reject non-finite input per component here at the render boundary.
    for(std::size_t channel=0;channel<r.selfIllumination.size();++channel)
        r.selfIllumination[channel]=bounded(surface.selfIllumination[channel],0,16,0);
    const bool environment=(!m.environmentMap.key.empty() || !m.bumpyShiny.key.empty()) &&
        p.environmentMapped.value_or(true);
    // Odyssey-style inverse diffuse-alpha reflection mask. Explicit blend
    // directives take precedence; header alphaBlending is never substituted.
    r.textureAlphaIsOpacity=!environment || p.blending.has_value();
    const auto alpha=m.diffuse.metadata ? m.diffuse.metadata->facts.pixelAlpha : PixelAlpha::Opaque;
    r.alphaTest=p.blending==BlendDirective::PunchThrough ||
        (!p.blending && r.textureAlphaIsOpacity && alpha==PixelAlpha::Binary);
    if(p.blending==BlendDirective::Additive) r.pass=RenderPass::Additive;
    else if(r.opacity<1 || surface.vertexAlpha || p.decal.value_or(false) ||
        (r.textureAlphaIsOpacity && alpha==PixelAlpha::Fractional && !r.alphaTest)) r.pass=RenderPass::Translucent;
    else if(r.alphaTest) r.pass=RenderPass::PunchThrough;
    if(!m.bumpMap.key.empty()) r.bumpKind=b.bumpMapKind.value_or(BumpMapKind::Height);
    r.bumpScale=bounded(p.bumpMapScaling.value_or(b.bumpMapScaling.value_or(1)),-100,100,1)*
        bounded(b.bumpIntensity.value_or(p.bumpIntensity.value_or(1)),0,100,1);
    r.diffuseBump=b.diffuseBump.value_or(p.diffuseBump.value_or(true));
    r.specularBump=b.specularBump.value_or(p.specularBump.value_or(false));
    r.diffuseBumpIntensity=bounded(b.diffuseBumpIntensity.value_or(p.diffuseBumpIntensity.value_or(1)),0,10,1);
    r.specularBumpIntensity=bounded(b.specularBumpIntensity.value_or(p.specularBumpIntensity.value_or(1)),0,10,1);
    r.specularColor=b.specularColor.value_or(p.specularColor.value_or(std::array<float,3>{{1,1,1}}));
    for(auto& v:r.specularColor) v=bounded(v,0,10,1);
    r.environmentStrength=environment ? bounded(p.environmentAlpha.value_or(1),0,10,1) : 0;
    const auto velocity=surface.uvAnimation ? surface.uvVelocity : std::array<float,2>{};
    r.diffuseUv=evaluateUv(m.diffuse,seconds,velocity);
    r.bumpUv=evaluateUv(m.bumpMap,seconds,velocity);
    return r;
}
std::vector<std::size_t> renderQueueOrder(const std::vector<QueueKey>& keys) {
    std::vector<std::size_t> order(keys.size());std::iota(order.begin(),order.end(),0u);
    std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) {
        const auto& x=keys[a];const auto& y=keys[b];
        if(x.pass!=y.pass) return x.pass<y.pass;
        if(x.pass==RenderPass::Translucent) {
            const float xd=finite(x.eyeDepth,0),yd=finite(y.eyeDepth,0);
            if(xd!=yd) return xd>yd;
            // A complete tie-break is required for strict weak ordering. The
            // engine-defined part is the signed renderOrder comparison within
            // one nonzero owner group. Group ordering (including ordinary
            // meshes in group zero) is only a deterministic equal-depth tie.
            if(x.authoredOrderGroup!=y.authoredOrderGroup)
                return x.authoredOrderGroup<y.authoredOrderGroup;
            if(x.authoredOrderGroup!=0 && x.authoredOrder!=y.authoredOrder)
                return x.authoredOrder<y.authoredOrder;
        }
        return false;
    });
    return order;
}
} // namespace neoshared::material
