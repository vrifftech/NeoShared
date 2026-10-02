#include <neoshared/model/Emitter.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace neoshared::model {
namespace {
constexpr float pi=3.14159265358979323846f, tau=2*pi;
std::string lower(std::string s) {
    for (auto& c:s) if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return s;
}
bool finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
bool finite(const Mat4& m) {
    return std::all_of(m.values.begin(),m.values.end(),[](float v){return std::isfinite(v);});
}
float bound(float v,float lo,float hi,float fallback=0) {
    return std::isfinite(v) ? std::clamp(v,lo,hi) : fallback;
}
Vec3 add(Vec3 a,Vec3 b) {return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 sub(Vec3 a,Vec3 b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 mul(Vec3 a,float s) {return {a.x*s,a.y*s,a.z*s};}
float dot(Vec3 a,Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b) {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float length(Vec3 a) {return std::sqrt(dot(a,a));}
Vec3 unit(Vec3 v,Vec3 fallback={0,0,1}) {
    const float n=length(v);return std::isfinite(n)&&n>1e-10f ? mul(v,1/n) : fallback;
}
Vec3 position(const Mat4& m) {return {m.values[12],m.values[13],m.values[14]};}
Vec3 axis(const Mat4& m,unsigned n) {return unit({m.values[n*4],m.values[n*4+1],m.values[n*4+2]},n==0?Vec3{1,0,0}:n==1?Vec3{0,1,0}:Vec3{0,0,1});}
Vec3 rotate(Vec3 v,Vec3 a,float angle) {
    const float c=std::cos(angle),s=std::sin(angle);
    return add(add(mul(v,c),mul(cross(a,v),s)),mul(a,dot(a,v)*(1-c)));
}
void rotateBasisToDirection(EmitterParticle& p,Vec3 target) {
    const Vec3 from=unit(p.direction);
    target=unit(target,from);
    const Vec3 arc=cross(from,target);
    const float sine=length(arc),cosine=bound(dot(from,target),-1,1,1);
    if(sine<=1.0e-7f) {
        if(cosine>=0) {p.direction=target;return;}
        Vec3 fallback=cross(from,p.right);
        if(length(fallback)<=1.0e-7f) fallback=cross(from,{0,1,0});
        const Vec3 axis=unit(fallback,{1,0,0});
        p.right=rotate(p.right,axis,pi);p.up=rotate(p.up,axis,pi);p.direction=target;
        return;
    }
    const Vec3 axis=mul(arc,1/sine);
    const float angle=std::atan2(sine,cosine);
    p.right=unit(rotate(p.right,axis,angle),p.right);
    p.up=unit(rotate(p.up,axis,angle),p.up);
    p.direction=target;
}
void applyDeadSpace(const EmitterNodeData& data,EmitterParticle& p,
    const ParticleEnvironment* environment) {
    if(data.deadSpace==0 || !std::isfinite(data.deadSpace) || !environment ||
       !environment->cameraPosition || !finite(*environment->cameraPosition)) return;
    // K2 compares |cameraDirection dot particleDirection| against
    // cos(pi/2-deadSpace). Directions inside the camera-perpendicular band are
    // moved to its nearest boundary without consuming another random value.
    const Vec3 cameraDirection=unit(sub(p.position,*environment->cameraPosition),{});
    if(length(cameraDirection)<=1.0e-7f) return;
    const float threshold=bound(std::cos(pi*0.5f-data.deadSpace),0,1);
    const float current=bound(dot(p.direction,cameraDirection),-1,1);
    if(std::fabs(current)>=threshold) return;
    Vec3 tangent=sub(p.direction,mul(cameraDirection,current));
    if(length(tangent)<=1.0e-7f) tangent=sub(p.right,mul(cameraDirection,dot(p.right,cameraDirection)));
    if(length(tangent)<=1.0e-7f) tangent=cross(cameraDirection,
        std::fabs(cameraDirection.z)<0.9f?Vec3{0,0,1}:Vec3{0,1,0});
    const float signedThreshold=current<0 ? -threshold : threshold;
    const Vec3 target=add(mul(cameraDirection,signedThreshold),
        mul(unit(tangent),std::sqrt(std::max(0.0f,1-threshold*threshold))));
    rotateBasisToDirection(p,target);
}
Vec3 rotationDelta(Vec3 v,const Mat4& old,const Mat4& now) {
    return add(add(mul(axis(now,0),dot(v,axis(old,0))),mul(axis(now,1),dot(v,axis(old,1)))),
               mul(axis(now,2),dot(v,axis(old,2))));
}
bool usable(const EmitterParticle& p) {
    return std::isfinite(p.age)&&finite(p.position)&&finite(p.velocity)&&
        std::max({std::fabs(p.position.x),std::fabs(p.position.y),std::fabs(p.position.z)})<1.0e6f;
}
int integer(float v) {return static_cast<int>(bound(v,-1000000,1000000));}
float interpolate(float a,float b,float t) {return a*(1-t)+b*t;}
struct LifetimeSegment {unsigned first{},second{};float weight{};};
LifetimeSegment segment(const NodeEmitterState& s,const EmitterParticle& p) {
    if(s.lifeExpectancy<0) return {0,0,0};
    const float t=p.age*(s.lifeExpectancy>0 ? 1.0f/s.lifeExpectancy : 0.01f);
    if(s.percentStart==255.0f) return {0,2,t};
    if(t>=s.percentEnd) return {2,2,0};
    if(t>=s.percentMid) {
        const float d=s.percentEnd-s.percentMid;
        return {1,2,d!=0 ? (t-s.percentMid)/d : 1.0f};
    }
    if(t>=s.percentStart) {
        const float d=s.percentMid-s.percentStart;
        return {0,1,d!=0 ? (t-s.percentStart)/d : 1.0f};
    }
    return {0,0,0};
}
float evaluate(const std::array<float,3>& v,LifetimeSegment s) {
    return s.first==s.second ? v[s.first] : interpolate(v[s.first],v[s.second],s.weight);
}
void initialFrame(const EmitterNodeData& data,const NodeEmitterState& s,EmitterParticle& p,ParticleRandom& rng) {
    p.frame=integer(s.frameStart);
    if((data.rawWords.back()&emitter_flags::RandomFrame)!=0) {
        // The original applies abs AFTER adding one, even for descending ranges.
        const int span=std::abs(integer(s.frameEnd-s.frameStart)+1);
        if(span>0) p.frame=integer(static_cast<float>(rng.next()%static_cast<unsigned>(span))+s.frameStart);
    }
}
EmitterParticle initializeParticle(const EmitterNodeData& data,const NodeEmitterState& s,
    const Mat4& world,const Mat4& previous,float dt,float scale,ParticleRandom& rng,
    const ParticleEnvironment* environment) {
    EmitterParticle p;p.lifetime=s.lifeExpectancy;p.sourceScale=scale;
    p.right=axis(world,0);p.up=axis(world,1);p.direction=axis(world,2);
    p.position=position(world);
    float dv=0;
    const float rv=bound(s.randomVelocity,0,10000);
    if(static_cast<double>(rv)>0.01) {
        const int count=std::max(1,integer(rv*100));
        int value=static_cast<int>(rng.next()%static_cast<unsigned>(count));
        if(rng.next()%2u) value=-value;
        dv=static_cast<float>(value)*0.01f;
    }
    const int width=integer(bound(s.xSize,0,100000)*100*scale);
    const int height=integer(bound(s.ySize,0,100000)*100*scale);
    if(width || height) {
        const auto offset=[&](int amount) {
            if(amount<=0) return 0;
            const auto a=rng.next(),b=rng.next();
            // Tiny invalid rectangles divide by zero in the binary. Preserve
            // draw count but use a finite zero offset instead.
            const auto divisor=static_cast<unsigned>(amount/2);
            return divisor ? static_cast<int>((a*b)%divisor) : 0;
        };
        int x=offset(width),y=offset(height);
        if(rng.next()%2u) x=-x;
        if(rng.next()%2u) y=-y;
        p.position=add(add(p.position,mul(p.right,static_cast<float>(x)*0.0001f)),
                                  mul(p.up,static_cast<float>(y)*0.0001f));
    }
    p.tail=p.position;
    if(data.rawWords.back()&emitter_flags::InheritVelocity) {
        const float inv=dt>0 ? 1.0f/dt : 0.01f;
        const float radius=length(sub(position(world),p.position));
        const Vec3 inherited=add(mul(sub(axis(world,2),axis(previous,2)),radius*inv),
                                 mul(sub(position(world),position(previous)),inv));
        p.inheritedSpeed=length(inherited);
        p.inheritedDirection=unit(inherited,{0,0,0});
    }
    if(s.spread!=0) {
        const unsigned count=static_cast<unsigned>(std::max(1,integer(bound(s.spread,0,tau)*0.5f*100)+1));
        const float tilt=static_cast<float>(rng.next()%count)*0.01f;
        const float azimuth=static_cast<float>(rng.next()%628u)*0.01f;
        const auto orient=[&](Vec3 v) {return rotate(rotate(v,axis(world,0),tilt),axis(world,2),azimuth);};
        p.right=orient(p.right);p.up=orient(p.up);p.direction=orient(p.direction);
    }
    applyDeadSpace(data,p,environment);
    p.velocity=mul(p.direction,bound(s.velocity,-10000,10000)+dv);
    p.frame=integer(s.frameStart);
    return p;
}
int nextFrame(const NodeEmitterState& s,int f) {
    if(s.frameStart==s.frameEnd) return -1;
    const int n=f+(s.frameStart>s.frameEnd?-1:1);
    return (s.frameStart>s.frameEnd ? static_cast<float>(n)<s.frameEnd : static_cast<float>(n)>s.frameEnd) ? -1 : n;
}
int previousFrame(const NodeEmitterState& s,int f) {
    if(s.frameStart==s.frameEnd) return -1;
    const int n=f+(s.frameStart>s.frameEnd?1:-1);
    return (s.frameStart>s.frameEnd ? static_cast<float>(n)>s.frameStart : static_cast<float>(n)<s.frameStart) ? -1 : n;
}
} // namespace

float calculateK2EmitterRadius(const EmitterRadiusInput& input) noexcept {
    if(input.forceUnbounded) return 10000.0f;
    if(!input.state || !finite(input.emitterPosition)) return 0.0f;
    const auto& state=*input.state;
    if(input.targetPosition && finite(*input.targetPosition)) {
        const float result=length(sub(*input.targetPosition,input.emitterPosition));
        return std::isfinite(result) ? result : 0.0f;
    }

    const float gravityAcceleration=std::fabs(state.mass*-9.81f);
    const float windMagnitude=finite(input.globalWind) ? length(input.globalWind) : 0.0f;
    const float life=state.lifeExpectancy;
    const float travel=((state.velocity+state.randomVelocity+windMagnitude)*life)+
        (0.5f*gravityAcceleration*life*life);

    const float maximumX=std::max({state.sizeStart,state.sizeMid,state.sizeEnd});
    const float maximumY=std::max({state.sizeStartY,state.sizeMidY,state.sizeEndY});
    const float halfX=maximumX*0.5f;
    const float halfY=(maximumY<0.0f ? maximumX : maximumY)*0.5f;
    const float particleRadius=std::sqrt(halfX*halfX+halfY*halfY);
    const float sourceX=state.xSize/100.0f;
    const float sourceY=state.ySize/100.0f;
    const float sourceRadius=std::sqrt(sourceX*sourceX+sourceY*sourceY);
    float result=(travel+particleRadius+sourceRadius)*1.15f;
    if(!std::isfinite(result)) result=0.0f;

    for(const auto& object:input.attachedObjects) {
        if(!finite(object.center) || !std::isfinite(object.radius)) continue;
        const float candidate=length(sub(object.center,input.emitterPosition))+object.radius;
        if(std::isfinite(candidate)) result=std::max(result,candidate);
    }
    return result;
}

std::vector<std::size_t> k2EmitterRoomMembership(
    Vec3 currentOwnerPosition,Vec3 previousOwnerPosition,
    const std::vector<std::size_t>& cachedOwnerRooms,
    const std::vector<EmitterSceneRoom>& rooms) {
    if(rooms.empty()) return {};
    const bool unchanged=currentOwnerPosition.x==previousOwnerPosition.x &&
        currentOwnerPosition.y==previousOwnerPosition.y &&
        currentOwnerPosition.z==previousOwnerPosition.z;
    if(unchanged && !cachedOwnerRooms.empty() && cachedOwnerRooms.front()<rooms.size())
        return {cachedOwnerRooms.front()};

    std::vector<std::size_t> result;
    result.reserve(rooms.size());
    for(std::size_t index=0;index<rooms.size();++index) {
        const auto& room=rooms[index];
        if(currentOwnerPosition.x>=room.minimumX &&
           currentOwnerPosition.y>=room.minimumY &&
           currentOwnerPosition.x<=room.maximumX &&
           currentOwnerPosition.y<=room.maximumY) result.push_back(index);
    }
    if(result.empty()) {
        result.resize(rooms.size());
        for(std::size_t index=0;index<rooms.size();++index) result[index]=index;
    }
    return result;
}

bool k2EmitterOutside(Vec3 center,float radius,
                      std::optional<Vec3> cameraPosition,
                      const std::vector<EmitterScenePlane>& planes,
                      const EmitterCullPolicy& policy) noexcept {
    if(!finite(center) || !std::isfinite(radius)) return true;
    if(policy.screenSizeGate && cameraPosition && finite(*cameraPosition)) {
        const float screenRadius=std::min(radius,policy.maximumScreenRadius);
        const float surfaceDistance=length(sub(*cameraPosition,center))-screenRadius;
        if(surfaceDistance>policy.nearSurfaceDistance &&
           screenRadius/surfaceDistance<policy.minimumRadiusRatio) return true;
    }
    for(const auto& plane:planes) {
        if(!finite(plane.normal) || !std::isfinite(plane.distance)) continue;
        if(dot(plane.normal,center)+plane.distance-radius>0.0f) return true;
    }
    return false;
}

int compareK2EmitterBucketEntries(const EmitterBucketEntry& left,
                                  const EmitterBucketEntry& right,
                                  Vec3 cameraPosition) noexcept {
    if(left.hasOwner && right.hasOwner && left.ownerIdentity==right.ownerIdentity)
        return static_cast<int>(left.renderOrder)-static_cast<int>(right.renderOrder);
    if(left.category==2 && right.category!=2) return 1;
    if(left.category!=2 && right.category==2) return -1;
    const Vec3 leftPosition=left.hasOwner ? left.ownerPosition : left.emitterPosition;
    const Vec3 rightPosition=right.hasOwner ? right.ownerPosition : right.emitterPosition;
    const float leftDistance=finite(cameraPosition)&&finite(leftPosition)
        ? length(sub(cameraPosition,leftPosition)) : 0.0f;
    const float rightDistance=finite(cameraPosition)&&finite(rightPosition)
        ? length(sub(cameraPosition,rightPosition)) : 0.0f;
    if(std::fabs(leftDistance-rightDistance)>=0.01f)
        return leftDistance>rightDistance ? -1 : 1;
    if(left.identity==right.identity) return 0;
    return left.identity>right.identity ? -1 : 1;
}

void sortK2EmitterBucket(std::vector<EmitterBucketEntry>& entries,Vec3 cameraPosition) {
    // EmitterOrderCmp is an old qsort comparator whose same-owner branch
    // bypasses the cross-owner category branch. Valid engine buckets satisfy
    // the corresponding owner/category invariants, but accepting arbitrary
    // host-provided records through std::stable_sort would impose C++'s strict
    // weak-ordering precondition on a comparator that does not promise it.
    // A stable insertion pass applies the recovered pairwise comparator
    // directly and remains deterministic for complete ties.
    for(std::size_t index=1;index<entries.size();++index) {
        auto entry=entries[index];
        std::size_t destination=index;
        while(destination>0u && compareK2EmitterBucketEntries(
              entry,entries[destination-1u],cameraPosition)<0) {
            entries[destination]=entries[destination-1u];
            --destination;
        }
        entries[destination]=entry;
    }
}

void moveEmitterParticle(EmitterParticle& p,const NodeEmitterState& s,const Mat4& old,
    const Mat4& now,std::uint32_t flags,float dt,const ParticleEnvironment* environment) {
    if(!std::isfinite(dt) || dt<=0 || dt>0.25f) return;
    const bool targeted=(flags&emitter_flags::PointToPoint)!=0;
    // Only the standard mover skips settled particles. Targeted paths keep
    // following the object even after a collision set the resting flag.
    if(p.stopped && !targeted) return;
    // MoveFunc does not substitute a standard mover when a target is absent.
    if(targeted && (!environment || !environment->target || !finite(*environment->target))) return;
    const Vec3 original=p.position;
    if(flags&emitter_flags::Inherit) {
        p.position=add(position(now),rotationDelta(sub(p.position,position(old)),old,now));
        p.velocity=rotationDelta(p.velocity,old,now);
    }
    if(!targeted && (flags&emitter_flags::InheritPart)) {
        const auto delta=sub(position(now),position(old));
        p.position=add(p.position,delta);p.tail=add(p.tail,delta);
    }
    const auto start=p.position;
    Vec3 predicted=add(start,mul(p.velocity,dt));
    if((flags&emitter_flags::AffectedByWind) && environment && environment->windDisplacement) {
        const auto wind=environment->windDisplacement(start,dt);
        if(!finite(wind)) throw ModelError("Particle scene returned non-finite wind displacement");
        predicted=add(predicted,wind);
    }
    if(targeted) {
        const auto target=position(*environment->target);
        if(flags&emitter_flags::PointToPointBezier) {
            // Binary order: advance freely, then blend toward a cubic whose
            // handles follow the source and TARGET orientations respectively.
            const float t=s.lifeExpectancy!=0 ? std::min(1.0f,p.age/s.lifeExpectancy) : 1.0f;
            const float u=1-t, t3=t*t*t;
            const auto source=position(now);
            const auto handleA=add(source,mul(axis(now,2),s.bezier2));
            const auto handleB=add(target,mul(axis(*environment->target,2),s.bezier3));
            const auto curve=add(add(mul(source,u*u*u),mul(handleA,3*t*u*u)),
                                 add(mul(handleB,3*t*t*u),mul(target,t3)));
            const float weight=std::pow(t3,s.combineTime);
            if(!finite(curve) || !std::isfinite(weight)) throw ModelError("Invalid particle Bezier inputs");
            predicted=add(mul(curve,weight),mul(predicted,1-weight));
        } else {
            const auto delta=sub(target,start);
            const float threshold=std::fabs(s.targetThreshold);
            if(dot(delta,delta)<=threshold*threshold) {p.age=s.lifeExpectancy;return;}
            const float drag=std::pow(s.drag,dt);
            if(!std::isfinite(drag)) throw ModelError("Invalid particle target drag");
            const auto acceleration=mul(unit(delta,{0,0,0}),s.gravity);
            p.velocity=add(mul(p.velocity,drag),mul(acceleration,dt));
            predicted=add(predicted,mul(acceleration,dt*dt));
            const auto travel=sub(predicted,start);
            if(dot(travel,travel)>1.0e-12f) p.direction=unit(travel);
            const float fraction=dot(travel,travel)>1.0e-12f
                ? std::clamp(dot(delta,travel)/dot(travel,travel),0.0f,1.0f) : 0;
            const auto closest=add(start,mul(travel,fraction));
            const auto miss=sub(target,closest);
            if(dot(miss,miss)<=threshold*threshold) p.age=s.lifeExpectancy;
        }
    }
    p.position=add(predicted,mul(p.inheritedDirection,p.inheritedSpeed*dt));
    p.velocity.z+=bound(s.mass,-10000,10000)*(-9.81f)*dt;
    p.inheritedSpeed=std::fabs(p.inheritedSpeed*(1-dt));
    if((flags&emitter_flags::Bounce) && environment && environment->trace) {
        const auto hit=environment->trace(original,p.position);
        if(hit) {
            if(!finite(hit->normal) || dot(hit->normal,hit->normal)<1.0e-12f ||
                !std::isfinite(hit->fraction) || hit->fraction<0 || hit->fraction>1)
                throw ModelError("Particle scene returned an invalid collision contact");
            const auto n=unit(hit->normal);
            const auto reflected=sub(p.velocity,mul(n,2*dot(p.velocity,n)));
            const float coefficient=bound(s.bounceCoefficient,-1000,1000);
            bool settle=false;
            if(targeted || environment->bounceMode==ParticleBounceMode::Reflection) {
                p.velocity=mul(reflected,coefficient);
                if(targeted) {
                    p.position=add(p.position,mul(n,0.005f));
                    settle=dot(p.velocity,p.velocity)<0.5f;
                } else {
                    const float speed=length(p.velocity);
                    settle=std::fabs(n.x)<0.70710678f && std::fabs(n.y)<0.70710678f && speed<26*dt;
                }
            } else {
                const float scale=std::clamp(0.033f/dt,0.5f,1.0f);
                p.velocity={reflected.x*0.75f,reflected.y*0.75f,reflected.z*0.75f*coefficient*scale};
                if(coefficient==0) p.velocity={};
                settle=dot(p.velocity,p.velocity)<0.5f;
            }
            if(settle) {
                p.stopped=true;p.contactNormal=n;p.direction=n;
                p.position=add(p.position,mul(n,0.02f));
                // Surface-facing basis for the modes that consume particle axes.
                // Shortest arc from world Z, without introducing an arbitrary
                // twist around the contact normal.
                if(n.z>-0.999999f) {
                    const float d=1+n.z;
                    p.right={1-n.x*n.x/d,-n.x*n.y/d,-n.x};
                    p.up={-n.x*n.y/d,1-n.y*n.y/d,-n.y};
                } else {p.right={1,0,0};p.up={0,-1,0};}
            }
        }
    }
}
Vec3 particleBlastDisplacement(const ParticleBlast& blast,Vec3 point) noexcept {
    return pointSourceWindVector(
        {blast.center,blast.radius,blast.remaining,blast.strength},point);
}
std::optional<ParticleContact> traceParticleGround(Vec3 from,Vec3 to,float height) noexcept {
    if(!finite(from)||!finite(to)||!std::isfinite(height)||to.z>=from.z || from.z<height || to.z>height) return {};
    return ParticleContact{{0,0,1},std::clamp((from.z-height)/(from.z-to.z),0.0f,1.0f)};
}
namespace {
float component(Vec3 p,unsigned a) {return a==0?p.x:a==1?p.y:p.z;}
Vec3 minimum(Vec3 a,Vec3 b) {return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)};}
Vec3 maximum(Vec3 a,Vec3 b) {return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)};}
bool segmentBounds(Vec3 from,Vec3 delta,Vec3 lo,Vec3 hi,float maxFraction) {
    float enter=0,leave=maxFraction;
    for(unsigned a=0;a<3;++a) {
        const float d=component(delta,a),f=component(from,a),l=component(lo,a),h=component(hi,a);
        if(std::fabs(d)<1.0e-12f) {if(f<l || f>h) return false;}
        else {
            float p=(l-f)/d,q=(h-f)/d;if(p>q) std::swap(p,q);
            enter=std::max(enter,p);leave=std::min(leave,q);if(enter>leave) return false;
        }
    }
    return true;
}
}
bool ParticleCollisionMesh::build(std::vector<ParticleTriangle> triangles) {
    triangles_.clear();branches_.clear();
    if(triangles.size()>32768) return false;
    for(const auto& t:triangles) for(const auto p:{t.a,t.b,t.c})
        if(!finite(p)||std::max({std::fabs(p.x),std::fabs(p.y),std::fabs(p.z)})>1.0e6f) return false;
    triangles.erase(std::remove_if(triangles.begin(),triangles.end(),[](const ParticleTriangle& t) {
        return dot(cross(sub(t.b,t.a),sub(t.c,t.a)),cross(sub(t.b,t.a),sub(t.c,t.a)))<1.0e-16f;
    }),triangles.end());
    triangles_=std::move(triangles);branches_.reserve(triangles_.size()*2);
    if(!triangles_.empty()) buildBranch(0,triangles_.size());
    return true;
}
std::size_t ParticleCollisionMesh::buildBranch(std::size_t start,std::size_t count) {
    Branch branch;branch.start=start;branch.count=count;
    branch.minimum=branch.maximum=triangles_[start].a;
    for(std::size_t i=start;i<start+count;++i) for(const auto p:{triangles_[i].a,triangles_[i].b,triangles_[i].c}) {
        branch.minimum=minimum(branch.minimum,p);branch.maximum=maximum(branch.maximum,p);
    }
    const auto index=branches_.size();branches_.push_back(branch);
    if(count>8) {
        const auto extent=sub(branch.maximum,branch.minimum);unsigned a=extent.y>extent.x?1u:0u;
        if(extent.z>component(extent,a)) a=2;
        const auto middle=start+count/2;
        std::nth_element(triangles_.begin()+static_cast<std::ptrdiff_t>(start),
            triangles_.begin()+static_cast<std::ptrdiff_t>(middle),
            triangles_.begin()+static_cast<std::ptrdiff_t>(start+count),[a](const ParticleTriangle& x,const ParticleTriangle& y) {
                return component(add(add(x.a,x.b),x.c),a)<component(add(add(y.a,y.b),y.c),a);
            });
        const auto left=buildBranch(start,count/2),right=buildBranch(middle,count-count/2);
        branches_[index].left=left;branches_[index].right=right;branches_[index].count=0;
    }
    return index;
}
std::optional<ParticleContact> ParticleCollisionMesh::trace(Vec3 from,Vec3 to) const noexcept {
    if(!finite(from)||!finite(to)||branches_.empty()) return {};
    const auto delta=sub(to,from);if(dot(delta,delta)<1.0e-16f) return {};
    std::array<std::size_t,64> stack{};std::size_t size=1;float best=1;
    std::optional<ParticleContact> result;
    while(size) {
        const auto& branch=branches_[stack[--size]];
        if(!segmentBounds(from,delta,branch.minimum,branch.maximum,best)) continue;
        if(branch.count==0) {
            // Balanced median splits over <=32768 triangles fit this stack.
            if(size+2>stack.size()) return {};
            stack[size++]=branch.left;stack[size++]=branch.right;continue;
        }
        for(std::size_t i=branch.start;i<branch.start+branch.count;++i) {
            const auto& t=triangles_[i];const auto e1=sub(t.b,t.a),e2=sub(t.c,t.a);
            const auto h=cross(delta,e2);const float det=dot(e1,h);
            if(std::fabs(det)<1.0e-10f) continue;
            const auto s=sub(from,t.a);const float u=dot(s,h)/det;if(u<0||u>1) continue;
            const auto q=cross(s,e1);const float v=dot(delta,q)/det;if(v<0||u+v>1) continue;
            const float f=dot(e2,q)/det;if(f<0||f>best) continue;
            auto n=unit(cross(e1,e2));if(dot(n,delta)>0) n=mul(n,-1);
            best=f;result=ParticleContact{n,f};
        }
    }
    return result;
}

