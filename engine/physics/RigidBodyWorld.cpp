#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include "engine/physics/RigidBodyWorld.h"
#include "engine/core/Logger.h"
#if ENGINE_MODEL_CONVEX
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#endif
#if ENGINE_MODEL_STATIC
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#endif
#if ENGINE_MODEL_DEFORMING
#include "engine/physics/DeformingMeshCollider.h"
#include <cstring>
#endif
#if ENGINE_RAGDOLL
#include "engine/physics/RagdollModel.h"
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#endif
#if ENGINE_DEFORMATION_CCD
#include "engine/physics/ContinuousDeformation.h"
#endif
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
namespace physics {
using namespace JPH;
namespace {
struct Registration {
    Registration(){ RegisterDefaultAllocator(); Factory::sInstance=new Factory;RegisterTypes(); }
    ~Registration(){UnregisterTypes();delete Factory::sInstance;Factory::sInstance=nullptr;}
};
struct Layers : BroadPhaseLayerInterface {
    uint GetNumBroadPhaseLayers()const override{return 2;}
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l)const override{return BroadPhaseLayer(static_cast<uint8>(l));}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer l)const override{return l.GetValue()==0?"static":"dynamic";}
#endif
};
struct Pairs : ObjectLayerPairFilter {
    bool ShouldCollide(ObjectLayer a,ObjectLayer b)const override{return a!=0 || b!=0;}
};
struct BroadPairs : ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(ObjectLayer a,BroadPhaseLayer b)const override{return a!=0 || b.GetValue()!=0;}
};
bool finite(DirectX::XMFLOAT3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
Vec3 V(DirectX::XMFLOAT3 v){return {v.x,v.y,v.z};}
DirectX::XMFLOAT3 F(Vec3Arg v){return {v.GetX(),v.GetY(),v.GetZ()};}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
}
struct RigidBodyWorld::Impl {
#if ENGINE_RAGDOLL
    std::vector<Ref<Constraint>> ragdollConstraints;
#endif
    Layers layers;Pairs pairs;BroadPairs broad;
    TempAllocatorImpl allocator{4*1024*1024};
    JobSystemSingleThreaded jobs{1024};PhysicsSystem system;
    std::vector<BodyID> ids;std::vector<DirectX::XMFLOAT3> sizes;
#if ENGINE_MODEL_DEFORMING
    struct Deformation { uint32_t id;geometry::TriangleMesh mesh;DirectX::XMFLOAT3 position;DirectX::XMFLOAT4 rotation; };
    std::vector<Deformation> deformations;
    Deformation& Deformed(uint32_t id){
        for(auto& d:deformations)if(d.id==id)return d;
        throw std::runtime_error("Body is not a deforming mesh");
    }
#endif
    uint32_t Add(const Shape* shape,DirectX::XMFLOAT3 center,DirectX::XMFLOAT3 size,float mass,float restitution,float friction){
        require(finite(center)&&std::isfinite(mass)&&mass>=0&&std::isfinite(restitution)&&restitution>=0&&restitution<=1&&std::isfinite(friction)&&friction>=0,"Invalid model body settings");
        require(ids.size()<128,"Rigid body capacity reached");
        BodyCreationSettings settings(shape,RVec3(center.x,center.y,center.z),Quat::sIdentity(),mass>0?EMotionType::Dynamic:EMotionType::Static,mass>0?1:0);
        settings.mRestitution=restitution;settings.mFriction=friction;
        if(mass>0){settings.mOverrideMassProperties=EOverrideMassProperties::CalculateInertia;settings.mMassPropertiesOverride.mMass=mass;settings.mMotionQuality=EMotionQuality::LinearCast;}
        auto id=system.GetBodyInterface().CreateAndAddBody(settings,mass>0?EActivation::Activate:EActivation::DontActivate);
        require(!id.IsInvalid(),"Jolt body allocation failed");ids.push_back(id);sizes.push_back(size);return static_cast<uint32_t>(ids.size()-1);
    }
    Impl(){system.Init(128,0,1024,1024,layers,broad,pairs);}
    ~Impl(){
#if ENGINE_RAGDOLL
        for(auto& constraint:ragdollConstraints)system.RemoveConstraint(constraint);
        ragdollConstraints.clear();
#endif
        for(auto id:ids){system.GetBodyInterface().RemoveBody(id);system.GetBodyInterface().DestroyBody(id);}}
};
RigidBodyWorld::RigidBodyWorld(){static Registration registry;m=std::make_unique<Impl>();}
RigidBodyWorld::~RigidBodyWorld()=default;
uint32_t RigidBodyWorld::Box(DirectX::XMFLOAT3 center,DirectX::XMFLOAT3 size,float mass,float restitution,float friction){
    require(finite(center)&&finite(size)&&size.x>0&&size.y>0&&size.z>0,"Invalid box geometry");
    require(std::isfinite(mass)&&mass>=0&&std::isfinite(restitution)&&restitution>=0&&restitution<=1&&std::isfinite(friction)&&friction>=0,"Invalid rigid body material");
    require(m->ids.size()<128,"Rigid body capacity reached");
    BoxShapeSettings shape(V(size)*.5f, std::min({size.x,size.y,size.z})*.05f);
    auto cooked=shape.Create();require(!cooked.HasError(),"Jolt box creation failed");
    return m->Add(cooked.Get(),center,size,mass,restitution,friction);
}
#if ENGINE_MODEL_CONVEX
uint32_t RigidBodyWorld::ConvexHull(const geometry::TriangleMesh& mesh,DirectX::XMFLOAT3 position,float mass,float restitution,float friction){
    mesh.Validate();require(mass>0,"Convex model requires positive mass");
    std::vector<bool> used(mesh.positions.size());for(auto index:mesh.indices)used[index]=true;
    Array<Vec3> points;for(size_t i=0;i<used.size();++i)if(used[i])points.push_back(V(mesh.positions[i]));
    require(points.size()>=4&&points.size()<=4096,"Convex model requires 4..4096 referenced points");
    ConvexHullShapeSettings settings(points,0);auto result=settings.Create();
    if(result.HasError())throw std::runtime_error("Jolt convex model: "+std::string(result.GetError().c_str()));
    auto bounds=result.Get()->GetLocalBounds().GetSize();
    require(bounds.GetX()>1e-5f&&bounds.GetY()>1e-5f&&bounds.GetZ()>1e-5f&&result.Get()->GetVolume()>1e-9f,"Degenerate convex volume");
    LOG_INFO("MODEL PHYSICS convex-hull points=%zu",points.size());
    return m->Add(result.Get(),position,{1,1,1},mass,restitution,friction);
}
#endif
#if ENGINE_MODEL_STATIC
uint32_t RigidBodyWorld::StaticMesh(const geometry::TriangleMesh& mesh,DirectX::XMFLOAT3 position,float friction){
    mesh.Validate();TriangleList triangles;triangles.reserve(mesh.indices.size()/3);
    for(size_t i=0;i<mesh.indices.size();i+=3)triangles.emplace_back(V(mesh.positions[mesh.indices[i]]),V(mesh.positions[mesh.indices[i+1]]),V(mesh.positions[mesh.indices[i+2]]));
    MeshShapeSettings settings(triangles);auto result=settings.Create();
    if(result.HasError())throw std::runtime_error("Jolt static model: "+std::string(result.GetError().c_str()));
    LOG_INFO("MODEL PHYSICS static-mesh triangles=%zu",triangles.size());
    return m->Add(result.Get(),position,{1,1,1},0,.2f,friction);
}
#endif
#if ENGINE_MODEL_DEFORMING
uint32_t RigidBodyWorld::DeformingMesh(const geometry::TriangleMesh& mesh,DirectX::XMFLOAT3 position,DirectX::XMFLOAT4 rotation,float friction){
    deforming::ValidatePose(position,rotation);auto shape=deforming::Cook(mesh);
    // Allocate bookkeeping before adding a body so allocation failure leaves the world unchanged.
    Impl::Deformation state{0,mesh,position,rotation};m->deformations.reserve(m->deformations.size()+1);
    auto id=m->Add(shape,position,{1,1,1},0,0,friction);state.id=id;
    m->system.GetBodyInterface().SetRotation(m->ids.at(id),Quat(rotation.x,rotation.y,rotation.z,rotation.w),EActivation::DontActivate);
    m->deformations.push_back(std::move(state));
    LOG_INFO("MODEL PHYSICS deforming-mesh triangles=%zu",mesh.indices.size()/3);return id;
}
void RigidBodyWorld::UpdateDeformingMesh(uint32_t id,const geometry::TriangleMesh& mesh,DirectX::XMFLOAT3 position,DirectX::XMFLOAT4 rotation){
    auto& state=m->Deformed(id);deforming::ValidatePose(position,rotation);
    require(mesh.positions.size()==state.mesh.positions.size()&&mesh.indices==state.mesh.indices,"Deforming collision topology changed");
    bool verticesChanged=std::memcmp(mesh.positions.data(),state.mesh.positions.data(),mesh.positions.size()*sizeof(DirectX::XMFLOAT3))!=0;
    bool poseChanged=std::memcmp(&position,&state.position,sizeof(position))!=0||std::memcmp(&rotation,&state.rotation,sizeof(rotation))!=0;
    if(!verticesChanged&&!poseChanged)return;
    // All validation/allocation precedes mutation; invalid poses keep the last valid collider.
    RefConst<Shape> shape=verticesChanged?deforming::Cook(mesh):m->system.GetBodyInterface().GetShape(m->ids.at(id));
    auto next=mesh;deforming::Replace(m->system,m->ids.at(id),shape,position,rotation);
    state.mesh=std::move(next);state.position=position;state.rotation=rotation;
}
bool RigidBodyWorld::RayDeformingMesh(uint32_t id,DirectX::XMFLOAT3 origin,DirectX::XMFLOAT3 direction,float maxDistance,float& distance)const{
    m->Deformed(id);return deforming::Ray(m->system,m->ids.at(id),origin,direction,maxDistance,distance);
}
DirectX::XMFLOAT3 RigidBodyWorld::MoveAgainstDeformingMesh(uint32_t id,DirectX::XMFLOAT3 center,DirectX::XMFLOAT3 halfExtent,DirectX::XMFLOAT3 delta)const{
    m->Deformed(id);return deforming::Move(m->system,m->ids.at(id),center,halfExtent,delta);
}
bool RigidBodyWorld::IsActive(uint32_t id)const{return m->system.GetBodyInterface().IsActive(m->ids.at(id));}
#endif
#if ENGINE_RAGDOLL
void RigidBodyWorld::ConnectRagdoll(std::span<const uint32_t> ids,std::span<const RagdollBone> bones,DirectX::XMFLOAT3 origin){
    require(ids.size()==bones.size()&&ids.size()>=2&&ids.size()<=32&&finite(origin),"Invalid ragdoll connection count/origin");
    auto& bodies=m->system.GetBodyInterface();
    Ref<GroupFilterTable> filter=new GroupFilterTable(static_cast<uint32_t>(ids.size()));
    std::vector<Ref<Constraint>> staged;
    for(size_t i=0;i<ids.size();++i){
        require(bodies.GetMotionType(m->ids.at(ids[i]))==EMotionType::Dynamic,"Ragdoll requires dynamic bodies");
        for(size_t j=0;j<i;++j){require(ids[i]!=ids[j],"Duplicate ragdoll body");filter->DisableCollision(static_cast<uint32_t>(i),static_cast<uint32_t>(j));}
        int parent=bones[i].parent;require(parent>=-1&&parent<int(ids.size())&&parent!=int(i),"Invalid ragdoll parent");
        if(parent<0)continue;
        require(finite(bones[i].anchor),"Invalid ragdoll joint position");
        auto a=bones[i].anchor;SwingTwistConstraintSettings settings;
        settings.mPosition1=settings.mPosition2=RVec3(a.x+origin.x,a.y+origin.y,a.z+origin.z);
        settings.mTwistAxis1=settings.mTwistAxis2=Vec3::sAxisY();settings.mPlaneAxis1=settings.mPlaneAxis2=Vec3::sAxisX();
        settings.mNormalHalfConeAngle=settings.mPlaneHalfConeAngle=JPH_PI/4;
        settings.mTwistMinAngle=-JPH_PI/6;settings.mTwistMaxAngle=JPH_PI/6;settings.mMaxFrictionTorque=.05f;
        Ref<Constraint> constraint=bodies.CreateConstraint(&settings,m->ids.at(ids[parent]),m->ids.at(ids[i]));
        require(constraint!=nullptr,"Ragdoll constraint allocation failed");staged.push_back(constraint);
    }
    m->ragdollConstraints.reserve(m->ragdollConstraints.size()+staged.size());
    for(size_t i=0;i<ids.size();++i)bodies.SetCollisionGroup(m->ids.at(ids[i]),CollisionGroup(filter,ids[0],static_cast<uint32_t>(i)));
    for(auto& c:staged){m->system.AddConstraint(c);m->ragdollConstraints.push_back(c);}
}
#endif
#if ENGINE_DEFORMATION_CCD
geometry::TriangleMesh RigidBodyWorld::ConstrainDeformation(uint32_t id,const geometry::TriangleMesh& desired,DirectX::XMFLOAT3 position,
    DirectX::XMFLOAT4 rotation,float seconds,DirectX::XMFLOAT3 playerCenter,DirectX::XMFLOAT3 half)const{
    using namespace DirectX;
    const auto& previous=m->Deformed(id);deforming::ValidatePose(position,rotation);
    require(std::isfinite(seconds)&&seconds>0&&seconds<=1&&finite(playerCenter)&&finite(half)&&half.x>0&&half.y>0&&half.z>0,"Invalid continuous deformation step");
    require(std::memcmp(&rotation,&previous.rotation,sizeof(rotation))==0,"Continuous root rotation requires a vertex trajectory; bake rotation into vertices");
    auto world=[](const geometry::TriangleMesh& mesh,XMFLOAT3 p,XMFLOAT4 q){auto result=mesh;
        auto matrix=XMMatrixRotationQuaternion(XMLoadFloat4(&q))*XMMatrixTranslation(p.x,p.y,p.z);
        for(auto& v:result.positions)XMStoreFloat3(&v,XMVector3TransformCoord(XMLoadFloat3(&v),matrix));return result;};
    auto before=world(previous.mesh,previous.position,previous.rotation),after=world(desired,position,rotation);
    std::vector<SweptSphere> spheres;auto& bodies=m->system.GetBodyInterface();
    for(auto body:m->ids){if(bodies.GetMotionType(body)!=EMotionType::Dynamic)continue;
        auto p=bodies.GetCenterOfMassPosition(body);auto shape=bodies.GetShape(body);auto bounds=shape->GetLocalBounds();
        float radius=bounds.GetExtent().Length()+bounds.GetCenter().Length();auto gravity=m->system.GetGravity()*bodies.GetGravityFactor(body);
        auto delta=bodies.GetLinearVelocity(body)*seconds+gravity*(.5f*seconds*seconds);
        // Enclose the entire predicted path in one stationary sphere. Animation key segments
        // can then be traced independently without incorrectly restarting the body's time.
        radius+=.5f*delta.Length()+gravity.Length()*(seconds*seconds/8);
        XMFLOAT3 center{p.GetX()+delta.GetX()*.5f,p.GetY()+delta.GetY()*.5f,p.GetZ()+delta.GetZ()*.5f};
        spheres.push_back({center,center,radius});
    }
    spheres.push_back({playerCenter,playerCenter,XMVectorGetX(XMVector3Length(XMLoadFloat3(&half)))});
    float fraction=LimitDeformation(before,after,spheres);
    if(fraction==1)return desired;
    auto inverse=XMMatrixInverse(nullptr,XMMatrixRotationQuaternion(XMLoadFloat4(&rotation))*XMMatrixTranslation(position.x,position.y,position.z));
    for(size_t i=0;i<after.positions.size();++i){auto v=XMVectorLerp(XMLoadFloat3(&before.positions[i]),XMLoadFloat3(&after.positions[i]),fraction);XMStoreFloat3(&after.positions[i],XMVector3TransformCoord(v,inverse));}
    after.Validate();return after;
}
#endif
void RigidBodyWorld::Step(float seconds){
    require(std::isfinite(seconds)&&seconds>0&&seconds<=1,"Rigid body step must be in (0,1]");
    int steps=std::max(1,static_cast<int>(std::ceil(seconds*120)));
    auto error=m->system.Update(seconds,steps,&m->allocator,&m->jobs);
    require(error==EPhysicsUpdateError::None,"Jolt simulation capacity error");
}
void RigidBodyWorld::Impulse(uint32_t id,DirectX::XMFLOAT3 impulse,DirectX::XMFLOAT3 point){
    require(finite(impulse)&&finite(point),"Invalid impulse");
    m->system.GetBodyInterface().AddImpulse(m->ids.at(id),V(impulse),RVec3(point.x,point.y,point.z));
}
Pose RigidBodyWorld::Get(uint32_t id)const{
    auto& b=m->system.GetBodyInterface();RVec3 p;Quat q;b.GetPositionAndRotation(m->ids.at(id),p,q);
    return {{p.GetX(),p.GetY(),p.GetZ()},m->sizes.at(id),{q.GetX(),q.GetY(),q.GetZ(),q.GetW()},F(b.GetLinearVelocity(m->ids.at(id))),F(b.GetAngularVelocity(m->ids.at(id)))};
}
void CheckRigidBodies(){
    RigidBodyWorld world;world.Box({0,-.5f,0},{40,1,40});
    auto box=world.Box({-4,3,0},{1,1,1},1,0,.8f);
    world.Step(.1f);require(world.Get(box).position.y<3,"Rigid gravity failed");
    for(int i=0;i<600;++i)world.Step(1.f/120);
    require(std::abs(world.Get(box).position.y-.5f)<.04f,"Rigid ground support failed");
    world.Impulse(box,{3,0,0},world.Get(box).position);
    for(int i=0;i<360;++i)world.Step(1.f/120);
    require(std::abs(world.Get(box).velocity.x)<.1f,"Rigid friction failed");
    auto bounce=world.Box({4,2,0},{1,1,1},1,.9f,0);bool rebounded=false;
    for(int i=0;i<180;++i){world.Step(1.f/120);rebounded|=world.Get(bounce).velocity.y>1.f;}
    require(rebounded,"Rigid restitution failed");
    auto spin=world.Box({0,6,4},{1,1,1},2);world.Impulse(spin,{2,0,0},{0,6.5f,4});world.Step(.01f);
    require(std::abs(world.Get(spin).angularVelocity.z)>.2f,"Rigid angular impulse failed");
    auto bottom=world.Box({-8,.55f,0},{1,1,1},1,0);auto top=world.Box({-8,3,0},{1,1,1},1,0);
    for(int i=0;i<600;++i)world.Step(1.f/120);
    require(world.Get(bottom).position.y>.45f&&world.Get(top).position.y>1.35f,"Rigid dynamic contact/stack failed");
    LOG_INFO("JOLT CHECK OK: gravity, ground, friction, restitution, angular impulse, dynamic contact");
}
#if ENGINE_MODEL_CONVEX || ENGINE_MODEL_STATIC
void CheckModelBodies(){
#if ENGINE_MODEL_CONVEX
    geometry::TriangleMesh tetra;tetra.positions={{-1,0,-1},{1,0,-1},{0,0,1},{0,2,0}};tetra.indices={0,1,3,1,2,3,2,0,3,0,2,1};
    RigidBodyWorld world;world.Box({0,-.5f,0},{20,1,20});auto id=world.ConvexHull(tetra,{0,3,0},2,0);
    for(int i=0;i<600;++i)world.Step(1.f/120);
    auto pose=world.Get(id);float lowest=100;
    auto transform=DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&pose.rotation))*DirectX::XMMatrixTranslation(pose.position.x,pose.position.y,pose.position.z);
    for(auto v:tetra.positions)lowest=std::min(lowest,DirectX::XMVectorGetY(DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&v),transform)));
    require(std::abs(lowest)<.06f,"Model convex ground contact/origin failed");
    auto point=pose.position;point.y+=1;world.Impulse(id,{3,0,0},point);world.Step(.01f);
    require(std::abs(world.Get(id).angularVelocity.z)>.1f,"Model convex angular response failed");
#endif
#if ENGINE_MODEL_STATIC
    geometry::TriangleMesh split;split.positions={{-3,0,-2},{-3,0,2},{-1,0,2},{-1,0,-2},{1,0,-2},{1,0,2},{3,0,2},{3,0,-2}};split.indices={0,1,2,0,2,3,4,5,6,4,6,7};
    RigidBodyWorld world;world.StaticMesh(split,{0,0,0});
    auto supported=world.Box({-2,3,0},{.5f,.5f,.5f},1,0);auto hole=world.Box({0,3,0},{.5f,.5f,.5f},1,0);
    for(int i=0;i<240;++i)world.Step(1.f/120);
    require(std::abs(world.Get(supported).position.y-.25f)<.06f,"Static mesh support failed");
    require(world.Get(hole).position.y < -5,"Concave mesh opening was incorrectly filled");
#endif
    LOG_INFO("MODEL PHYSICS CHECK OK");
}
#endif

}
