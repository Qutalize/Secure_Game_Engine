// Small, procedural starting scenes. They share engine modules, not legacy game dependencies.
#include "engine/core/EngineFeatures.h"
#include "engine/core/Logger.h"
#if ENGINE_MODEL_GEOMETRY
#include "engine/geometry/TriangleMesh.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "engine/core/ConsoleUtf8.h"
#if ENGINE_MODEL_ANIMATION
#include "engine/geometry/SkeletalAnimation.h"
#include "engine/core/Common.h"
#include <cstring>
#endif
#if ENGINE_RAGDOLL
#include "engine/physics/RagdollModel.h"
#endif
#if ENGINE_RIGID_BODY
#include "engine/physics/RigidBodyWorld.h"
#endif
#if ENGINE_DISTRIBUTION_SIGNATURE
#include "engine/security/DistributionIntegrity.h"
#endif
#include "engine/core/RunMetrics.h"
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if ENGINE_COLLISION
#include "engine/math/Collision.h"
#endif
#if ENGINE_AUDIO
#include "engine/audio/AudioPlayer.h"
#endif
#if ENGINE_RENDER
#include "engine/core/Window.h"
#include "engine/renderer/Renderer.h"
#endif
using namespace DirectX;
namespace {
constexpr bool IsGame(const char* value) { return std::string_view(RUNTIME_GAME) == value; }
constexpr bool thirdPerson = IsGame("tps") || IsGame("monster_capture") || IsGame("action_rpg") || IsGame("chase") || IsGame("social_deduction");
constexpr bool shoot = IsGame("fps") || IsGame("tps") || IsGame("aim_training");
#if ENGINE_MODEL_GEOMETRY
const geometry::TriangleMesh& ModelGeometry(){
    static const auto mesh=[](){wchar_t path[32768]{};auto length=GetModuleFileNameW(nullptr,path,32768);
        if(!length||length==32768)throw std::runtime_error("Cannot locate packaged model");
        return geometry::TriangleMesh::Load(std::filesystem::path(path).parent_path()/"assets"/"model.mesh");}();
    return mesh;
}
#endif
#if ENGINE_MODEL_ANIMATION
const geometry::SkeletalAnimation& ModelAnimation(){
    static const auto animation=[](){wchar_t path[32768]{};auto length=GetModuleFileNameW(nullptr,path,32768);
        if(!length||length==32768)throw std::runtime_error("Cannot locate packaged animation");
        return geometry::SkeletalAnimation::Load(std::filesystem::path(path).parent_path()/"assets"/"model.anim",ModelGeometry());}();
    return animation;
}
#endif
#if ENGINE_RAGDOLL
const std::vector<physics::RagdollBone>& ModelRig(){
    static const auto bones=[](){wchar_t path[32768]{};auto length=GetModuleFileNameW(nullptr,path,32768);
        if(!length||length==32768)throw std::runtime_error("Cannot locate packaged rig");
        return physics::RagdollModel::Load(std::filesystem::path(path).parent_path()/"assets"/"model.rig",static_cast<uint32_t>(ModelAnimation().InitialPalette().size()));}();return bones;
}
#endif
struct Scene {
#if ENGINE_RIGID_BODY
    std::unique_ptr<physics::RigidBodyWorld> rigid=std::make_unique<physics::RigidBodyWorld>();
    std::vector<uint32_t> dynamicBodies;
#if ENGINE_RAGDOLL
    std::unique_ptr<physics::RagdollModel> ragdoll;
    geometry::TriangleMesh ragdollGeometry;
#endif
#if ENGINE_MODEL_CONVEX || ENGINE_MODEL_STATIC || ENGINE_MODEL_DEFORMING
    uint32_t modelBody=0;
#endif
#if ENGINE_MODEL_DEFORMING
    geometry::TriangleMesh modelGeometry=ModelAnimation().Sample(0);
#endif
    Scene(){
        rigid->Box({0,-.1f,2},{24,.2f,24});rigid->Box({0,1.5f,-.5f},{2,3,1});
        for(int i=0;i<3;++i)dynamicBodies.push_back(rigid->Box({-3.f+i*2,4.f+i*2,3},{1,1,1},1,.35f,.7f));
#if ENGINE_MODEL_CONVEX
        modelBody=rigid->ConvexHull(ModelGeometry(),{2,4,1.5f},2,.2f,.7f);
#elif ENGINE_MODEL_STATIC
        modelBody=rigid->StaticMesh(ModelGeometry(),{2,1.5f,1.5f});
#elif ENGINE_MODEL_DEFORMING
        modelBody=rigid->DeformingMesh(modelGeometry,{2,1.5f,1.5f});
#endif
#if ENGINE_RAGDOLL
        ragdoll=std::make_unique<physics::RagdollModel>(*rigid,ModelAnimation(),ModelRig(),XMFLOAT3{-3,4,1.5f});
        ragdollGeometry=ragdoll->Pose();
#endif
    }
    void Kick(){
#if ENGINE_RAGDOLL
        auto id=ragdoll->Bodies().back();
#elif ENGINE_MODEL_CONVEX
        auto id=modelBody;
#else
        auto id=dynamicBodies[0];
#endif
        auto p=rigid->Get(id).position;p.y+=.4f;rigid->Impulse(id,{3,4,1},p);}
#endif
#if ENGINE_MODEL_GEOMETRY
    XMMATRIX ModelWorld()const{
#if ENGINE_MODEL_CONVEX || ENGINE_MODEL_STATIC || ENGINE_MODEL_DEFORMING
        auto pose=rigid->Get(modelBody);
        return XMMatrixRotationQuaternion(XMLoadFloat4(&pose.rotation))*XMMatrixTranslation(pose.position.x,pose.position.y,pose.position.z);
#else
        return XMMatrixTranslation(2,1.5f,1.5f);
#endif
    }
#endif
    XMFLOAT3 player{0,1,-5};
    std::array<XMFLOAT3,5> targets{{{0,1,3},{-3,1,4},{3,1,5},{-5,1,7},{5,1,7}}};
    std::array<bool,5> active{{true,true,true,true,true}};
    float yaw=0, pitch=0;double time=0;
    int score=0;
#if ENGINE_COLLISION
    std::vector<AABB> walls{{{-1,0,-1},{1,3,0}}};
#endif
    void Step(float x,float z,float dt) {
#if ENGINE_MODEL_DEFORMING
        if(!std::isfinite(dt)||dt<=0||dt>1)throw std::runtime_error("Invalid scene step");
        // Update skin and collider before EVERY physics substep, even at low physics_hz.
        const int steps=std::max(1,static_cast<int>(std::ceil(dt*120)));
        for(int i=0;i<steps;++i)StepOnce(x,z,dt/steps);
#else
        StepOnce(x,z,dt);
#endif
    }
    void StepOnce(float x,float z,float dt) {
#if ENGINE_DEFORMATION_CCD
        // Trace animation segments without feeding tiny boundary remainders to the physics solver.
        // All segments use a sphere enclosing the predicted body's WHOLE physical step.
        const double end=time+dt;unsigned segments=0;
        while(time<end){
            if(++segments>8)throw std::runtime_error("Continuous animation segment budget exceeded");
            const double nextTime=std::min(end,time+ModelAnimation().TimeToNextKey(time));
            if(nextTime<=time)throw std::runtime_error("Continuous animation clock did not advance");
            auto next=rigid->ConstrainDeformation(modelBody,ModelAnimation().Sample(nextTime),{2,1.5f,1.5f},{0,0,0,1},dt,player,{.3f,.9f,.3f});
            rigid->UpdateDeformingMesh(modelBody,next,{2,1.5f,1.5f});modelGeometry=std::move(next);time=nextTime;
        }
#else
        time+=dt;
#if ENGINE_MODEL_DEFORMING
        auto next=ModelAnimation().Sample(time);
        rigid->UpdateDeformingMesh(modelBody,next,{2,1.5f,1.5f});modelGeometry=std::move(next);
#endif
#endif
#if ENGINE_RIGID_BODY
        rigid->Step(dt);
#if ENGINE_RAGDOLL
        ragdollGeometry=ragdoll->Pose();
#endif
#endif
        float len=std::sqrt(x*x+z*z); if(len>1) {x/=len;z/=len;}
        XMFLOAT3 delta{(x*std::cos(yaw)+z*std::sin(yaw))*4*dt,0,(-x*std::sin(yaw)+z*std::cos(yaw))*4*dt};
#if ENGINE_COLLISION
        const int substeps=std::max(1,static_cast<int>(std::ceil(std::max(std::abs(delta.x),std::abs(delta.z))/.25f)));
        delta.x/=substeps;delta.z/=substeps;
        for(int i=0;i<substeps;++i){
            auto desired=ResolveAABBCollision(player,{.3f,.9f,.3f},delta,walls);
#if ENGINE_MODEL_DEFORMING
            desired=rigid->MoveAgainstDeformingMesh(modelBody,player,{.3f,.9f,.3f},
                         {desired.x-player.x,desired.y-player.y,desired.z-player.z});
            desired=ResolveAABBCollision(desired,{.3f,.9f,.3f},{0,0,0},walls);
#endif
            player=desired;
        }
#else
        player.x+=delta.x;player.z+=delta.z;
#endif
        if(IsGame("chase")) targets[0]={static_cast<float>(std::sin(time*.5)*4),1,static_cast<float>(3+std::cos(time*.5)*2)};
    }
    XMFLOAT3 Eye() const {
        if(thirdPerson) return {player.x-std::sin(yaw)*5,player.y+2.3f,player.z-std::cos(yaw)*5};
        return {player.x,player.y+.6f,player.z};
    }
    void Act() {
#if ENGINE_AUDIO
        if(shoot) AudioPlayer::Get().PlayGunshot();
#endif
#if ENGINE_COLLISION
        int hit=-1;
        if(shoot) {
            float nearest=1000;const auto eye=Eye();
            const XMFLOAT3 dir{std::sin(yaw)*std::cos(pitch),std::sin(pitch),std::cos(yaw)*std::cos(pitch)};
            float t;
            for(const auto& wall:walls) if(RayIntersectAABB(eye,dir,wall,t)) nearest=std::min(nearest,t);
#if ENGINE_MODEL_DEFORMING
            if(rigid->RayDeformingMesh(modelBody,eye,dir,nearest,t))nearest=t;
#endif
            for(int i=0;i<5;++i) if(active[i]) {
                auto p=targets[i];AABB box{{p.x-.5f,p.y-.5f,p.z-.5f},{p.x+.5f,p.y+.5f,p.z+.5f}};
                if(RayIntersectAABB(eye,dir,box,t) && t<nearest) {nearest=t;hit=i;}
            }
        } else {
            for(int i=0;i<5;++i) if(active[i]) {
                auto p=targets[i];if(std::hypot(player.x-p.x,player.z-p.z)<1.8f) {hit=i;break;}
            }
        }
        if(hit>=0) {
            active[hit]=false;++score;
            LOG_INFO("INTERACTION game=%s target=%d score=%d",RUNTIME_GAME,hit,score);
#if ENGINE_AUDIO
            AudioPlayer::Get().PlayHit();
#endif
        }
#endif
    }
};
void CheckSimulation() {
#if ENGINE_RIGID_BODY
    physics::CheckRigidBodies();
#if ENGINE_MODEL_CONVEX || ENGINE_MODEL_STATIC
    physics::CheckModelBodies();
#endif
#endif
#if ENGINE_MODEL_GEOMETRY
    ModelGeometry().Validate();LOG_INFO("MODEL GEOMETRY CHECK OK");
#endif
#if ENGINE_MODEL_ANIMATION
    for(float fraction:{0.f,.25f,.5f,.75f,1.f})ModelAnimation().Sample(ModelAnimation().Duration()*fraction,false).Validate();
    LOG_INFO("MODEL ANIMATION CHECK OK");
#endif
    Scene scene;
    for(int i=0;i<RUNTIME_PHYSICS_HZ*(ENGINE_DEFORMATION_CCD?3:2);++i) scene.Step(0,1,1.0f/RUNTIME_PHYSICS_HZ);
#if ENGINE_COLLISION
    if(scene.player.z > -1.299f) throw std::runtime_error("collision smoke failed");
    float t=0;
    if(!RayIntersectAABB({0,1,-5},{0,0,1},scene.walls[0],t) || std::abs(t-4)>0.001f)
        throw std::runtime_error("ray smoke failed");
#else
    if(std::abs(scene.player.z-3)>0.01f) throw std::runtime_error("free movement smoke failed");
#endif
    Scene interaction;
#if ENGINE_COLLISION
    if(shoot) {
        interaction.Act();
        if(interaction.score!=0) throw std::runtime_error("wall occlusion smoke failed");
        interaction.player.x=3;
        interaction.targets[0]={3,interaction.Eye().y,3};
    } else interaction.player=interaction.targets[0];
    interaction.Act();
    if(interaction.score!=1 || interaction.active[0]) throw std::runtime_error("interaction smoke failed");
#else
    interaction.Act();
    if(interaction.score!=0) throw std::runtime_error("disabled collision still performs hit tests");
#endif
#if ENGINE_MODEL_DEFORMING
    Scene animated;
    animated.walls.clear();
    for(int step=0;step<4;++step){
        animated.Step(0,0,.25f);
#if ENGINE_DEFORMATION_CCD
        const auto& expected=animated.modelGeometry; // Contact-limited pose may precede the animation clock.
#else
        const auto expected=ModelAnimation().Sample(animated.time);
#endif
        if(expected.positions.size()!=animated.modelGeometry.positions.size()||
           std::memcmp(expected.positions.data(),animated.modelGeometry.positions.data(),expected.positions.size()*sizeof(XMFLOAT3)))
            throw std::runtime_error("Scene skin and collision time mismatch");
        auto a=XMLoadFloat3(&expected.positions.at(expected.indices.at(0)));
        auto b=XMLoadFloat3(&expected.positions.at(expected.indices.at(1)));
        auto c=XMLoadFloat3(&expected.positions.at(expected.indices.at(2)));
        XMFLOAT3 center,normal;
        XMStoreFloat3(&center,XMVector3TransformCoord((a+b+c)/3,animated.ModelWorld()));
        XMStoreFloat3(&normal,XMVector3Normalize(XMVector3TransformNormal(XMVector3Cross(b-a,c-a),animated.ModelWorld())));
        XMFLOAT3 eye{center.x-normal.x,center.y-normal.y,center.z-normal.z};float distance;
        if(!animated.rigid->RayDeformingMesh(animated.modelBody,eye,normal,1.01f,distance))
            throw std::runtime_error("Scene animated triangle ray mismatch");
        if(shoot){
            animated.yaw=std::atan2(normal.x,normal.z);animated.pitch=std::asin(std::clamp(normal.y,-1.f,1.f));
            animated.player={eye.x+(thirdPerson?std::sin(animated.yaw)*5:0),eye.y-(thirdPerson?2.3f:.6f),eye.z+(thirdPerson?std::cos(animated.yaw)*5:0)};
            animated.active.fill(false);animated.active[0]=true;animated.score=0;
            animated.targets[0]={center.x+normal.x,center.y+normal.y,center.z+normal.z};
            animated.Act();if(animated.score)throw std::runtime_error("Animated model did not occlude shooting");
        }
    }
    if(shoot){
        animated.rigid->UpdateDeformingMesh(animated.modelBody,animated.modelGeometry,{100,100,100});
        animated.Act();if(animated.score!=1)throw std::runtime_error("Old animated model still occludes shooting");
    }
    LOG_INFO("DEFORMING SCENE CHECK OK: shared pose, substeps, shooting occlusion");
#endif
#if ENGINE_RAGDOLL
    if(scene.ragdoll->MaxJointError()>.15f)throw std::runtime_error("Ragdoll scene joint separation");
    scene.ragdollGeometry.Validate();LOG_INFO("RAGDOLL SCENE CHECK OK");
#endif
#if ENGINE_DEFORMATION_CCD
    LOG_INFO("DEFORMATION CCD active conservative-advancement=1 stop-at-contact=1");
#endif
    LOG_INFO("SIMULATION CHECK OK collision=%d physics_hz=%d",ENGINE_COLLISION,RUNTIME_PHYSICS_HZ);
}
#if ENGINE_RENDER
Mesh Cube(Renderer& r) {
    const float positions[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    const int faces[6][4]={{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
    const XMFLOAT3 normals[6]={{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
#if ENGINE_PBR
    std::vector<PbrVertex> vertices;
#else
    std::vector<Vertex> vertices;
#endif
    std::vector<uint32_t> indices;
    for(int face=0;face<6;++face) {
        uint32_t start=static_cast<uint32_t>(vertices.size());
        for(int n=0;n<4;++n) {
            auto p=positions[faces[face][n]];XMFLOAT3 pos{p[0]*.5f,p[1]*.5f,p[2]*.5f};
#if ENGINE_PBR
            XMFLOAT4 tangent=face==2||face==3?XMFLOAT4{0,0,1,1}:XMFLOAT4{1,0,0,1};
            vertices.push_back({pos,normals[face],tangent,{n==1||n==2?1.f:0.f,n>=2?1.f:0.f}});
#else
            const float shade=.55f+.07f*face;
            vertices.push_back({pos,normals[face],{.3f*shade,.8f*shade,shade,1}});
#endif
        }
        for(uint32_t index:{0u,1u,2u,0u,2u,3u}) indices.push_back(start+index);
    }
#if ENGINE_PBR
    return r.CreatePbrMesh(vertices,indices);
#else
    return r.CreateMesh(vertices,indices);
#endif
}
#if ENGINE_MODEL_GEOMETRY
Mesh ModelMesh(Renderer& renderer,const geometry::TriangleMesh& geometry,Mesh* update=nullptr){
    std::vector<uint32_t> indices;
#if ENGINE_PBR
    std::vector<PbrVertex> vertices;
#else
    std::vector<Vertex> vertices;
#endif
    for(size_t i=0;i<geometry.indices.size();i+=3){
        auto a=XMLoadFloat3(&geometry.positions[geometry.indices[i]]),b=XMLoadFloat3(&geometry.positions[geometry.indices[i+1]]),c=XMLoadFloat3(&geometry.positions[geometry.indices[i+2]]);
        XMFLOAT3 normal;XMStoreFloat3(&normal,XMVector3Normalize(XMVector3Cross(b-a,c-a)));
        for(int j=0;j<3;++j){
            indices.push_back(static_cast<uint32_t>(vertices.size()));auto position=geometry.positions[geometry.indices[i+j]];
#if ENGINE_PBR
            vertices.push_back({position,normal,{1,0,0,1},{0,0}});
#else
            vertices.push_back({position,normal,{.9f,.35f,.15f,1}});
#endif
        }
    }
#if ENGINE_MODEL_ANIMATION
    if(update){
        const auto bytes=vertices.size()*sizeof(vertices[0]);
        if(bytes!=update->vbv.SizeInBytes||indices.size()!=update->indexCount)throw std::runtime_error("Animated topology changed");
        void* mapped=nullptr;D3D12_RANGE noRead{0,0};DX_CHECK(update->vertexBuffer->Map(0,&noRead,&mapped));
        std::memcpy(mapped,vertices.data(),bytes);update->vertexBuffer->Unmap(0,nullptr);return *update;
    }
#endif
#if ENGINE_PBR
    return renderer.CreatePbrMesh(vertices,indices);
#else
    return renderer.CreateMesh(vertices,indices);
#endif
}
#endif
struct Graphics {
    Renderer renderer;
    Mesh cube, crosshair;
#if ENGINE_RAGDOLL
    std::array<Mesh,3> ragdollFrames;
#if ENGINE_RT_MESH
    uint32_t rayRagdoll=0;
#endif
#endif
#if ENGINE_MODEL_GEOMETRY
#if ENGINE_MODEL_ANIMATION
    std::array<Mesh,3> modelFrames;
#else
    Mesh model;
#endif
#endif
#if ENGINE_RT_MESH
    uint32_t rayModel=0;
#endif
#if ENGINE_PBR
    PbrMaterial material;
#endif
    ~Graphics() { renderer.Shutdown(); } // Drain before mesh members release GPU resources.
    void Init(Window& w) {
        if(!renderer.Initialize(w.GetHandle(),RUNTIME_WIDTH,RUNTIME_HEIGHT)) throw std::runtime_error("Renderer init failed");
        renderer.SetShadowEnabled(ENGINE_SHADOWS != 0);renderer.SetFog(false);
        bool lit=std::string_view(RUNTIME_LIGHTS)=="directional";
        float ambient=std::string_view(RUNTIME_INDIRECT_LIGHT)=="ambient"?.25f:0.f;
        renderer.SetDirectionalLight({.4f,-1,.3f},{1,1,1},lit?1.f:0.f,{ambient,ambient,ambient});
        cube=Cube(renderer);crosshair=renderer.CreateCrosshairMesh();
#if ENGINE_RAGDOLL
        for(auto& frame:ragdollFrames)frame=ModelMesh(renderer,ModelGeometry());
#if ENGINE_RT_MESH
        rayRagdoll=renderer.RegisterRayMesh(ModelGeometry());
#endif
#endif
#if ENGINE_MODEL_GEOMETRY
#if ENGINE_MODEL_ANIMATION
        for(auto& frame:modelFrames)frame=ModelMesh(renderer,ModelGeometry());
#else
        model=ModelMesh(renderer,ModelGeometry());
#endif
#endif
#if ENGINE_RT_MESH
        rayModel=renderer.RegisterRayMesh(ModelGeometry());
#endif
#if ENGINE_PBR
        material=renderer.CreatePbrMaterial(nullptr,nullptr,nullptr);
        material.baseColorFactor={.2f,.65f,.9f,1};material.metallicFactor=.2f;material.roughnessFactor=.4f;
#endif
    }
    void Draw(const Scene& scene) {
        Camera camera;camera.SetPosition(scene.Eye());camera.SetRotation(scene.yaw,scene.pitch);
        renderer.BeginFrame(camera);
#if ENGINE_RAGDOLL
        auto& ragdollMesh=ragdollFrames.at(renderer.FrameSlot());ModelMesh(renderer,scene.ragdollGeometry,&ragdollMesh);
#if ENGINE_RT_MESH
        renderer.UpdateRayMesh(rayRagdoll,scene.ragdollGeometry);
#endif
#endif
#if ENGINE_MODEL_ANIMATION
        // MoveToNextFrame has already waited on this frame slot's fence.
        
#if ENGINE_MODEL_DEFORMING
        const auto& deformed=scene.modelGeometry;
#else
        auto deformed=ModelAnimation().Sample(scene.time);
#endif
        auto& model=modelFrames.at(renderer.FrameSlot());ModelMesh(renderer,deformed,&model);
#if ENGINE_RT_MESH
        renderer.UpdateRayMesh(rayModel,deformed);
#endif
#endif
#if ENGINE_RAY_TRACING
        std::vector<RayTracingScene::Box> rayBoxes;
        auto rayBox=[&](const XMMATRIX& world) {
            RayTracingScene::Box b;XMStoreFloat4x4(&b.world,world);b.color={.2f,.65f,.9f,1};rayBoxes.push_back(b);
        };
#endif
        auto box=[&](XMFLOAT3 p,XMFLOAT3 scale) {
            XMMATRIX world=XMMatrixScaling(scale.x,scale.y,scale.z)*XMMatrixTranslation(p.x,p.y,p.z);
#if ENGINE_RAY_TRACING
            rayBox(world);
#endif
#if ENGINE_PBR
            renderer.DrawPbrMesh(cube,material,world);
#else
            renderer.DrawMesh(cube,world);
#endif
        };
        box({0,-.1f,2},{24,.2f,24});box({0,1.5f,-.5f},{2,3,1});
        for(int i=0;i<5;++i) if(scene.active[i]) box(scene.targets[i],{1,1,1});
        if(thirdPerson) box(scene.player,{.6f,1.8f,.6f});
#if ENGINE_RIGID_BODY
        for(auto id:scene.dynamicBodies){
            auto pose=scene.rigid->Get(id);
            auto world=XMMatrixScaling(pose.size.x,pose.size.y,pose.size.z)*XMMatrixRotationQuaternion(XMLoadFloat4(&pose.rotation))*XMMatrixTranslation(pose.position.x,pose.position.y,pose.position.z);
#if ENGINE_RAY_TRACING
            rayBox(world);
#endif
#if ENGINE_PBR
            renderer.DrawPbrMesh(cube,material,world);
#else
            renderer.DrawMesh(cube,world);
#endif
        }
#endif
#if ENGINE_RAY_TRACING
#if ENGINE_RT_MESH
        RayTracingScene::Box modelInstance;XMStoreFloat4x4(&modelInstance.world,scene.ModelWorld());
        modelInstance.color={.9f,.35f,.15f,1};modelInstance.geometry=rayModel;rayBoxes.push_back(modelInstance);
#endif
#if ENGINE_RAGDOLL && ENGINE_RT_MESH
        RayTracingScene::Box ragInstance;XMStoreFloat4x4(&ragInstance.world,XMMatrixIdentity());
        ragInstance.color={.3f,.8f,.35f,1};ragInstance.geometry=rayRagdoll;rayBoxes.push_back(ragInstance);
#endif
        renderer.SetRayScene(rayBoxes);
#endif
#if ENGINE_MODEL_GEOMETRY
#if ENGINE_PBR
        auto modelMaterial=material;modelMaterial.baseColorFactor={.9f,.35f,.15f,1};
        renderer.DrawPbrMesh(model,modelMaterial,scene.ModelWorld());
#else
        renderer.DrawMesh(model,scene.ModelWorld());
#endif
#endif
#if ENGINE_RAGDOLL
#if ENGINE_PBR
        auto ragMaterial=material;ragMaterial.baseColorFactor={.3f,.8f,.35f,1};renderer.DrawPbrMesh(ragdollMesh,ragMaterial,XMMatrixIdentity());
#else
        renderer.DrawMesh(ragdollMesh,XMMatrixIdentity());
#endif
#endif
        renderer.DrawOverlay(crosshair);renderer.EndFrame();
    }
};
#endif
int Run(int argc,char** argv) {
    RunMetrics metrics;
    int frames=0;bool smoke=false,selfTest=false;std::filesystem::path metricsFile;
    for(int i=1;i<argc;++i) {
        std::string arg=argv[i];
        if(arg=="--verify-distribution") {
#if ENGINE_DISTRIBUTION_SIGNATURE
            std::puts("signed distribution verified");return 0;
#else
            throw std::runtime_error("This build does not enforce distribution signatures");
#endif
        }
        if(arg=="--describe") {std::puts(RuntimeDescription);return 0;}
        if(arg=="--smoke-test") {smoke=true;frames=8;}
        else if(arg=="--self-test") selfTest=true;
        else if(arg=="--frames" && i+1<argc) {
            std::string number=argv[++i];size_t used=0;frames=std::stoi(number,&used);
            if(used!=number.size() || frames<1 || frames>100000) throw std::runtime_error("--frames must be 1..100000");
        } else if(arg=="--metrics" && i+1<argc) metricsFile=std::filesystem::u8path(argv[++i]);
        else throw std::runtime_error("Options: --describe | --self-test | --smoke-test | --frames N | --metrics file.csv");
    }
#if ENGINE_DISTRIBUTION_SIGNATURE
    if(!metricsFile.empty()) {
        auto root=std::filesystem::weakly_canonical(distribution::ExecutableDirectory()).wstring();
        auto output=std::filesystem::weakly_canonical(std::filesystem::absolute(metricsFile)).wstring();
        const auto equal=[](const std::wstring& a,const std::wstring& b,size_t count){
            return a.size()>=count && b.size()>=count && CompareStringOrdinal(a.data(),static_cast<int>(count),b.data(),static_cast<int>(count),TRUE)==CSTR_EQUAL;
        };
        bool inside=output.size()==root.size() && equal(output,root,root.size());
        root+=L"\\";inside=inside || equal(output,root,root.size());
        if(inside) throw std::runtime_error("Metrics must be outside the signed package");
    }
#endif
    if(!metricsFile.empty() && !metrics.Configure(metricsFile,5,0)) throw std::runtime_error("Invalid metrics configuration");
    if(smoke || selfTest) CheckSimulation();
    if(selfTest && !smoke) return 0;
    LOG_INFO("SELECTED START game=%s render=%d collision=%d audio=%d pbr=%d shadows=%d",RUNTIME_GAME,ENGINE_RENDER,ENGINE_COLLISION,ENGINE_AUDIO,ENGINE_PBR,ENGINE_SHADOWS);
#if ENGINE_AUDIO
    if(!AudioPlayer::Get().Init()) throw std::runtime_error("Selected audio device initialization failed");
    metrics.Mark("audio_init_returned");
    if(smoke) AudioPlayer::Get().PlayHit();
    else AudioPlayer::Get().PlayBgm();
#endif
    Scene scene;
#if ENGINE_RENDER
    Window window;
    if(!window.Initialize(GetModuleHandleW(nullptr),RUNTIME_WIDTH,RUNTIME_HEIGHT,L"Creator starter")) throw std::runtime_error("Window init failed");
    Graphics graphics;graphics.Init(window);metrics.Mark("renderer_ready");
    bool tabHeld=false,actionHeld=false,kickHeld=false;
#else
    if(frames==0 && metricsFile.empty()) frames=300;
#endif
    metrics.SceneReady();auto previous=std::chrono::steady_clock::now();
    double accumulator=0;const double tick=1.0/RUNTIME_PHYSICS_HZ;int frame=0;
    bool pendingAction=false;
    while(frames==0 || frame<frames) {
#if ENGINE_RENDER
        if(!window.ProcessMessages()) break;
#endif
        if(metrics.FrameStart()) break;
        auto now=std::chrono::steady_clock::now();
        double dt=smoke?tick:std::min(.25,std::chrono::duration<double>(now-previous).count());previous=now;
        float x=0,z=0;
#if ENGINE_RENDER
        auto& input=window.GetInput();input.BeginFrame(window.GetHandle());
        bool tab=input.IsKeyDown(VK_TAB);
        if(tab&&!tabHeld&&!smoke) input.SetCursorLock(!input.IsCursorLocked(),window.GetHandle());tabHeld=tab;
        if(input.IsPadDown(GamepadButton::Options)) break;
        input.GetPadMove(x,z);
        x+=(input.IsKeyDown('D')?1.f:0.f)-(input.IsKeyDown('A')?1.f:0.f);
        z+=(input.IsKeyDown('W')?1.f:0.f)-(input.IsKeyDown('S')?1.f:0.f);
        float lx=0,ly=0;input.GetPadLook(lx,ly);scene.yaw+=lx*static_cast<float>(dt)*2;scene.pitch+=ly*static_cast<float>(dt)*2;
        if(input.IsCursorLocked()) {int mx=0,my=0;input.GetMouseDelta(mx,my);scene.yaw+=mx*.002f;scene.pitch-=my*.002f;}
        scene.pitch=std::clamp(scene.pitch,-1.3f,1.3f);
        bool action=input.IsKeyDown('E') || (input.IsCursorLocked()&&input.IsKeyDown(VK_LBUTTON)) || input.IsPadDown(GamepadButton::R2);
        pendingAction|=action&&!actionHeld;actionHeld=action;
        if(input.IsKeyDown('R')) scene=Scene{};
#if ENGINE_RIGID_BODY
        bool kick=input.IsKeyDown(VK_SPACE);if(kick&&!kickHeld)scene.Kick();kickHeld=kick;
#endif
#endif
        accumulator+=dt;
        while(accumulator>=tick) {
            scene.Step(x,z,static_cast<float>(tick));accumulator-=tick;
            if(pendingAction) {scene.Act();pendingAction=false;}
        }
#if ENGINE_RENDER
        graphics.Draw(scene);
        std::wstring title=std::filesystem::path(RUNTIME_GAME).wstring()+L" starter | WASD / Tab: mouse / E or click: action / R: reset | score="+std::to_wstring(scene.score);
        SetWindowTextW(window.GetHandle(),title.c_str());
#else
        if(!smoke) std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
        ++frame;
    }
#if ENGINE_RENDER
    window.GetInput().SetCursorLock(false,window.GetHandle());graphics.renderer.Shutdown();
#endif
#if ENGINE_AUDIO
    AudioPlayer::Get().Shutdown();
#endif
    metrics.Mark("shutdown_complete");if(!metrics.Save()) throw std::runtime_error("Metrics save failed");
    LOG_INFO("SELECTED EXIT frames=%d",frame);return 0;
}
}
int main(int argc,char** argv) {
    EnableConsoleUtf8();
    try {
#if ENGINE_DISTRIBUTION_SIGNATURE
        distribution::Guard verified(distribution::ExecutableDirectory());
        // Verification includes --describe and --self-test; there is no unsigned startup bypass.
#endif
        return Run(argc,argv);
    }
    catch(const std::exception& ex) {
#if ENGINE_AUDIO
        AudioPlayer::Get().Shutdown();
#endif
        std::fprintf(stderr,"Selected runtime failed: %s\n",ex.what());return 2;
    }
}