std::uint32_t ParticleRandom::next() noexcept {
    state=state*214013u+2531011u;return (state>>16u)&32767u;
}
EmitterPreviewSupport emitterPreviewSupport(const EmitterNodeData& data) {
    EmitterPreviewSupport result;
    const auto update=lower(data.updateMode),render=lower(data.renderMode),blend=lower(data.blendMode);
    if(update=="fountain") result.update=EmitterUpdate::Fountain;
    else if(update=="single") result.update=EmitterUpdate::Single;
    else if(update=="explosion") result.update=EmitterUpdate::Explosion;
    else if(update=="lightning") result.update=EmitterUpdate::Lightning;
    else {result.reason="Unsupported update mode: "+data.updateMode;return result;}
    if(!data.chunkName.empty()) {
        if(result.update==EmitterUpdate::Lightning) {
            result.reason="Lightning with chunk models is not an authored combination supported by this runtime";
            return result;
        }
        result.supported=true;return result; // real model instance, not a sprite render mode
    }
    if(render=="normal") result.render=EmitterRender::Normal;
    else if(render=="billboard_to_world_z") result.render=EmitterRender::BillboardWorldZ;
    else if(render=="billboard_to_local_z") result.render=EmitterRender::BillboardLocalZ;
    else if(render=="aligned_to_world_z") result.render=EmitterRender::AlignedWorldZ;
    else if(render=="aligned_to_particle_dir") result.render=EmitterRender::AlignedParticleDirection;
    else if(render=="motion_blur") result.render=EmitterRender::MotionBlur;
    else if(render=="linked") result.render=EmitterRender::Linked;
    else {result.reason="Unsupported render mode: "+data.renderMode;return result;}
    if(blend=="normal") result.blend=EmitterBlend::Normal;
    else if(blend=="punchthrough" || blend=="punch-through") result.blend=EmitterBlend::PunchThrough;
    else if(blend=="lighten" || blend=="additive") result.blend=EmitterBlend::Additive;
    else {result.reason="Unsupported blend mode: "+data.blendMode;return result;}
    result.supported=true;return result;
}
float emitterBirthRateForQuality(const EmitterNodeData&,const NodeEmitterState& state,
    EmitterUpdate update,ParticleQuality quality) noexcept {
    float rate=bound(state.birthRate,0,1000000);
    if(quality==ParticleQuality::Full) return rate;
    float cap=rate;
    switch(update) {
    case EmitterUpdate::Fountain:
        cap=state.lifeExpectancy<3.0f?10.0f:3.0f;
        break;
    case EmitterUpdate::Single:
        cap=17.0f;
        break;
    case EmitterUpdate::Lightning:
        cap=20.0f;
        break;
    case EmitterUpdate::Explosion:
        cap=std::max({state.sizeStart,state.sizeMid,state.sizeEnd})<0.3f?7.0f:3.0f;
        break;
    }
    if(rate>cap) rate=std::min(rate*0.5f,cap);
    return rate;
}
std::vector<std::string> emitterPreviewWarnings(const EmitterNodeData& data,const NodeEmitterState&) {
    std::vector<std::string> result;
    const auto support=emitterPreviewSupport(data);
    if(!support.supported) {result.push_back(support.reason);return result;}
    const auto flags=data.rawWords.back();
    if(flags&emitter_flags::PointToPoint) result.emplace_back("Point-to-point motion requires an explicit target transform");
    if(flags&emitter_flags::AffectedByWind) result.emplace_back("Affected by scene wind; requires a supplied wind field");
    if(flags&emitter_flags::Bounce) result.emplace_back("Collision uses the supplied scene proxy");
    if(flags&emitter_flags::Splat) result.emplace_back("Splat particles orient on contact; no surface-clipped decal projection");
    if(support.update==EmitterUpdate::Lightning) result.emplace_back(
        "Lightning requires an explicit target transform; branches are one-level K2 child emitters");
    if(flags&emitter_flags::Tinted) result.emplace_back("Particle RGB uses the caller-supplied object tint");
    if(flags&emitter_flags::InheritLocal) result.emplace_back(
        "Inherit-local uses the caller-supplied owner translation delta");
    if(data.deadSpace!=0) result.emplace_back(
        "Dead-space emission is camera-relative and requires a supplied camera position");
    if((flags&emitter_flags::DepthTexture) || !data.depthTexture.empty())
        result.emplace_back("Depth-texture metadata is retained; the inspected K2 Android UsesDepthTexture path returns false");
    if(support.update==EmitterUpdate::Explosion && (data.blastRadius!=0 || data.blastLength!=0))
        result.emplace_back("Explosion blast wind requires scene point-source wind to be enabled");
    constexpr std::uint32_t known=0x1fffu;
    if(flags&~known) result.emplace_back("Additional emitter flags are retained without inferred behavior");
    return result;
}
void EmitterSimulation::reset(std::uint64_t newSeed) noexcept {
    particles.clear();accumulator=0;birthRemainder=0;seed=newSeed;random.state=static_cast<std::uint32_t>(newSeed);
    nextSerial=0;droppedBirths=0;invalidEnvironmentSamples=0;detonations=0;freeParticles=0;pendingBursts=0;
    world=previousWorld=identityMatrix();distanceRemainder={};initialized=false;previousDetonate=false;
    ribbonStarts.clear();lightningPrevious.clear();lightningNext.clear();lightningOffsets.clear();
    tangentPrevious.clear();tangentNext.clear();lightningBranches.clear();
    lightningAge=controlAge=0;lightningMainCount=lightningActiveBranches=0;lightningReady=false;
    renderTint={1,1,1};renderAlpha=1;
}
void EmitterSimulation::detonate() noexcept {pendingBursts=1;}
void lightningFractal(std::vector<Vec3>& out,std::size_t index,std::size_t span,
    Vec3 direction,float inherited,float amplitude,float radius,ParticleRandom& rng) {
    // Original recurrence deliberately adds the scalar inherited X component;
    // it is not midpoint interpolation. Preserve duplicate leaf visits/RNG.
    if(out.empty() || out.size()>8192 || span>8192 || !finite(direction) ||
       !std::isfinite(inherited) || !std::isfinite(amplitude) || !std::isfinite(radius)) return;
    const unsigned divisor=static_cast<unsigned>(std::max(0,integer(amplitude*100)));
    const auto random=rng.next();
    const float offset=static_cast<float>(divisor?random%divisor:0)*0.01f*radius;
    const float value=inherited+offset;
    if(index<out.size()) out[index]=mul(direction,value);
    if(span) {
        const auto half=span/2;
        if(index>=half) lightningFractal(out,index-half,half,direction,value,amplitude*0.5f,radius,rng);
        lightningFractal(out,index+half,half,direction,value,amplitude*0.5f,radius,rng);
    }
}
namespace {
Vec3 cubic(Vec3 a,Vec3 b,Vec3 c,Vec3 d,float t) {
    const float u=1-t;
    return add(add(mul(a,u*u*u),mul(b,3*u*u*t)),add(mul(c,3*u*t*t),mul(d,t*t*t)));
}
Vec3 cubicTangent(Vec3 a,Vec3 b,Vec3 c,Vec3 d,float t) {
    const float u=1-t;
    return add(add(mul(sub(b,a),3*u*u),mul(sub(c,b),6*u*t)),mul(sub(d,c),3*t*t));
}
float signedHundredths(float range,ParticleRandom& rng) {
    const unsigned divisor=static_cast<unsigned>(std::max(0,integer(range*100)));
    if(!divisor) return 0;
    const float value=static_cast<float>(rng.next()%divisor)*0.01f;
    return rng.next()%2 ? -value : value;
}
Vec3 lightningTargetOffset(float targetSize,ParticleRandom& rng) {
    const unsigned divisor=static_cast<unsigned>(std::max(0,integer(bound(targetSize,0,10000)*100)));
    if(!divisor) return {};
    const auto component=[&]() {
        const auto random=rng.next();
        const float value=static_cast<float>(random%divisor)*0.01f;
        return random&1u ? value : -value;
    };
    return {component(),component(),component()};
}
Vec3 lightningConeDirection(Vec3 direction,Vec3 sideways,Vec3 upward,ParticleRandom& rng) {
    // UpdateBranches consumes a 0..44 degree pitch followed by a 0..359
    // degree rotation. Expressing that as a cone avoids depending on the
    // engine's YawPitchRoll matrix convention while preserving both ranges
    // and RNG consumption order.
    const float tilt=static_cast<float>(rng.next()%45u)*(pi/180);
    const float angle=static_cast<float>(rng.next()%360u)*(pi/180);
    const Vec3 radial=add(mul(sideways,std::cos(angle)),mul(upward,std::sin(angle)));
    return unit(add(mul(direction,std::cos(tilt)),mul(radial,std::sin(tilt))),direction);
}
void refreshLightningOffsets(std::vector<Vec3>& offsets,std::size_t count,float distance,
    Vec3 sideways,Vec3 upward,const NodeEmitterState& s,ParticleRandom& rng) {
    offsets.assign(count,{});
    if(count<2) return;
    const float subdivisions=bound(s.lightningSubdivisions,0,4096);
    const auto requested=static_cast<std::size_t>(std::max(0,integer(distance*subdivisions*0.5f)));
    // RecursiveFractal is initially called with index==span. Its largest leaf
    // is 2*span, so malformed assets are bounded to the allocated ribbon.
    const auto span=std::min(requested,(count-1)/2);
    if(span && s.lightningRadius!=0) lightningFractal(offsets,span,span,sideways,0,1.0f,
        bound(s.lightningRadius,0,10000),rng);
    const float scale=bound(s.lightningScale,0,10000);
    // The non-fractal K2 path selects one of two camera-independent transverse
    // axes, then a hundredth-unit magnitude. Retain that two-call order and
    // use the authored zigzag value as its bounded amplitude in the preview.
    const float zigzag=bound(s.lightningZigzag,0,10000)*distance/static_cast<float>(count);
    const unsigned divisor=static_cast<unsigned>(std::max(0,integer(zigzag*100)));
    for(std::size_t i=1;i+1<count;++i) {
        offsets[i]=mul(offsets[i],scale);
        if(divisor) {
            const Vec3 axisChoice=(rng.next()&1u)?sideways:upward;
            const float magnitude=static_cast<float>(rng.next()%divisor)*0.01f*scale;
            offsets[i]=add(offsets[i],mul(axisChoice,magnitude));
        }
    }
    offsets.front()=offsets.back()={};
}
void refreshLightningBranches(const EmitterNodeData& data,const NodeEmitterState& s,
    Vec3 source,Vec3 target,Vec3 direction,Vec3 sideways,Vec3 upward,
    const std::vector<Vec3>& parentTangents,std::size_t mainCount,
    std::size_t maximum,EmitterSimulation& sim,ParticleRandom& rng) {
    // Initialize allocates one level of child LightningEmitters. The runtime
    // chooses a new active prefix whenever the main controls are refreshed.
    const auto capacity=std::min<std::size_t>({static_cast<std::size_t>(data.branchCount),
        maximum/2,1024u});
    const auto previousMainCount=sim.lightningMainCount;
    sim.lightningBranches.resize(capacity);
    sim.lightningActiveBranches=capacity ? rng.next()%static_cast<unsigned>(capacity+1) : 0;
    const float tangentLength=bound(s.tangentLength,0,10000);
    const float subdivisions=bound(s.lightningSubdivisions,0,4096);
    const Vec3 mainDelta=sub(target,source);
    const float mainDistance=length(mainDelta);
    for(std::size_t i=0;i<capacity;++i) {
        auto& branch=sim.lightningBranches[i];
        if(i>=sim.lightningActiveBranches) {branch=LightningBranchState{};continue;}
        branch=LightningBranchState{};
        branch.scale=static_cast<float>(rng.next()%80u)*0.01f+0.1f;
        branch.attachment=std::min(mainCount-1,
            static_cast<std::size_t>(static_cast<float>(mainCount)*branch.scale));
        Vec3 origin=add(source,mul(mainDelta,static_cast<float>(branch.attachment)/
            static_cast<float>(std::max<std::size_t>(1,mainCount-1))));
        if(previousMainCount==mainCount && branch.attachment<sim.particles.size() &&
           finite(sim.particles[branch.attachment].position))
            origin=sim.particles[branch.attachment].position;
        branch.tangentStart=mul(lightningConeDirection(direction,sideways,upward,rng),tangentLength);
        if(branch.scale>0.5f) {
            branch.targetsMain=true;branch.target=target;
            branch.tangentEnd=parentTangents.empty()?mul(direction,tangentLength):parentTangents.back();
        } else {
            const float radialScale=(static_cast<float>(rng.next()%75u)*0.01f+0.25f)*mainDistance*0.5f;
            const float angle=static_cast<float>(rng.next()%360u)*(pi/180);
            const float forward=static_cast<float>(rng.next()%25u)*0.01f+0.25f;
            const Vec3 fork=mul(rotate(direction,upward,angle),radialScale);
            branch.target=add(add(origin,mul(mainDelta,forward)),fork);
            branch.tangentEnd=branch.tangentStart;
        }
        const float branchDistance=length(sub(branch.target,origin));
        branch.particleCount=static_cast<std::size_t>(std::max(0,
            integer(branchDistance*subdivisions+2.0f)));
        branch.lightningAge=std::numeric_limits<float>::infinity();
    }
}
void advanceLightning(const EmitterNodeData& data,const NodeEmitterState& s,
    const Mat4& world,float dt,EmitterSimulation& sim,std::size_t maximum,
    ParticleQuality quality,ParticleRandom& rng,const ParticleEnvironment* env) {
    if(!env || !env->target || !finite(*env->target) || maximum<2) {
        sim.particles.clear();sim.ribbonStarts.clear();sim.lightningReady=false;return;
    }
    const Vec3 source=position(world),rawTarget=position(*env->target);
    // LightningEmitter perturbs its target particle independently on every
    // update when targetSize is non-zero. The same random value supplies both
    // magnitude and sign for each component.
    const Vec3 target=add(rawTarget,lightningTargetOffset(s.targetSize,rng));
    const Vec3 delta=sub(target,source);
    const float distance=length(delta);
    if(!std::isfinite(distance)||distance>1.0e6f) {
        sim.particles.clear();sim.ribbonStarts.clear();sim.lightningReady=false;++sim.invalidEnvironmentSamples;return;
    }
    const auto direction=unit(delta);
    // LightningEmitter::Update takes the integer birth rate as the persistent
    // ribbon population. Subdivision remains part of the fractal/control path.
    const float requested=emitterBirthRateForQuality(data,s,EmitterUpdate::Lightning,quality);
    const auto count=std::min(maximum,static_cast<std::size_t>(std::max(0,integer(requested))));
    if(count<2) {
        sim.particles.clear();sim.ribbonStarts.clear();sim.lightningReady=false;return;
    }
    const auto controls=static_cast<std::size_t>(bound(std::floor(distance*bound(s.controlPointCount,0,4096)+0.5f)+2,2,128,2));
    const bool first=!sim.lightningReady || controls!=sim.lightningNext.size();
    const bool resized=first || count!=sim.lightningMainCount;
    sim.lightningAge+=dt;sim.controlAge+=dt;
    const float tangentLength=bound(s.tangentLength,0,10000);
    const Vec3 a=source,b=add(source,mul(axis(world,2),tangentLength)),
        c=sub(target,mul(direction,tangentLength)),d=target;
    std::vector<Vec3> base(controls),tangents(controls);
    for(std::size_t i=0;i<controls;++i) {
        const float t=static_cast<float>(i)/static_cast<float>(controls-1);
        base[i]=cubic(a,b,c,d,t);
        tangents[i]=mul(unit(cubicTangent(a,b,c,d,t),direction),tangentLength);
    }
    tangents.front()=mul(axis(world,2),tangentLength);tangents.back()=mul(direction,tangentLength);
    const auto sideways=unit(cross(direction,std::fabs(direction.z)<0.9f?Vec3{0,0,1}:Vec3{0,1,0}),{1,0,0});
    const auto upward=unit(cross(direction,sideways),{0,1,0});
    const float delay=bound(s.controlPointDelay,0,120);
    if(first) {
        sim.lightningPrevious.assign(controls,{});sim.lightningNext.assign(controls,{});
        sim.tangentPrevious=tangents;sim.tangentNext=tangents;
    }
    const bool controlRefreshed=first || delay==0 || sim.controlAge>delay;
    if(controlRefreshed) {
        sim.lightningPrevious=sim.lightningNext;sim.tangentPrevious=sim.tangentNext;
        for(std::size_t i=1;i+1<controls;++i) {
            const float radius=signedHundredths(bound(s.controlPointRadius,0,10000),rng);
            const float angle=radius!=0?static_cast<float>(rng.next()%360u)*(pi/180):0;
            sim.lightningNext[i]=mul(add(mul(sideways,std::cos(angle)),mul(upward,std::sin(angle))),radius);
            const float yaw=signedHundredths(bound(s.tangentSpread,0,tau),rng);
            const float pitch=signedHundredths(bound(s.tangentSpread,0,tau),rng);
            sim.tangentNext[i]=rotate(rotate(tangents[i],sideways,pitch),upward,yaw);
        }
        // Smoothing is applied to internal controls only; endpoints stay exact.
        if(data.controlPointSmoothing && controls>3) {
            const auto unsmoothed=sim.lightningNext;
            for(std::size_t i=1;i+1<controls;++i)
                sim.lightningNext[i]=mul(add(add(unsmoothed[i-1],mul(unsmoothed[i],2)),unsmoothed[i+1]),0.25f);
        }
        if(first) {sim.lightningPrevious=sim.lightningNext;sim.tangentPrevious=sim.tangentNext;}
        sim.controlAge=0;
    }
    const float blend=delay>0?std::min(1.0f,sim.controlAge/delay):1.0f;
    for(std::size_t i=0;i<controls;++i) {
        base[i]=add(base[i],add(mul(sim.lightningPrevious[i],1-blend),mul(sim.lightningNext[i],blend)));
        if(i && i+1<controls) tangents[i]=add(mul(sim.tangentPrevious[i],1-blend),mul(sim.tangentNext[i],blend));
    }
    const auto branchCapacity=std::min<std::size_t>({static_cast<std::size_t>(data.branchCount),
        maximum/2,static_cast<std::size_t>(1024)});
    const bool branchesRefreshed=controlRefreshed || resized ||
        sim.lightningBranches.size()!=branchCapacity;
    if(branchesRefreshed)
        refreshLightningBranches(data,s,source,rawTarget,direction,sideways,upward,tangents,
            count,maximum,sim,rng);
    const float jitterDelay=bound(s.lightningDelay,0,120);
    const bool refresh=resized || jitterDelay==0 || sim.lightningAge>=jitterDelay;
    if(refresh) {
        refreshLightningOffsets(sim.lightningOffsets,count,distance,sideways,upward,s,rng);
        sim.lightningAge=0;
    }
    // The renderer uses one flattened array, but each child LightningEmitter
    // owns its own particle population in K2. Preserve only the old main prefix
    // as the new main ribbon; never reinterpret a previous child as a resized
    // main ribbon. Child prefixes are restored separately below.
    const auto previousMainCount=sim.lightningMainCount;
    auto previousParticles=std::move(sim.particles);
    auto previousRibbonStarts=std::move(sim.ribbonStarts);
    sim.particles.resize(count);
    const bool motionBlur=lower(data.renderMode)=="motion_blur";
    for(std::size_t i=0;i<count;++i) {
        const bool existing=i<previousMainCount && i<previousParticles.size();
        auto& p=sim.particles[i];
        if(existing) p=previousParticles[i];
        else {p=EmitterParticle{};p.serial=sim.nextSerial++;initialFrame(data,s,p,rng);}
        const float t=static_cast<float>(i)*static_cast<float>(controls-1)/static_cast<float>(count-1);
        const auto segmentIndex=std::min(static_cast<std::size_t>(t),controls-2);
        const float local=t-static_cast<float>(segmentIndex);
        const Vec3 nextPosition=add(cubic(base[segmentIndex],add(base[segmentIndex],tangents[segmentIndex]),
            sub(base[segmentIndex+1],tangents[segmentIndex+1]),base[segmentIndex+1],local),sim.lightningOffsets[i]);
        if(motionBlur) {
            if(existing) {
                const float length=bound(s.blurLength,0,10000);
                const float weight=length>0?std::min(length,dt)/length:1.0f;
                p.tail=add(mul(p.position,weight),mul(p.tail,1-weight));
            } else p.tail=nextPosition;
        }
        p.position=nextPosition;
        // Lightning Initialize forces lifetime 1. Its Update resets the
        // age/frame clocks on equality, not once per elapsed second.
        if(p.age==p.frameOffset) p.age=p.frameOffset=p.lastFrameAge=0;
        p.age+=dt;p.lifetime=1;p.sourceScale=1;p.direction=direction;
    }
    sim.particles.front().position=source;sim.particles.back().position=target;
    sim.ribbonStarts={0};sim.lightningMainCount=count;
    // Child LightningEmitters remain a flat list. Their source tracks the
    // selected main particle each frame; free-fork endpoints persist until
    // UpdateBranches is next invoked, while target branches follow the target.
    for(std::size_t branchIndex=0;branchIndex<sim.lightningActiveBranches &&
        branchIndex<sim.lightningBranches.size() && sim.particles.size()+2<=maximum;++branchIndex) {
        auto& branch=sim.lightningBranches[branchIndex];
        const auto attachment=std::min(branch.attachment,count-1);
        const Vec3 origin=sim.particles[attachment].position;
        const Vec3 destination=branch.targetsMain
            ? add(rawTarget,lightningTargetOffset(s.targetSize,rng)) : branch.target;
        const Vec3 branchDelta=sub(destination,origin);
        const float branchDistance=length(branchDelta);
        if(!std::isfinite(branchDistance)) continue;
        const auto available=maximum-sim.particles.size();
        const auto childCount=std::min(branch.particleCount,available);
        if(childCount<2) continue;
        const Vec3 branchDirection=unit(branchDelta,direction);
        const Vec3 branchSideways=unit(cross(branchDirection,
            std::fabs(branchDirection.z)<0.9f?Vec3{0,0,1}:Vec3{0,1,0}),sideways);
        const Vec3 branchUpward=unit(cross(branchDirection,branchSideways),upward);
        branch.lightningAge+=dt;
        if(branch.offsets.size()!=childCount || jitterDelay==0 || branch.lightningAge>=jitterDelay) {
            refreshLightningOffsets(branch.offsets,childCount,branchDistance,
                branchSideways,branchUpward,s,rng);
            branch.lightningAge=0;
        }
        const auto begin=sim.particles.size();
        const bool restoreChild=!branchesRefreshed && branchIndex+1<previousRibbonStarts.size();
        const auto previousBegin=restoreChild?previousRibbonStarts[branchIndex+1]:previousParticles.size();
        const auto previousEnd=restoreChild
            ? (branchIndex+2<previousRibbonStarts.size()?previousRibbonStarts[branchIndex+2]:previousParticles.size())
            : previousParticles.size();
        const auto previousChildCount=previousEnd>=previousBegin?previousEnd-previousBegin:0;
        sim.ribbonStarts.push_back(begin);sim.particles.resize(begin+childCount);
        for(std::size_t i=0;i<childCount;++i) {
            const bool existing=restoreChild && i<previousChildCount && previousBegin+i<previousParticles.size();
            auto& p=sim.particles[begin+i];
            if(existing) p=previousParticles[previousBegin+i];
            else {p=EmitterParticle{};p.serial=sim.nextSerial++;initialFrame(data,s,p,rng);}
            const float t=static_cast<float>(i)/static_cast<float>(childCount-1);
            const Vec3 nextPosition=add(cubic(origin,add(origin,branch.tangentStart),
                sub(destination,branch.tangentEnd),destination,t),branch.offsets[i]);
            if(motionBlur) {
                if(existing) {
                    const float length=bound(s.blurLength,0,10000);
                    const float weight=length>0?std::min(length,dt)/length:1.0f;
                    p.tail=add(mul(p.position,weight),mul(p.tail,1-weight));
                } else p.tail=nextPosition;
            }
            p.position=nextPosition;
            if(p.age==p.frameOffset) p.age=p.frameOffset=p.lastFrameAge=0;
            p.age+=dt;p.lifetime=1;p.sourceScale=branch.scale;p.direction=branchDirection;
        }
        sim.particles[begin].position=origin;
        sim.particles[begin+childCount-1].position=destination;
    }
    if(std::any_of(sim.particles.begin(),sim.particles.end(),[](const EmitterParticle& p) {
        return !finite(p.position) || std::fabs(p.position.x)>1.0e7f ||
               std::fabs(p.position.y)>1.0e7f || std::fabs(p.position.z)>1.0e7f;
    })) {
        sim.particles.clear();sim.ribbonStarts.clear();sim.lightningReady=false;
        ++sim.invalidEnvironmentSamples;return;
    }
    sim.lightningReady=true;sim.previousWorld=world;sim.accumulator+=dt;
}
} // namespace

