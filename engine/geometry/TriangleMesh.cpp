#include "engine/geometry/TriangleMesh.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
using namespace DirectX;
namespace geometry {
void TriangleMesh::Validate() const {
    if(positions.size()<3 || positions.size()>MaxVertices || indices.empty() || indices.size()%3 || indices.size()>3*MaxTriangles)
        throw std::runtime_error("Invalid model vertex/triangle count");
    for(auto p:positions) for(float v:{p.x,p.y,p.z})
        if(!std::isfinite(v) || std::abs(v)>100000) throw std::runtime_error("Model coordinates must be finite and within 100000 units");
    for(size_t i=0;i<indices.size();i+=3){
        for(int j=0;j<3;++j) if(indices[i+j]>=positions.size()) throw std::runtime_error("Model index outside vertex array");
        auto a=XMLoadFloat3(&positions[indices[i]]),b=XMLoadFloat3(&positions[indices[i+1]]),c=XMLoadFloat3(&positions[indices[i+2]]);
        auto n=XMVector3Cross(b-a,c-a);
        if(XMVectorGetX(XMVector3LengthSq(n))<=1e-16f)throw std::runtime_error("Degenerate model triangle");
    }
}
TriangleMesh TriangleMesh::Load(const std::filesystem::path& file){
    std::ifstream in(file,std::ios::binary|std::ios::ate);
    if(!in)throw std::runtime_error("Packaged model.mesh is missing");
    auto bytes=in.tellg();in.seekg(0);char magic[8]{};uint32_t counts[2]{};
    in.read(magic,8);in.read(reinterpret_cast<char*>(counts),sizeof(counts));
    if(!in || std::memcmp(magic,"GEMESH1\0",8) || counts[0]<3 || counts[0]>MaxVertices || counts[1]<1 || counts[1]>MaxTriangles)
        throw std::runtime_error("Invalid model.mesh header");
    const uint64_t expected=16+uint64_t(counts[0])*12+uint64_t(counts[1])*12;
    if(bytes!=static_cast<std::streamoff>(expected))throw std::runtime_error("Invalid model.mesh length");
    static_assert(sizeof(XMFLOAT3)==12);
    TriangleMesh mesh;mesh.positions.resize(counts[0]);mesh.indices.resize(counts[1]*3);
    in.read(reinterpret_cast<char*>(mesh.positions.data()),mesh.positions.size()*sizeof(XMFLOAT3));
    in.read(reinterpret_cast<char*>(mesh.indices.data()),mesh.indices.size()*sizeof(uint32_t));
    if(!in)throw std::runtime_error("Truncated model.mesh");mesh.Validate();return mesh;
}
}
