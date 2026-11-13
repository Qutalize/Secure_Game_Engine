#include "game/modes/RangeMode.h"
#include "engine/core/Window.h"
#include "engine/core/Logger.h"
#include "game/OverlayText.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;
namespace {
// 両版で同じ色/面ごとの明るさを使う。テクスチャ・ライト・影・音声は初版では使わない。
Mesh BoxMesh(Renderer& renderer,const rangegame::Box& b) {
    std::vector<Vertex> vertices; std::vector<uint32_t> indices;
    const float signs[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    const int faces[6][4]={{0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0}};
    const float shade[6]={1,.65f,.8f,.9f,1,.55f};
    for (int face=0;face<6;++face) {
        const uint32_t start=static_cast<uint32_t>(vertices.size());
        for (int corner:faces[face]) {
            const auto& p=signs[corner];
            vertices.push_back({{static_cast<float>(b.x+p[0]*b.sx*.5),static_cast<float>(b.y+p[1]*b.sy*.5),static_cast<float>(b.z+p[2]*b.sz*.5)},
                                {0,1,0},{static_cast<float>(b.r*shade[face]),static_cast<float>(b.g*shade[face]),static_cast<float>(b.b*shade[face]),1}});
        }
        for (uint32_t i:{0u,1u,2u,0u,2u,3u}) indices.push_back(start+i);
    }
    return renderer.CreateMesh(vertices,indices);
}
void Quad(std::vector<Vertex>& v,std::vector<uint32_t>& i,float x,float y,float w,float h,XMFLOAT4 c) {
    uint32_t b=static_cast<uint32_t>(v.size());
    for (auto p:{XMFLOAT3{x,y,0},XMFLOAT3{x+w,y,0},XMFLOAT3{x+w,y+h,0},XMFLOAT3{x,y+h,0}}) v.push_back({p,{0,0,-1},c});
    for (uint32_t n:{0u,1u,2u,0u,2u,3u}) i.push_back(b+n);
}
Mesh HudMesh(Renderer& r,int score,int seconds,int crosshair) {
    std::vector<Vertex> v; std::vector<uint32_t> i;
    if (score>=0) for (int dot=0;dot<10;++dot)
        Quad(v,i,-.92f+dot*.085f,.82f,.06f,.045f,dot<score?XMFLOAT4{1,.65f,.15f,1}:XMFLOAT4{.2f,.25f,.3f,1});
    if (seconds>=0) {
        Quad(v,i,.25f,.82f,.67f,.045f,{.15f,.2f,.25f,1});
        if (seconds>0) Quad(v,i,.25f,.82f,.67f*seconds/60,.045f,{.2f,.85f,.75f,1});
    }
    if (crosshair>=0) {
        XMFLOAT4 color=crosshair ? XMFLOAT4{.2f,1,.4f,1}:XMFLOAT4{1,1,1,1};
        Quad(v,i,-.0015f,-.017f,.003f,.034f,color);
        Quad(v,i,-.0096f,-.0027f,.0192f,.0054f,color);
    }
    return r.CreateMesh(v,i);
}
}
RangeMode::RangeMode(RangeOptions options):m_options(std::move(options)) {}
RangeMode::~RangeMode()=default;
const char* RangeMode::GetName() const { return "range"; }
bool RangeMode::Init(const GameContext& ctx) {
    rangegame::Spec spec; std::string error;
    if (!rangegame::LoadSpec(m_options.data,spec,error) ||
        (!m_options.replay.empty() && !rangegame::LoadReplay(m_options.replay,m_replay,error))) {
        LOG_ERROR("Range: %s",error.c_str()); return false;
    }
    if (m_options.exitAfterReplay && m_replay.empty()) { LOG_ERROR("--exit-after-replay requires --replay"); return false; }
    m_sim=std::make_unique<rangegame::Simulation>(spec); m_window=ctx.window;
    RECT client{};
    if (!GetClientRect(m_window->GetHandle(),&client) || client.right-client.left!=1600 || client.bottom-client.top!=900)
        throw std::runtime_error("Range requires a 1600x900 client area");
    if (!m_options.stateOutput.empty()) m_trace.reserve(72000);
    auto& r=*ctx.renderer;
    r.SetShadowEnabled(false); r.SetFog(false);
    r.SetDirectionalLight({0,-1,0},{1,1,1},0,{1,1,1});
    m_floor=BoxMesh(r,{0,-.1,0,20,.2,20,.16,.21,.25});
    for (const auto& wall:spec.walls) m_walls.push_back(BoxMesh(r,wall));
    for (const auto& target:spec.targets) m_targets.push_back(BoxMesh(r,target));
    for (int hits=0;hits<=10;++hits) m_scores.push_back(HudMesh(r,hits,-1,-1));
    for (int sec=0;sec<=60;++sec) m_timers.push_back(HudMesh(r,-1,sec,-1));
    m_crosshair=HudMesh(r,-1,-1,0); m_hitCrosshair=HudMesh(r,-1,-1,1);
    m_win=overlaytext::Build(r,r.GetAspect(),{"CLEAR !","R=RETRY","ESC=QUIT"});
    m_lose=overlaytext::Build(r,r.GetAspect(),{"TIME UP","R=RETRY","ESC=QUIT"});
    SetWindowTextW(m_window->GetHandle(),L"RANGE 60 | Tab: mouse  R: retry  ESC: quit");
    LOG_INFO("RANGE SETTINGS: api=D3D12 resolution=1600x900 color=UNORM vsync=1 msaa=0");
    LOG_INFO("RANGE: 60秒で10個の的。WASD/左スティック=移動  右スティック=視点  R2/クリック=発砲  Tab=マウス捕捉  R/△=再開");
    return true;
}
void RangeMode::Update(float dt,const Input& input) {
    bool tab=input.IsKeyDown(VK_TAB), restart=input.IsKeyDown('R')||input.IsPadDown(GamepadButton::Triangle);
    if (tab && !m_tabHeld && m_replay.empty()) m_window->GetInput().SetCursorLock(!input.IsCursorLocked(),m_window->GetHandle());
    m_tabHeld=tab; m_restartPending|=restart&&!m_restartHeld; m_restartHeld=restart;
    if (m_replay.empty() && input.IsCursorLocked()) {
        int dx=0,dy=0; input.GetMouseDelta(dx,dy); m_mouseYaw+=dx*.08; m_mousePitch-=dy*.08;
    }
    // 長いフレームでも実時間を捨てず120Hzへ積む。Main側のrangeではdtを上限処理しない。
    m_accumulator+=dt;
    const double step=1.0/m_sim->Config().tickRate;
    while (m_accumulator>=step) {
        m_accumulator-=step; rangegame::Command command;
        if (!m_replay.empty()) {
            if (m_replayStep==m_replay.size()) {
                if (m_options.exitAfterReplay) PostQuitMessage(0);
                m_accumulator=0; break;
            }
            command=m_replay[m_replayStep++];
        } else {
            float mx=0,mz=0,lx=0,ly=0; input.GetPadMove(mx,mz); input.GetPadLook(lx,ly);
            command.moveX=std::clamp(static_cast<double>(mx)+(input.IsKeyDown('D')?1:0)-(input.IsKeyDown('A')?1:0),-1.0,1.0);
            command.moveZ=std::clamp(static_cast<double>(mz)+(input.IsKeyDown('W')?1:0)-(input.IsKeyDown('S')?1:0),-1.0,1.0);
            command.yawDelta=lx*120.0*step+m_mouseYaw; command.pitchDelta=ly*120.0*step+m_mousePitch;
            m_mouseYaw=m_mousePitch=0;
            command.fire=input.IsPadDown(GamepadButton::R2) || (input.IsCursorLocked()&&input.IsKeyDown(VK_LBUTTON));
            command.restart=m_restartPending; m_restartPending=false;
        }
        m_sim->Step(command);
        if (m_flash>0) --m_flash;
        if (m_sim->Current().lastHit>=0) m_flash=12;
        if (!m_options.stateOutput.empty()) {
            if (m_trace.size()==72000) throw std::runtime_error("Range trace limit reached");
            m_trace.push_back({m_steps,m_sim->Current()});
        }
        ++m_steps;
    }
    const auto& s=m_sim->Current();
    m_camera.SetPosition({static_cast<float>(s.x),static_cast<float>(m_sim->Config().eyeHeight),static_cast<float>(s.z)});
    m_camera.SetRotation(XMConvertToRadians(static_cast<float>(s.yaw)),XMConvertToRadians(static_cast<float>(s.pitch)));
}
void RangeMode::Render(Renderer& r) {
    const auto& s=m_sim->Current();
    r.BeginFrame(m_camera); auto identity=XMMatrixIdentity();
    r.DrawMesh(m_floor,identity);
    for (const auto& mesh:m_walls) r.DrawMesh(mesh,identity);
    for (size_t i=0;i<m_targets.size();++i) if (!(s.mask&(1<<i))) r.DrawMesh(m_targets[i],identity);
    r.DrawOverlay(m_scores[s.hits]);
    r.DrawOverlay(m_timers[std::clamp((m_sim->Config().durationTicks-s.tick+119)/120,0,60)]);
    r.DrawOverlay(m_flash>0?m_hitCrosshair:m_crosshair);
    if (s.status!=0) r.DrawOverlay(s.status==1?m_win:m_lose);
    r.EndFrame();
}
void RangeMode::Shutdown() {
    if (!rangegame::SaveTrace(m_options.stateOutput,m_trace)) throw std::runtime_error("cannot save range state trace");
    if (m_window) m_window->GetInput().SetCursorLock(false,m_window->GetHandle());
}