void advanceEmitter(const EmitterNodeData& data,const NodeEmitterState& state,
    const Mat4& world,double elapsed,EmitterSimulation& sim,const ParticleLimits& limits,ParticleRandom* shared,const ParticleEnvironment* environment) {
    if(!std::isfinite(elapsed)||elapsed<0) return;
    const auto support=emitterPreviewSupport(data);
    if(!support.supported || !finite(world)) {sim.particles.clear();return;}
    if(!sim.initialized) {sim.previousWorld=world;sim.initialized=true;}
    sim.world=world;
    const auto flags=data.rawWords.back();
    if(environment) {
        const auto tint=environment->objectTint;
        sim.renderTint=(flags&emitter_flags::Tinted) && finite(tint)
            ? Vec3{bound(tint.x,0,1,1),bound(tint.y,0,1,1),bound(tint.z,0,1,1)}
            : Vec3{1,1,1};
        sim.renderAlpha=bound(environment->objectAlpha,0,1,1)*
            bound(environment->tileAlpha,0,1,1);
    } else {sim.renderTint={1,1,1};sim.renderAlpha=1;}
    if(elapsed==0) return; // do not eat an attachment delta during pose-only updates
    const float dt=static_cast<float>(std::min(elapsed,0.25));
    auto& rng=shared ? *shared : sim.random;
    Mat4 previousSource=sim.previousWorld;
    if((flags&emitter_flags::InheritLocal) && environment && finite(environment->ownerTranslationDelta)) {
        previousSource.values[12]+=environment->ownerTranslationDelta.x;
        previousSource.values[13]+=environment->ownerTranslationDelta.y;
        previousSource.values[14]+=environment->ownerTranslationDelta.z;
    }
    const auto maximum=std::min<std::size_t>(limits.maximumParticles,8192u);
    if(support.update==EmitterUpdate::Lightning) {
        advanceLightning(data,state,world,dt,sim,maximum,limits.quality,rng,environment);return;
    }
    if(sim.particles.size()>maximum) sim.particles.resize(maximum);
    const float rate=bound(emitterBirthRateForQuality(data,state,support.update,limits.quality),
        0,bound(limits.maximumBirthRate,0,16384));
    const float life=bound(state.lifeExpectancy,-1,bound(limits.maximumLifetime,0,3600));
    if(support.update==EmitterUpdate::Single && integer(rate)==0) {
        sim.particles.clear();sim.previousWorld=world;sim.accumulator+=dt;return;
    }
    if(support.update!=EmitterUpdate::Single) {
        const auto before=sim.particles.size();
        sim.particles.erase(std::remove_if(sim.particles.begin(),sim.particles.end(),[&](const EmitterParticle& p) {
            return !usable(p) || (life>=0 && p.age>=life);
        }),sim.particles.end());
        sim.freeParticles+=before-sim.particles.size();
    }
    // The standard mover reuses a wind sample for blocks of six when more
    // than 49 particles are alive. Preserve population-index sampling (stopped
    // particles still occupy their indices), not a per-callback counter.
    ParticleEnvironment cachedEnvironment;
    const ParticleEnvironment* activeEnvironment=environment;
    std::size_t movingIndex=0;
    Vec3 cachedWind{};
    if(environment && environment->windDisplacement && sim.particles.size()>49 &&
       !(flags&emitter_flags::PointToPoint)) {
        cachedEnvironment=*environment;
        cachedEnvironment.windDisplacement=[&](Vec3 p,float seconds) {
            if(movingIndex%6==0) {
                const auto sample=environment->windDisplacement(p,seconds);
                if(!finite(sample)) throw ModelError("Particle scene returned non-finite wind displacement");
                cachedWind=sample;
            }
            return cachedWind;
        };
        activeEnvironment=&cachedEnvironment;
    }
    for(auto& p:sim.particles) {
        movingIndex=static_cast<std::size_t>(&p-sim.particles.data());
        p.lifetime=life;p.age+=dt;
        if(support.update==EmitterUpdate::Single) {
            if(life>0 && p.age>life && data.loop) p.age=0;
            else if(life<=0 && 0>p.age) p.age=dt; // binary comparison; negative lifetimes normally continue
        }
        p.rotation+=bound(state.rotation,-10000,10000)*dt;
        if(p.rotation>tau) p.rotation-=tau;
        else if(p.rotation<-tau) p.rotation+=tau;
        if(support.render==EmitterRender::MotionBlur) {
            const float blur=bound(state.blurLength,0,10000);
            const float t=blur>0 ? std::min(blur,dt)/blur : 1.0f;
            p.tail=add(mul(p.position,t),mul(p.tail,1-t));
        }
        const auto before=p;
        try {
            moveEmitterParticle(p,state,sim.previousWorld,world,flags,dt,activeEnvironment);
        } catch (const std::exception&) {
            p=before;++sim.invalidEnvironmentSamples;
        }
        if(!usable(p)) {p=before;++sim.invalidEnvironmentSamples;}
    }
    float sourceScale=bound(length(transformDirection(world,{1,0,0})),0,1000,1);
    // Matrix round-off must not turn an exact integer object scale into a
    // different integer rectangle divisor (e.g. 1000 becomes 999).
    const float rounded=std::round(sourceScale);
    if(std::fabs(sourceScale-rounded)<=1.0e-6f*std::max(1.0f,sourceScale)) sourceScale=rounded;
    const auto spawn=[&](std::optional<Vec3> overridePosition={}) {
        const auto serial=sim.nextSerial++;
        if(sim.particles.size()>=maximum) {++sim.droppedBirths;return;}
        // New Fountain/Explosion particles run the constructor initializer and
        // then the virtual initializer. Recycled objects only run the latter.
        if(support.update!=EmitterUpdate::Single) {
            if(sim.freeParticles) --sim.freeParticles;
            else (void)initializeParticle(data,state,world,previousSource,dt,1.0f,rng,environment);
        }
        const float particleScale=data.chunkName.empty()?sourceScale:1.0f;
        auto p=initializeParticle(data,state,world,previousSource,dt,particleScale,rng,environment);
        p.serial=serial;p.lifetime=life;
        initialFrame(data,state,p,rng);
        if(overridePosition) p.position=*overridePosition;
        if(usable(p)) sim.particles.push_back(p);
    };
    if(support.update==EmitterUpdate::Single) {
        if(sim.particles.empty() && integer(rate)!=0) spawn();
    } else if(support.update==EmitterUpdate::Explosion) {
        const bool high=sim.pendingBursts!=0 || (std::isfinite(state.detonate)&&state.detonate!=0);
        const bool trigger=high && !sim.previousDetonate;
        if(trigger) {++sim.detonations;for(int n=0;n<integer(rate);++n) spawn();}
        sim.pendingBursts=0;
        sim.previousDetonate=high;
    } else if(integer(rate)>0) {
        if(data.spawnType) {
            const auto delta=add(sim.distanceRemainder,sub(position(world),position(previousSource)));
            const float distance=length(delta);const auto direction=unit(delta,{0,0,0});
            const int count=integer(rate*distance);
            const int boundedCount=std::min(count,16384);
            for(int n=0;n<boundedCount;++n) {
                if(state.xSize==0 && state.ySize==0)
                    spawn(sub(position(world),mul(direction,distance-
                        (sim.particles.empty()?0.0f:static_cast<float>(n+1)/rate))));
                else spawn();
            }
            if(count>boundedCount) sim.droppedBirths+=static_cast<std::uint64_t>(count-boundedCount);
            sim.distanceRemainder=sub(delta,mul(direction,static_cast<float>(count)/rate));
        } else {
            sim.birthRemainder+=dt;
            if(sim.birthRemainder>=1/rate) {
                float varied=rate;
                const int variance=std::max(0,integer(state.randomBirthRate));
                if(variance) {
                    const bool negative=rng.next()%2u==0;
                    const auto amount=static_cast<float>(rng.next()%static_cast<unsigned>(variance));
                    varied=std::max(0.0f,varied+(negative?-amount:amount));
                }
                const int divisor=integer(varied)+1;
                const int count=divisor>0 ? integer(sim.birthRemainder*varied)%divisor : 0;
                for(int n=0;n<count;++n) spawn();
                sim.birthRemainder=0; // intentional discarded remainder, not a fractional-birth accumulator
            }
        }
    }
    sim.previousWorld=world;sim.accumulator+=dt;
}
void updateEmitterParticleFrames(const EmitterNodeData& data,const NodeEmitterState& s,
    EmitterSimulation& sim,ParticleRandom* shared) noexcept {
    const float fps=bound(s.framesPerSecond,0,10000);
    if(fps<=0 || s.frameStart==s.frameEnd) return;
    const float interval=1/fps;const int first=integer(s.frameStart),last=integer(s.frameEnd);
    const auto flags=data.rawWords.back();const bool randomInitial=(flags&emitter_flags::RandomFrame)!=0;
    auto& rng=shared ? *shared : sim.random;
    for(auto& p:sim.particles) {
        const float elapsed=p.age-(randomInitial?p.lastFrameAge:p.frameOffset);
        if(!std::isfinite(elapsed) || elapsed<interval) continue;
        if(flags&emitter_flags::RandomOrder) {
            const int span=std::abs(integer(s.frameEnd-s.frameStart))+1;
            p.frame=static_cast<int>(rng.next()%static_cast<unsigned>(span)); // no +first in the engine
        } else {
            int steps=integer(std::floor(elapsed/interval));if(first>last) steps=-steps;
            const auto sum=static_cast<std::int64_t>(p.frame)+steps;
            p.frame=static_cast<int>(std::clamp<std::int64_t>(sum,-10000000,10000000));
            if(randomInitial) {
                p.frameOffset+=static_cast<float>(steps)*interval;
                const int span=std::abs(first-last)+1;
                if(first>last && p.frame<last) p.frame+=((last-p.frame+span-1)/span)*span;
                else if(first<=last && p.frame>last) p.frame-=((p.frame-last+span-1)/span)*span;
            } else if(first>last) {
                // Observed K2 behavior clamps against FIRST, not LAST. Do not
                // "fix" descending non-random tracks into an invented loop.
                if(p.frame<first && data.loop) p.frame=last;
                p.frame=std::max(p.frame,first);
            } else {
                if(p.frame>last && data.loop) p.frame=first;
                p.frame=std::min(p.frame,last);
            }
        }
        if(!randomInitial) p.frameOffset=static_cast<float>(p.frame)*interval;
        if(s.lifeExpectancy<0) p.age=p.lastFrameAge=0;
        else p.lastFrameAge=p.age;
    }
}
ParticleAppearance emitterParticleAppearance(const EmitterNodeData& data,
    const NodeEmitterState& s,const EmitterParticle& p) noexcept {
    ParticleAppearance a;const auto key=segment(s,p);
    for(std::size_t i=0;i<3;++i) a.color[i]=bound(evaluate({s.colorStart[i],s.colorMid[i],s.colorEnd[i]},key),0,1);
    a.color[3]=bound(evaluate({s.alphaStart,s.alphaMid,s.alphaEnd},key),0,1);
    a.size.x=bound(evaluate({s.sizeStart,s.sizeMid,s.sizeEnd},key),0,10000);
    const std::array<float,3> ys{{s.sizeStartY,s.sizeMidY,s.sizeEndY}};
    const bool useX=s.lifeExpectancy<0 ? ys[0]==0&&ys[1]==0&&ys[2]==0 : ys[key.first]==0&&ys[key.second]==0;
    a.size.y=useX ? a.size.x : bound(evaluate(ys,key),0,10000);
    if(data.rawWords.back()&emitter_flags::Splat) {
        a.size.x=bound(p.stopped?s.sizeEnd:s.sizeStart,0,10000);
        const float y=p.stopped?s.sizeEndY:s.sizeStartY;
        a.size.y=y==0?a.size.x:bound(y,0,10000);
    }
    a.columns=std::clamp(data.xGrid,1u,256u);a.rows=std::clamp(data.yGrid,1u,256u);
    a.frame=static_cast<unsigned>(std::clamp(p.frame,0,static_cast<int>(a.columns*a.rows-1)));
    return a;
}
std::array<ParticleFrameSample,5> emitterParticleFrames(const EmitterNodeData& data,
    const NodeEmitterState& s,const EmitterParticle& p) noexcept {
    std::array<ParticleFrameSample,5> result{};
    const auto mode=lower(data.renderMode);
    const bool blend=data.frameBlending && mode!="linked" && mode!="motion_blur";
    result[0]={p.frame,blend?0.5f:1.0f};
    if(!blend) return result;
    const float phase=bound((p.age-p.frameOffset)*bound(s.framesPerSecond,0,10000),0,1);
    const int next=nextFrame(s,p.frame),previous=previousFrame(s,p.frame);
    result[1]={next,next>=0 ? phase*0.5f : 0};
    result[2]={previous,previous>=0 ? 1-phase*0.5f : 0};
    // Invalid first neighbors are still passed through the original helper.
    const int next2=nextFrame(s,next),previous2=previousFrame(s,previous);
    result[3]={next2,next2>=0 ? phase*0.25f : 0};
    result[4]={previous2,previous2>=0 ? (1-phase)*0.25f : 0};
    return result;
}
void buildEmitterMesh(const EmitterNodeData& data,const NodeEmitterState& state,
    const EmitterSimulation& sim,const Mat4& view,Mesh& output,
    std::uint32_t /*textureWidth*/,std::uint32_t /*textureHeight*/,unsigned pass) {
    output.vertices.clear();output.indices.clear();output.castsShadow=false;
    output.hasVertexColors=true;output.render=true;output.diffuse={1,1,1};
    const auto support=emitterPreviewSupport(data);
    if(!finite(view)||!support.supported||!data.chunkName.empty()||pass>4 || (pass && !data.frameBlending)) return;
    const auto count=std::min<std::size_t>(sim.particles.size(),8192);
    output.vertices.reserve(count*4);output.indices.reserve(count*6);
    // Original CPU renderer fills its vertex array backwards. Keep this stable
    // source order instead of sorting particles anew when the camera moves.
    const bool lightning=support.update==EmitterUpdate::Lightning;
    const bool linked=support.render==EmitterRender::Linked;
    const bool splat=(data.rawWords.back()&emitter_flags::Splat)!=0;
    std::size_t previousSegment=std::numeric_limits<std::size_t>::max();
    const bool streak=support.render==EmitterRender::MotionBlur;
    for(std::size_t order=0;order<count;++order) {
        const auto index=(linked || streak)?order:count-order-1;
        if(linked && index+1>=count) break;
        if(linked && lightning && std::binary_search(sim.ribbonStarts.begin(),sim.ribbonStarts.end(),index+1)) continue;
        const auto& p=sim.particles[index];if(!usable(p)) continue;
        auto appearanceState=state;
        if(lightning) appearanceState.lifeExpectancy=1;
        const auto appearance=emitterParticleAppearance(data,appearanceState,p);
        const auto frame=emitterParticleFrames(data,state,p)[pass];
        if(frame.weight<=0 || frame.frame<0 || appearance.color[3]<=0 || appearance.size.x<=0 || appearance.size.y<=0) continue;
        const auto eye=transformPoint(view,p.position);if(!finite(eye)) continue;
        float hx=appearance.size.x*bound(p.sourceScale,0,1000)*0.5f;
        float hy=appearance.size.y*bound(p.sourceScale,0,1000)*0.5f;
        Vec3 right{1,0,0},up{0,1,0};
        if(support.render==EmitterRender::Normal && p.stopped) {
            right=transformDirection(view,p.right);up=transformDirection(view,p.up);
        } else if(support.render==EmitterRender::BillboardWorldZ) {
            right=transformDirection(view,{1,0,0});up=transformDirection(view,{0,1,0});
        } else if(support.render==EmitterRender::BillboardLocalZ) {
            right=transformDirection(view,axis(sim.world,0));up=transformDirection(view,axis(sim.world,1));
        } else if(support.render==EmitterRender::AlignedParticleDirection) {
            right=transformDirection(view,p.right);up=transformDirection(view,p.direction);
        } else if(support.render==EmitterRender::AlignedWorldZ) {
            // Only the UP axis is replaced with world Z. The binary retains
            // the rotated camera-right axis, including its world-Z component.
            right={std::cos(p.rotation),std::sin(p.rotation),0};
            up=transformDirection(view,{0,0,1});
        }
        if(support.render!=EmitterRender::AlignedParticleDirection &&
           support.render!=EmitterRender::AlignedWorldZ && !linked && !streak) {
            const float c=std::cos(p.rotation),sn=std::sin(p.rotation);
            const auto a=right;right=add(mul(a,c),mul(up,sn));up=add(mul(up,c),mul(a,-sn));
        }
        if(splat && !linked && !streak && support.render!=EmitterRender::AlignedWorldZ &&
           support.render!=EmitterRender::AlignedParticleDirection)
            up=cross(transformDirection(view,p.direction),right);
        std::array<Vec3,4> corners;
        float tailAlpha=1;
        if(linked || streak) {
            const auto other=transformPoint(view,linked ? sim.particles[index+1].position : p.tail);
            const auto start=linked?eye:other, finish=linked?other:eye;
            const auto delta=sub(finish,start);
            const float projected=std::sqrt(delta.x*delta.x+delta.y*delta.y);
            const Vec3 along=projected>1e-6f ? Vec3{delta.x/projected,delta.y/projected,0} : Vec3{0,1,0};
            const Vec3 across{along.y,-along.x,0};
            // Linked/motion renderers retain an observable doubled endpoint
            // width for held percentage keys; interpolated spans use half size.
            const auto k=segment(state,p);
            const float held=state.lifeExpectancy>=0 && state.percentStart!=255 && k.first==k.second ? 2.0f : 1.0f;
            hx*=held;hy*=held;
            const auto width=mul(across,2*hx), extension=mul(along,0.5f*hy);
            if(streak) {
                corners={{add(add(finish,width),extension),add(sub(finish,width),extension),
                          sub(sub(start,width),extension),sub(add(start,width),extension)}};
                const float unscaledWidth=appearance.size.x*held;
                tailAlpha=std::min(1.0f,unscaledWidth/(projected>1e-6f?projected:1.0f));
            } else {
                corners={{sub(add(start,width),extension),add(add(finish,width),extension),
                          add(sub(finish,width),extension),sub(sub(start,width),extension)}};
                if(output.vertices.size()>=4 && previousSegment+1==index) {
                    corners[0]=output.vertices[output.vertices.size()-3].position;
                    corners[3]=output.vertices[output.vertices.size()-2].position;
                }
            }
        } else {
            right=mul(right,hx);up=mul(up,hy);
            corners={{sub(sub(eye,right),up),sub(add(eye,right),up),add(add(eye,right),up),add(sub(eye,right),up)}};
        }
        previousSegment=index;
        const int grid=static_cast<int>(appearance.columns*appearance.rows);
        // Invalid authored frames remain finite and do not address arbitrary memory.
        const int f=std::clamp(frame.frame,0,grid-1);
        const float du=1.0f/appearance.columns,dv=1.0f/appearance.rows;
        const float u=static_cast<float>(f%static_cast<int>(appearance.columns))*du;
        const float v=1.0f-static_cast<float>(f/static_cast<int>(appearance.columns)+1)*dv;
        std::array<Vec2,4> uv{{{u,v},{u+du,v},{u+du,v+dv},{u,v+dv}}};
        if(streak) uv={{{u+du,v+dv},{u,v+dv},{u,v},{u+du,v}}};
        if(linked) uv={{{u+du,v},{u+du,v+dv},{u,v+dv},{u,v}}};
        const auto first=static_cast<std::uint32_t>(output.vertices.size());
        for(std::size_t n=0;n<4;++n) {
            Vertex vertex;vertex.position=corners[n];vertex.normal=unit(cross(right,up));vertex.texcoord0=uv[n];
            for(std::size_t c=0;c<4;++c) {
                float value=appearance.color[c];
                if(c<3) value*=c==0?sim.renderTint.x:c==1?sim.renderTint.y:sim.renderTint.z;
                else value*=sim.renderAlpha*frame.weight*(streak && n>=2 ? tailAlpha : 1.0f);
                vertex.color[c]=static_cast<std::uint8_t>(bound(value,0,1)*255.0f); // binary truncation, not round
            }
            output.vertices.push_back(vertex);
        }
        for(const unsigned n:{0u,1u,2u,0u,2u,3u}) output.indices.push_back(first+n);
    }
}
} // namespace neoshared::model
