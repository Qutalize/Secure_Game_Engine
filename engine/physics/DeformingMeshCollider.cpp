#include "engine/physics/DeformingMeshCollider.h"
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace physics::deforming {
using namespace JPH;
namespace {
void Require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
bool Finite(DirectX::XMFLOAT3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
Vec3 V(DirectX::XMFLOAT3 v){return {v.x,v.y,v.z};}
struct OnlyBody final:BodyFilter {
    BodyID id;explicit OnlyBody(BodyID value):id(value){}
    bool ShouldCollide(const BodyID& other)const override{return id==other;}
};
}
void ValidatePose(DirectX::XMFLOAT3 p,DirectX::XMFLOAT4 q){
    float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    Require(Finite(p)&&std::isfinite(norm)&&std::abs(norm-1)<1e-4f,"Invalid deforming collider pose");
}
RefConst<Shape> Cook(const geometry::TriangleMesh& mesh){
    mesh.Validate();Require(mesh.indices.size()/3<=4096,"Deforming collision exceeds 4096 triangles");
    TriangleList triangles;triangles.reserve(mesh.indices.size()/3);
    for(size_t i=0;i<mesh.indices.size();i+=3)
        triangles.emplace_back(V(mesh.positions[mesh.indices[i]]),V(mesh.positions[mesh.indices[i+1]]),V(mesh.positions[mesh.indices[i+2]]));
    MeshShapeSettings settings(triangles);auto cooked=settings.Create();
    if(cooked.HasError())throw std::runtime_error("Jolt deforming model: "+std::string(cooked.GetError().c_str()));
    return cooked.Get();
}
void Replace(PhysicsSystem& world,BodyID id,const Shape* shape,DirectX::XMFLOAT3 p,DirectX::XMFLOAT4 q){
    auto& bodies=world.GetBodyInterface();
    auto bounds=bodies.GetTransformedShape(id).GetWorldSpaceBounds();
    // Immutable replacement invalidates contacts and updates the broad phase.
    bodies.SetShape(id,shape,false,EActivation::DontActivate);
    bodies.SetPositionAndRotation(id,RVec3(p.x,p.y,p.z),Quat(q.x,q.y,q.z,q.w),EActivation::DontActivate);
    bounds.Encapsulate(bodies.GetTransformedShape(id).GetWorldSpaceBounds());bounds.ExpandBy(Vec3::sReplicate(.05f));
    // Static bodies cannot wake themselves. Wake supported/overlapped sleeping bodies explicitly.
    world.GetBodyInterface().ActivateBodiesInAABox(bounds,BroadPhaseLayerFilter{},ObjectLayerFilter{});
}
bool Ray(const PhysicsSystem& world,BodyID id,DirectX::XMFLOAT3 origin,DirectX::XMFLOAT3 direction,float maxDistance,float& distance){
    Require(Finite(origin)&&Finite(direction)&&std::isfinite(maxDistance)&&maxDistance>0&&maxDistance<=100000,"Invalid deforming ray");
    const float length=V(direction).Length();Require(std::isfinite(length)&&length>1e-8f,"Invalid deforming ray direction");
    RayCastSettings settings;settings.SetBackFaceMode(EBackFaceMode::CollideWithBackFaces);
    ClosestHitCollisionCollector<CastRayCollector> hits;
    world.GetNarrowPhaseQuery().CastRay(RRayCast(RVec3(origin.x,origin.y,origin.z),V(direction)*(maxDistance/length)),
                                      settings,hits,{}, {},OnlyBody(id));
    if(!hits.HadHit())return false;
    distance=hits.mHit.mFraction*maxDistance;return true;
}
DirectX::XMFLOAT3 Move(const PhysicsSystem& world,BodyID id,DirectX::XMFLOAT3 center,DirectX::XMFLOAT3 halfExtent,DirectX::XMFLOAT3 delta){
    Require(Finite(center)&&Finite(halfExtent)&&Finite(delta)&&halfExtent.x>0&&halfExtent.y>0&&halfExtent.z>0,"Invalid deforming player query");
    BoxShape box(V(halfExtent),0);box.SetEmbedded();
    auto target=world.GetBodyInterface().GetTransformedShape(id);
    RVec3 position(center.x,center.y,center.z);Vec3 remaining=V(delta);
    constexpr float skin=.001f;
    CollideShapeSettings overlap;overlap.mBackFaceMode=EBackFaceMode::CollideWithBackFaces;
    for(int i=0;i<8;++i){
        ClosestHitCollisionCollector<CollideShapeCollector> hits;
        target.CollideShape(&box,Vec3::sReplicate(1),RMat44::sTranslation(position),overlap,RVec3::sZero(),hits);
        if(!hits.HadHit()||hits.mHit.mPenetrationDepth<=0)break;
        auto axis=hits.mHit.mPenetrationAxis.NormalizedOr(Vec3::sZero());
        position-=axis*(hits.mHit.mPenetrationDepth+skin);
    }
    ShapeCastSettings cast;cast.SetBackFaceMode(EBackFaceMode::CollideWithBackFaces);cast.mReturnDeepestPoint=true;
    for(int i=0;i<4&&remaining.LengthSq()>1e-12f;++i){
        AllHitCollisionCollector<CastShapeCollector> hits;
        target.CastShape(RShapeCast(&box,Vec3::sReplicate(1),RMat44::sTranslation(position),remaining),cast,RVec3::sZero(),hits);
        const ShapeCastResult* first=nullptr;
        for(const auto& hit:hits.mHits){
            if(remaining.Dot(hit.mPenetrationAxis)<=1e-8f)continue; // Allow separating/tangent movement.
            if(!first||hit.mFraction<first->mFraction)first=&hit;
        }
        if(!first){position+=remaining;break;}
        float fraction=std::max(0.f,first->mFraction-skin/remaining.Length());
        position+=remaining*fraction;remaining*=1-fraction;
        auto axis=first->mPenetrationAxis.NormalizedOr(Vec3::sZero());
        remaining-=axis*std::max(0.f,remaining.Dot(axis));
    }
    Require(std::isfinite(position.GetX())&&std::isfinite(position.GetY())&&std::isfinite(position.GetZ()),"Invalid deforming player result");
    return {position.GetX(),position.GetY(),position.GetZ()};
}
}
