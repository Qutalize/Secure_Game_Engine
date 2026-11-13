#include "engine/renderer/TriangleAcceleration.h"
#include <algorithm>
#include <limits>
using namespace DirectX;
namespace raymesh {
namespace {
float Component(XMFLOAT3 v,int axis){return axis==0?v.x:axis==1?v.y:v.z;}
void Grow(Node& n,XMFLOAT3 v){n.minimum={std::min(n.minimum.x,v.x),std::min(n.minimum.y,v.y),std::min(n.minimum.z,v.z)};n.maximum={std::max(n.maximum.x,v.x),std::max(n.maximum.y,v.y),std::max(n.maximum.z,v.z)};}
void Split(Acceleration& tree,uint32_t node,uint32_t begin,uint32_t count){
    Node bounds;float inf=std::numeric_limits<float>::infinity();bounds.minimum={inf,inf,inf};bounds.maximum={-inf,-inf,-inf};
    for(uint32_t i=begin;i<begin+count;++i){Grow(bounds,tree.triangles[i].a);Grow(bounds,tree.triangles[i].b);Grow(bounds,tree.triangles[i].c);}
    tree.nodes[node]=bounds;
    if(count<=4){tree.nodes[node].first=begin;tree.nodes[node].count=count;return;}
    XMFLOAT3 extent{bounds.maximum.x-bounds.minimum.x,bounds.maximum.y-bounds.minimum.y,bounds.maximum.z-bounds.minimum.z};
    int axis=extent.y>extent.x?1:0;if(extent.z>Component(extent,axis))axis=2;
    auto midpoint=begin+count/2;
    std::nth_element(tree.triangles.begin()+begin,tree.triangles.begin()+midpoint,tree.triangles.begin()+begin+count,
        [axis](const Triangle& a,const Triangle& b){return Component(a.a,axis)+Component(a.b,axis)+Component(a.c,axis)<Component(b.a,axis)+Component(b.b,axis)+Component(b.c,axis);});
    auto child=static_cast<uint32_t>(tree.nodes.size());tree.nodes.resize(child+2);tree.nodes[node].first=child;
    Split(tree,child,begin,count/2);Split(tree,child+1,midpoint,count-count/2);
}
}
Acceleration Build(const geometry::TriangleMesh& mesh){
    mesh.Validate();Acceleration tree;tree.triangles.reserve(mesh.indices.size()/3);
    for(size_t i=0;i<mesh.indices.size();i+=3){Triangle t;t.a=mesh.positions[mesh.indices[i]];t.b=mesh.positions[mesh.indices[i+1]];t.c=mesh.positions[mesh.indices[i+2]];
        XMStoreFloat3(&t.normal,XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&t.b)-XMLoadFloat3(&t.a),XMLoadFloat3(&t.c)-XMLoadFloat3(&t.a))));tree.triangles.push_back(t);}
    // Median splitting limits depth to <= 15 for 65536 triangles; shader stack has 32 entries.
    tree.nodes.resize(1);Split(tree,0,0,static_cast<uint32_t>(tree.triangles.size()));return tree;
}
}
