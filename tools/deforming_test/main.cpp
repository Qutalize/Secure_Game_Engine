#include "engine/physics/RigidBodyWorld.h"
#include "engine/geometry/SkeletalAnimation.h"
#include <cmath>
#include <limits>
#include <iostream>
#include <stdexcept>
using namespace DirectX;
using physics::RigidBodyWorld;
using geometry::TriangleMesh;
namespace {
int checks=0;
void Check(bool ok,const char* label){if(!ok)throw std::runtime_error(label);++checks;}
template<class F>void Reject(F action,const char* label){bool failed=false;try{action();}catch(const std::exception&){failed=true;}Check(failed,label);}
TriangleMesh Panel(){return {{{-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0}},{0,2,1,0,3,2}};}
TriangleMesh Split(){return {{{-3,0,-2},{-3,0,2},{-1,0,2},{-1,0,-2},{1,0,-2},{1,0,2},{3,0,2},{3,0,-2}},{0,1,2,0,2,3,4,5,6,4,6,7}};}
bool Hit(RigidBodyWorld& w,uint32_t id,XMFLOAT3 p,XMFLOAT3 d,float* result=nullptr){float t;bool hit=w.RayDeformingMesh(id,p,d,100,t);if(result)*result=t;return hit;}
void Queries(){
    RigidBodyWorld w;auto panel=Panel();auto id=w.DeformingMesh(panel,{0,0,0});float t;
    Check(Hit(w,id,{0,0,-3},{0,0,2},&t)&&std::abs(t-3)<.001f,"ray front / normalized distance");
    Check(Hit(w,id,{0,0,3},{0,0,-1}),"ray back");
    auto p=w.MoveAgainstDeformingMesh(id,{0,0,-3},{.3f,.9f,.3f},{0,0,30});Check(p.z<-.3f&&p.z>-.31f,"fast player front sweep");
    p=w.MoveAgainstDeformingMesh(id,{0,0,3},{.3f,.9f,.3f},{0,0,-30});Check(p.z>.3f&&p.z<.31f,"fast player back sweep");
    p=w.MoveAgainstDeformingMesh(id,{0,0,-1},{.3f,.9f,.3f},{1,0,2});Check(p.x>.99f&&p.z<-.3f,"player slide");
    p=w.MoveAgainstDeformingMesh(id,{0,0,-.1f},{.3f,.9f,.3f},{0,0,0});Check(p.z<-.3f,"zero-input depenetration");
    p=w.MoveAgainstDeformingMesh(id,{0,0,-.301f},{.3f,.9f,.3f},{0,0,-1});Check(p.z<-1.29f,"outward movement");
    auto shifted=panel;for(auto& v:shifted.positions)v.x+=10;
    w.UpdateDeformingMesh(id,shifted,{0,0,0});
    Check(!Hit(w,id,{0,0,-3},{0,0,1})&&Hit(w,id,{10,0,-3},{0,0,1}),"broadphase follows vertex deformation");
    p=w.MoveAgainstDeformingMesh(id,{0,0,-3},{.3f,.9f,.3f},{0,0,6});Check(p.z>2.99f,"old player collider removed");
    w.UpdateDeformingMesh(id,panel,{4,0,0},{0,.70710678f,0,.70710678f});
    Check(Hit(w,id,{1,0,0},{1,0,0},&t)&&std::abs(t-3)<.001f,"root position rotation / shape COM");
    auto bad=panel;bad.positions[0].x=std::numeric_limits<float>::quiet_NaN();
    Reject([&]{w.UpdateDeformingMesh(id,bad,{0,0,0});},"reject NaN");
    bad=panel;std::swap(bad.indices[0],bad.indices[1]);
    Reject([&]{w.UpdateDeformingMesh(id,bad,{0,0,0});},"reject topology change");
    Reject([&]{w.UpdateDeformingMesh(id,panel,{0,0,0},{0,0,0,0});},"reject rotation");
    Check(Hit(w,id,{1,0,0},{1,0,0},&t)&&std::abs(t-3)<.001f,"failed updates preserve shape and pose");
    auto box=w.Box({20,0,0},{1,1,1});Reject([&]{w.UpdateDeformingMesh(box,panel,{0,0,0});},"reject wrong body");
    Reject([&]{w.RayDeformingMesh(id,{0,0,0},{0,0,0},10,t);},"reject invalid ray");
    Reject([&]{w.MoveAgainstDeformingMesh(id,{0,0,0},{-1,1,1},{0,0,1});},"reject invalid extent");
    bad=panel;for(int i=0;i<4096;++i)bad.indices.insert(bad.indices.end(),{0,2,1});
    Reject([&]{w.DeformingMesh(bad,{0,0,0});},"reject triangle budget");
}
void Contacts(){
    RigidBodyWorld w;auto mesh=Split();auto id=w.DeformingMesh(mesh,{0,0,0});
    auto support=w.Box({-2,2,0},{.5f,.5f,.5f},1,0);auto hole=w.Box({0,2,0},{.5f,.5f,.5f},1,0);
    for(int i=0;i<600;++i){w.UpdateDeformingMesh(id,mesh,{0,0,0});w.Step(1.f/120);}
    Check(std::abs(w.Get(support).position.y-.25f)<.04f,"triangle support");
    Check(w.Get(hole).position.y<-5,"concave hole stays open");
    Check(!w.IsActive(support),"unchanged mesh permits sleeping");
    auto p=w.MoveAgainstDeformingMesh(id,{0,2,0},{.2f,.2f,.2f},{0,-4,0});Check(p.y<-1.99f,"player through hole");
    for(size_t i=0;i<4;++i)mesh.positions[i].y+=.1f;
    w.UpdateDeformingMesh(id,mesh,{0,0,0});Check(w.IsActive(support),"local deformation wakes support");
    for(int i=0;i<180;++i)w.Step(1.f/120);
    Check(std::abs(w.Get(support).position.y-.35f)<.04f,"contact follows locally deformed patch");
    float height;Check(Hit(w,id,{2,2,0},{0,-1,0},&height)&&std::abs(height-2)<.001f,"other patch remains unchanged");
    for(auto& v:mesh.positions)v.x+=8;
    w.UpdateDeformingMesh(id,mesh,{0,0,0});Check(w.IsActive(support),"removed support wakes sleeper");
    for(int i=0;i<120;++i)w.Step(1.f/120);
    Check(w.Get(support).position.y<-3,"old contacts invalidated");
    RigidBodyWorld contact;contact.Box({0,-.5f,0},{30,1,30});
    auto resting=contact.Box({0,.5f,0},{1,1,1},1,0);
    auto plate=Split();auto moving=contact.DeformingMesh(plate,{10,0,0});
    for(int i=0;i<600;++i)contact.Step(1.f/120);
    Check(!contact.IsActive(resting),"new-contact fixture asleep");
    contact.UpdateDeformingMesh(moving,plate,{2,.3f,0});Check(contact.IsActive(resting),"new overlap wakes sleeper");
    for(int i=0;i<120;++i)contact.Step(1.f/120);
    Check(contact.Get(resting).position.y>.75f,"new triangle contacts resolve");
}
void Animation(const char* meshPath,const char* animationPath){
    auto mesh=TriangleMesh::Load(meshPath);auto animation=geometry::SkeletalAnimation::Load(animationPath,mesh);
    RigidBodyWorld w;auto id=w.DeformingMesh(animation.Sample(0),{0,0,0});
    for(float time:{0.f,.5f,1.f,1.5f,2.f,2.5f,0.f}){
        auto pose=animation.Sample(time);w.UpdateDeformingMesh(id,pose,{0,0,0});
        // Every actual triangle centroid must be hittable after a non-rigid skin update.
        for(size_t i=0;i<pose.indices.size();i+=3){
            XMFLOAT3 center{};for(int j=0;j<3;++j){auto v=pose.positions[pose.indices[i+j]];center.x+=v.x/3;center.y+=v.y/3;center.z+=v.z/3;}
            center.z-=2;float t;Check(Hit(w,id,center,{0,0,1},&t)&&std::abs(t-2)<.002f,"skeletal triangle / loop / reset");
            auto player=w.MoveAgainstDeformingMesh(id,center,{.05f,.05f,.05f},{0,0,4});
            Check(player.z<-.049f&&player.z>-.06f,"player follows skeletal triangle");
        }
    }
    w.UpdateDeformingMesh(id,animation.Sample(0),{0,0,0});Check(Hit(w,id,{-.3f,.1f,-2},{0,0,1}),"rest foot hit");
    w.UpdateDeformingMesh(id,animation.Sample(1),{0,0,0});Check(!Hit(w,id,{-.3f,.1f,-2},{0,0,1}),"animated foot leaves original location");
}
}
int main(int argc,char** argv){try{Queries();Contacts();if(argc==3)Animation(argv[1],argv[2]);else throw std::runtime_error("mesh anim arguments required");std::cout<<"DEFORMING CHECK OK checks="<<checks<<"\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
