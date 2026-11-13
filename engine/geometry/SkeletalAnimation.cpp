#include "engine/geometry/SkeletalAnimation.h"
#include "engine/core/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
using namespace DirectX;
namespace geometry {
SkeletalAnimation SkeletalAnimation::Load(const std::filesystem::path& file,const TriangleMesh& mesh){
    mesh.Validate();std::ifstream in(file,std::ios::binary|std::ios::ate);
    if(!in)throw std::runtime_error("Packaged model.anim is missing");
    const auto bytes=in.tellg();in.seekg(0);char magic[8]{};uint32_t counts[3]{};float duration=0;
    in.read(magic,8);in.read(reinterpret_cast<char*>(counts),sizeof(counts));in.read(reinterpret_cast<char*>(&duration),4);
    if(!in||std::memcmp(magic,"GEANIM1\0",8)||counts[0]!=mesh.positions.size()||counts[1]<1||counts[1]>256||counts[2]<2||counts[2]>3601||!std::isfinite(duration)||duration<=0||duration>60)
        throw std::runtime_error("Invalid model.anim header");
    static_assert(sizeof(SkinVertex)==36&&sizeof(XMFLOAT4X4)==64);
    const uint64_t expected=24+uint64_t(counts[0])*36+uint64_t(counts[1])*counts[2]*64;
    if(expected>64*1024*1024||bytes!=static_cast<std::streamoff>(expected))throw std::runtime_error("Invalid model.anim length");
    SkeletalAnimation anim;anim.m_jointCount=counts[1];anim.m_frameCount=counts[2];anim.m_duration=duration;anim.m_indices=mesh.indices;
    anim.m_vertices.resize(counts[0]);anim.m_matrices.resize(size_t(counts[1])*counts[2]);
    in.read(reinterpret_cast<char*>(anim.m_vertices.data()),anim.m_vertices.size()*sizeof(SkinVertex));
    in.read(reinterpret_cast<char*>(anim.m_matrices.data()),anim.m_matrices.size()*sizeof(XMFLOAT4X4));
    if(!in)throw std::runtime_error("Truncated model.anim");
    for(const auto& v:anim.m_vertices){
        for(float x:{v.bind.x,v.bind.y,v.bind.z})if(!std::isfinite(x)||std::abs(x)>100000)throw std::runtime_error("Invalid skin bind position");
        float sum=0;for(unsigned k=0;k<4;k++){if(v.joints[k]>=anim.m_jointCount||!std::isfinite(v.weights[k])||v.weights[k]<0||v.weights[k]>1)throw std::runtime_error("Invalid skin joint/weight");sum+=v.weights[k];}
        if(std::abs(sum-1)>1e-4f)throw std::runtime_error("Skin weights must sum to one");
    }
    for(const auto& m:anim.m_matrices){
        for(const auto& row:m.m)for(float x:row)if(!std::isfinite(x)||std::abs(x)>1e8f)throw std::runtime_error("Invalid skin matrix values");
        if(std::abs(m._14)>1e-6f||std::abs(m._24)>1e-6f||std::abs(m._34)>1e-6f||std::abs(m._44-1)>1e-6f)throw std::runtime_error("Skin matrix must be affine");
    }
    auto initial=anim.Sample(0,false);
    for(size_t i=0;i<mesh.positions.size();i++){
        const auto p=XMLoadFloat3(&mesh.positions[i]);const auto q=XMLoadFloat3(&initial.positions[i]);
        if(XMVectorGetX(XMVector3Length(p-q))>1e-3f*(1+XMVectorGetX(XMVector3Length(p))))throw std::runtime_error("Animation does not match packaged model.mesh");
    }
    LOG_INFO("MODEL ANIMATION loaded vertices=%u palette=%u samples=%u duration=%.3f cpu-skinning=1",counts[0],counts[1],counts[2],duration);return anim;
}
TriangleMesh SkeletalAnimation::Sample(double seconds,bool loop) const{
    if(!std::isfinite(seconds)||seconds<0||m_frameCount<2)throw std::runtime_error("Invalid animation playback time/state");
    const double time=loop?std::fmod(seconds,double(m_duration)):std::min(seconds,double(m_duration));
    const double frame=time/m_duration*(m_frameCount-1);const auto a=std::min(uint32_t(frame),m_frameCount-1),b=std::min(a+1,m_frameCount-1);
    const float fraction=static_cast<float>(std::clamp(frame-a,0.,1.));
    std::array<XMMATRIX,256> palette;
    for(uint32_t j=0;j<m_jointCount;j++){
        auto first=XMLoadFloat4x4(&m_matrices[size_t(a)*m_jointCount+j]),second=XMLoadFloat4x4(&m_matrices[size_t(b)*m_jointCount+j]);
        for(int row=0;row<4;row++)palette[j].r[row]=XMVectorLerp(first.r[row],second.r[row],fraction);
    }
    TriangleMesh mesh;mesh.indices=m_indices;mesh.positions.resize(m_vertices.size());
    for(size_t i=0;i<m_vertices.size();i++){
        const auto& v=m_vertices[i];auto position=XMLoadFloat3(&v.bind);auto deformed=XMVectorZero();
        for(int k=0;k<4;k++)if(v.weights[k]>0)deformed+=XMVector3TransformCoord(position,palette[v.joints[k]])*v.weights[k];
        XMStoreFloat3(&mesh.positions[i],deformed);
    }
    mesh.Validate();return mesh;
}
#if ENGINE_RAGDOLL
std::vector<XMFLOAT4X4> SkeletalAnimation::InitialPalette()const{return {m_matrices.begin(),m_matrices.begin()+m_jointCount};}
TriangleMesh SkeletalAnimation::Skin(std::span<const XMFLOAT4X4> matrices)const{
    if(matrices.size()!=m_jointCount)throw std::runtime_error("Ragdoll palette count mismatch");
    std::array<XMMATRIX,256> palette;
    for(size_t j=0;j<matrices.size();++j){
        const auto& m=matrices[j];for(const auto& row:m.m)for(float v:row)if(!std::isfinite(v))throw std::runtime_error("Invalid ragdoll skin matrix");
        if(std::abs(m._14)>1e-5f||std::abs(m._24)>1e-5f||std::abs(m._34)>1e-5f||std::abs(m._44-1)>1e-5f)throw std::runtime_error("Invalid ragdoll affine matrix");
        palette[j]=XMLoadFloat4x4(&m);
    }
    TriangleMesh mesh;mesh.indices=m_indices;mesh.positions.resize(m_vertices.size());
    for(size_t i=0;i<m_vertices.size();++i){const auto& v=m_vertices[i];auto p=XMLoadFloat3(&v.bind),out=XMVectorZero();
        for(int k=0;k<4;++k)if(v.weights[k]>0)out+=XMVector3TransformCoord(p,palette[v.joints[k]])*v.weights[k];
        XMStoreFloat3(&mesh.positions[i],out);}
    mesh.Validate();return mesh;
}
#endif
#if ENGINE_DEFORMATION_CCD
double SkeletalAnimation::TimeToNextKey(double seconds)const{
    if(!std::isfinite(seconds)||seconds<0)throw std::runtime_error("Invalid continuous animation time");
    double interval=double(m_duration)/(m_frameCount-1),next=(std::floor(seconds/interval)+1)*interval;
    if(next<=seconds)next+=interval;
    return next-seconds;
}
#endif

}
