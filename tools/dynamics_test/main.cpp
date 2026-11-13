#include "engine/core/EngineFeatures.h"
#include "engine/physics/RigidBodyWorld.h"
#if ENGINE_RAGDOLL
#include "engine/physics/RagdollModel.h"
#endif
#if ENGINE_DEFORMATION_CCD
#include "engine/physics/ContinuousDeformation.h"
#endif
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
using namespace DirectX;
namespace {int checks=0;void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class F>void Reject(F f,const char* message){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}Check(rejected,message);}
#if ENGINE_DEFORMATION_CCD
geometry::TriangleMesh Panel(float z){return {{{-2,-2,z},{2,-2,z},{2,2,z},{-2,2,z}},{0,2,1,0,3,2}};}
void Continuous(){
    using physics::SweptSphere;using physics::LimitDeformation;
    auto a=Panel(-10),b=Panel(10);std::vector<SweptSphere> spheres{{{0,0,0},{0,0,0},.25f}};
    float t=LimitDeformation(a,b,spheres);Check(t>.4874f&&t<.48751f,"fast thin plane time of impact");
    Check(LimitDeformation(b,a,spheres)>.4874f&&LimitDeformation(b,a,spheres)<.48751f,"continuous backface");
    Check(LimitDeformation(a,b,{})==1,"empty world allows full deformation");
    spheres[0]={{4,0,0},{4,0,0},.25f};Check(LimitDeformation(a,b,spheres)==1,"swept bounding box miss");
    spheres[0]={{0,0,0},{0,0,5},.25f};t=LimitDeformation(a,b,spheres);Check(t>.6499f&&t<.6501f,"relative sphere motion");
    spheres[0]={{0,0,0},{0,0,0},.25f};auto touching=Panel(-.25f);
    Check(LimitDeformation(touching,a,spheres)==1,"separating initial contact");
    Check(LimitDeformation(touching,b,spheres)==0,"approaching initial contact");
    auto deform=b;deform.positions[0].z=-10;deform.positions[1].z=-10;
    t=LimitDeformation(a,deform,spheres);Check(t<1&&t>0,"nonrigid triangle sweep");
    geometry::TriangleMesh split{{{-3,-2,-10},{-1,-2,-10},{-1,2,-10},{-3,2,-10},{1,-2,-10},{3,-2,-10},{3,2,-10},{1,2,-10}},{0,2,1,0,3,2,4,6,5,4,7,6}};
    auto moved=split;for(auto& p:moved.positions)p.z=10;Check(LimitDeformation(split,moved,spheres)==1,"continuous hole stays open");
    auto bad=b;bad.indices[0]=1;Reject([&]{LimitDeformation(a,bad,spheres);},"reject continuous topology change");
    spheres[0].radius=std::numeric_limits<float>::quiet_NaN();Reject([&]{LimitDeformation(a,b,spheres);},"reject invalid continuous sphere");
    physics::RigidBodyWorld world;auto id=world.DeformingMesh(a,{0,0,0});world.Box({0,0,0},{.5f,.5f,.5f},1);
    auto limited=world.ConstrainDeformation(id,b,{0,0,0},{0,0,0,1},1.f/120,{100,100,100},{.3f,.9f,.3f});
    Check(limited.positions[0].z<-.43f&&limited.positions[0].z>-.45f,"world bounds limit crossing");
    world.UpdateDeformingMesh(id,limited,{0,0,0});float distance;
    Check(world.RayDeformingMesh(id,{0,0,-5},{0,0,1},10,distance)&&distance<4.57f,"limited geometry installed");
    Reject([&]{world.ConstrainDeformation(id,b,{0,0,0},{0,1,0,0},.1f,{0,0,0},{1,1,1});},"reject unmodeled root rotation");
    // Player participates even when it is not a Jolt dynamic body.
    physics::RigidBodyWorld playerWorld;id=playerWorld.DeformingMesh(a,{0,0,0});
    limited=playerWorld.ConstrainDeformation(id,b,{0,0,0},{0,0,0,1},1.f/120,{0,0,0},{.1f,.1f,.1f});
    Check(limited.positions[0].z<-.173f,"stationary player protected from moving plane");
    std::cout<<"CCD CHECK OK\n";
}
#endif
#if ENGINE_RAGDOLL
void Ragdoll(const char* meshPath,const char* animationPath,const char* rigPath){
    auto mesh=geometry::TriangleMesh::Load(meshPath);auto animation=geometry::SkeletalAnimation::Load(animationPath,mesh);
#if ENGINE_DEFORMATION_CCD
    Check(std::abs(animation.TimeToNextKey(0)-1.f/60)<1e-6f,"first animation segment boundary");
    Check(std::abs(animation.TimeToNextKey(.012f)-(1.f/60-.012f))<1e-6f,"split before baked key boundary");
    Check(std::abs(animation.TimeToNextKey(2)-1.f/60)<1e-6f,"loop segment boundary");
#endif
    auto bones=physics::RagdollModel::Load(rigPath,static_cast<uint32_t>(animation.InitialPalette().size()));
    physics::RigidBodyWorld world;world.Box({0,-.5f,0},{30,1,30});
    physics::RagdollModel rag(world,animation,bones,{0,4,0});auto pose=rag.Pose();
    Check(pose.positions.size()==mesh.positions.size(),"ragdoll geometry count");
    for(size_t i=0;i<pose.positions.size();++i){auto a=pose.positions[i],b=mesh.positions[i];Check(std::abs(a.x-b.x)<1e-5f&&std::abs(a.y-b.y-4)<1e-5f&&std::abs(a.z-b.z)<1e-5f,"initial ragdoll skin bind");}
    auto first=world.Get(rag.Bodies()[0]).position;world.Step(.1f);Check(world.Get(rag.Bodies()[0]).position.y<first.y,"ragdoll gravity");
    for(int i=0;i<600;++i)world.Step(1.f/120);Check(rag.MaxJointError()<.05f,"ragdoll joints remain connected");
    for(auto id:rag.Bodies()){auto p=world.Get(id);Check(p.position.y>-.01f&&p.position.y<4,"ragdoll ground support");}
    auto id=rag.Bodies().back();auto p=world.Get(id).position;p.y+=.4f;world.Impulse(id,{3,3,1},p);
    float rotation=0,relativeRotation=0;for(int i=0;i<180;++i){world.Step(1.f/120);auto q=world.Get(id).rotation;rotation=std::max(rotation,std::abs(q.x)+std::abs(q.y)+std::abs(q.z));rag.Pose().Validate();
        for(size_t j=0;j<bones.size();++j)if(bones[j].parent>=0){auto a=world.Get(rag.Bodies()[j]).rotation,b=world.Get(rag.Bodies()[bones[j].parent]).rotation;
            float dot=std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w);float angle=2*std::acos(std::min(1.f,dot));
            relativeRotation=std::max(relativeRotation,angle);Check(angle<1.1f,"swing/twist joint angular limits");}
    }
    Check(rotation>.01f&&relativeRotation>.01f,"ragdoll angular impulse deforms skin");Check(rag.MaxJointError()<.08f,"joint constraints after impulse");
    auto bad=bones;bad[0].parent=0;Reject([&]{physics::RagdollModel invalid(world,animation,bad,{0,0,0});},"reject rig cycle before body creation");
    bad=bones;bad[0].size.x=-1;Reject([&]{physics::RagdollModel invalid(world,animation,bad,{0,0,0});},"reject invalid rig proxy");
    std::cout<<"RAGDOLL CHECK OK bones="<<bones.size()<<" joint-error="<<rag.MaxJointError()<<"\n";
}
#endif
}
int main(int argc,char** argv){try{
#if ENGINE_DEFORMATION_CCD
    Continuous();
#endif
#if ENGINE_RAGDOLL
    if(argc!=4)throw std::runtime_error("mesh anim rig arguments required");Ragdoll(argv[1],argv[2],argv[3]);
#endif
    std::cout<<"DYNAMICS CHECK OK checks="<<checks<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
