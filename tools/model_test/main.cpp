#include "engine/geometry/SkeletalAnimation.h"
#include <iostream>
#include <iomanip>
#include <string>
int wmain(int argc,wchar_t** argv){try{
    if(argc!=5)throw std::runtime_error("Usage: model_test model.mesh model.anim seconds loop(0|1)");
    auto mesh=geometry::TriangleMesh::Load(argv[1]);auto animation=geometry::SkeletalAnimation::Load(argv[2],mesh);
    auto pose=animation.Sample(std::stof(argv[3]),std::wstring(argv[4])==L"1");
    std::cout<<"POSE ["<<std::setprecision(9);for(size_t i=0;i<pose.positions.size();i++){
        auto p=pose.positions[i];if(i)std::cout<<',';std::cout<<'['<<p.x<<','<<p.y<<','<<p.z<<']';
    }std::cout<<"]\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 2;}}
