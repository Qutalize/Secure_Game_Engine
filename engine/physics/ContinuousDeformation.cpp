#include "engine/physics/ContinuousDeformation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace physics {
namespace {
struct V {double x,y,z;V operator+(V b)const{return {x+b.x,y+b.y,z+b.z};}V operator-(V b)const{return {x-b.x,y-b.y,z-b.z};}V operator*(double s)const{return {x*s,y*s,z*s};}};
V D(DirectX::XMFLOAT3 p){return {p.x,p.y,p.z};}
double Dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
double Length(V a){return std::sqrt(Dot(a,a));}
V Segment(V p,V a,V b){auto d=b-a;double l=Dot(d,d);return a+d*(l>1e-24?std::clamp(Dot(p-a,d)/l,0.,1.):0.);}
V Closest(V p,V a,V b,V c){
    // Project onto the plane, then test barycentric coordinates. Degenerate intermediate
    // triangles reduce to their edges instead of producing a division by zero.
    auto ab=b-a,ac=c-a,ap=p-a;double aa=Dot(ab,ab),bb=Dot(ab,ac),cc=Dot(ac,ac),u=Dot(ap,ab),v=Dot(ap,ac);
    double det=aa*cc-bb*bb;
    if(det>1e-24){double y=(cc*u-bb*v)/det,z=(aa*v-bb*u)/det;if(y>=0&&z>=0&&y+z<=1)return a+ab*y+ac*z;}
    V q=Segment(p,a,b),q2=Segment(p,b,c),q3=Segment(p,c,a);
    if(Dot(p-q2,p-q2)<Dot(p-q,p-q))q=q2;if(Dot(p-q3,p-q3)<Dot(p-q,p-q))q=q3;return q;
}
bool Finite(V p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
}
float LimitDeformation(const geometry::TriangleMesh& before,const geometry::TriangleMesh& after,std::span<const SweptSphere> spheres){
    before.Validate();after.Validate();
    if(before.indices!=after.indices||before.positions.size()!=after.positions.size()||before.indices.size()/3>4096||spheres.size()>128)
        throw std::runtime_error("Invalid continuous deformation topology/capacity");
    double safe=1;constexpr double skin=1e-4;
    for(const auto& sphere:spheres){
        V start=D(sphere.start),finish=D(sphere.end),motion=finish-start;
        if(!Finite(start)||!Finite(finish)||!std::isfinite(sphere.radius)||sphere.radius<=0)throw std::runtime_error("Invalid continuous sphere");
        for(size_t face=0;face<before.indices.size();face+=3){
            V a[3],delta[3];double speed=0;
            for(int k=0;k<3;++k){auto j=before.indices[face+k];a[k]=D(before.positions[j]);delta[k]=D(after.positions[j])-a[k];speed=std::max(speed,Length(delta[k]-motion));}
            if(speed<1e-12)continue;
            // Swept AABB rejection preserves holes and avoids a full-world hull approximation.
            bool separate=false;
            for(int axis=0;axis<3;++axis){auto component=[axis](V p){return axis==0?p.x:axis==1?p.y:p.z;};
                double lo=1e300,hi=-1e300;for(int k=0;k<3;++k){lo=std::min({lo,component(a[k]),component(a[k]+delta[k])});hi=std::max({hi,component(a[k]),component(a[k]+delta[k])});}
                if(hi<std::min(component(start),component(finish))-sphere.radius-skin||lo>std::max(component(start),component(finish))+sphere.radius+skin)separate=true;
            }
            if(separate)continue;
            double t=0;
            for(int iteration=0;iteration<96;++iteration){
                V p=start+motion*t,q=Closest(p,a[0]+delta[0]*t,a[1]+delta[1]*t,a[2]+delta[2]*t),gap=p-q;
                double distance=Length(gap),separation=distance-sphere.radius;
                if(separation<=skin){
                    // A supporting/tangent contact is allowed to separate. Only certified
                    // separating linear motion may bypass an initial overlap.
                    if(t==0&&distance>skin){V normal=gap*(1/distance);double closing=-1e300;
                        for(int k=0;k<3;++k)closing=std::max(closing,Dot(delta[k]-motion,normal));
                        if(closing<=1e-10)break;
                    }
                    safe=std::min(safe,t);break;
                }
                double advance=(separation-skin)/speed;
                if(t+advance>=safe)break;
                if(advance<1e-10||iteration==95){safe=std::min(safe,t);break;}
                t+=advance*.99; // Remain on the safe side of floating-point roundoff.
            }
        }
    }
    // Round down, not across the time of impact when converting double to float.
    float fraction=static_cast<float>(safe);if(fraction>safe)fraction=std::nextafter(fraction,0.f);return fraction;
}
}
