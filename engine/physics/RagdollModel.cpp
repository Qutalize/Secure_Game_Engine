#include "engine/physics/RagdollModel.h"
#include "engine/physics/RigidBodyWorld.h"
#include "engine/core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
using namespace DirectX;
namespace physics {
namespace {
void Check(const std::vector<RagdollBone>& bones,uint32_t count){
    if(count<2||count>32||bones.size()!=count)throw std::runtime_error("Invalid ragdoll bone count");
    bool joint=false;
    for(size_t i=0;i<bones.size();++i){const auto& b=bones[i];std::vector<bool> seen(count);int j=int(i);
        while(j!=-1){if(j<0||j>=int(count)||seen[j])throw std::runtime_error("Invalid ragdoll hierarchy");seen[j]=true;j=bones[j].parent;}
        joint|=b.parent>=0;
        for(float v:{b.center.x,b.center.y,b.center.z,b.anchor.x,b.anchor.y,b.anchor.z})if(!std::isfinite(v)||std::abs(v)>10000)throw std::runtime_error("Invalid ragdoll anchor/center");
        for(float v:{b.size.x,b.size.y,b.size.z})if(!std::isfinite(v)||v<.09999f||v>20)throw std::runtime_error("Invalid ragdoll proxy size");
    }
    if(!joint)throw std::runtime_error("Ragdoll has no joints");
}
}
std::vector<RagdollBone> RagdollModel::Load(const std::filesystem::path& file,uint32_t paletteCount){
    std::ifstream in(file,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("Packaged model.rig is missing");
    auto bytes=in.tellg();in.seekg(0);char magic[8]{};uint32_t count=0;in.read(magic,8);in.read(reinterpret_cast<char*>(&count),4);
    static_assert(sizeof(RagdollBone)==40);
    if(!in||std::memcmp(magic,"GERIG1\0\0",8)||count!=paletteCount||count<2||count>32||bytes!=12+int64_t(count)*40)throw std::runtime_error("Invalid model.rig header/length");
    std::vector<RagdollBone> bones(count);in.read(reinterpret_cast<char*>(bones.data()),bones.size()*sizeof(RagdollBone));
    if(!in)throw std::runtime_error("Truncated model.rig");Check(bones,count);return bones;
}
RagdollModel::RagdollModel(RigidBodyWorld& world,const geometry::SkeletalAnimation& animation,const std::vector<RagdollBone>& bones,XMFLOAT3 position)
    :m_world(world),m_animation(animation),m_bones(bones),m_initial(animation.InitialPalette()){
    Check(bones,static_cast<uint32_t>(m_initial.size()));
    if(!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(position.z))throw std::runtime_error("Invalid ragdoll position");
    double volume=0;for(const auto& b:bones)volume+=b.size.x*b.size.y*b.size.z;
    for(const auto& b:bones){float mass=std::max(.05f,float(12*b.size.x*b.size.y*b.size.z/volume));
        m_bodies.push_back(world.Box({b.center.x+position.x,b.center.y+position.y,b.center.z+position.z},b.size,mass,0,.8f));}
    world.ConnectRagdoll(m_bodies,bones,position);
    LOG_INFO("RAGDOLL active bones=%zu joints=limited-swing-twist self-collision=0",bones.size());
}
geometry::TriangleMesh RagdollModel::Pose()const{
    auto palette=m_initial;
    for(size_t j=0;j<m_bodies.size();++j){auto pose=m_world.Get(m_bodies[j]);auto c=m_bones[j].center;
        auto delta=XMMatrixTranslation(-c.x,-c.y,-c.z)*XMMatrixRotationQuaternion(XMLoadFloat4(&pose.rotation))*XMMatrixTranslation(pose.position.x,pose.position.y,pose.position.z);
        XMStoreFloat4x4(&palette[j],XMLoadFloat4x4(&m_initial[j])*delta);}
    return m_animation.Skin(palette); // World-space vertices; renderer uses identity transform.
}
float RagdollModel::MaxJointError()const{
    float error=0;
    for(size_t j=0;j<m_bones.size();++j){int parent=m_bones[j].parent;if(parent<0)continue;
        auto anchor=[&](size_t bone){auto p=m_world.Get(m_bodies[bone]);auto c=m_bones[bone].center;auto a=m_bones[j].anchor;
            XMFLOAT3 local{a.x-c.x,a.y-c.y,a.z-c.z};return XMVector3Rotate(XMLoadFloat3(&local),XMLoadFloat4(&p.rotation))+XMLoadFloat3(&p.position);};
        error=std::max(error,XMVectorGetX(XMVector3Length(anchor(j)-anchor(parent))));}
    return error;
}
}
