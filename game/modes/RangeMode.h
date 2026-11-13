#pragma once
#include "game/IGameMode.h"
#include "game/range/RangeSimulation.h"
#include "engine/renderer/Renderer.h"
#include <memory>

struct RangeOptions {
    std::filesystem::path data, replay, stateOutput;
    bool exitAfterReplay=false;
};
class RangeMode final : public IGameMode {
public:
    explicit RangeMode(RangeOptions options);
    ~RangeMode() override;
    bool Init(const GameContext& ctx) override;
    void Update(float dt,const Input& input) override;
    void Render(Renderer& renderer) override;
    void Shutdown() override;
    const char* GetName() const override;
private:
    RangeOptions m_options;
    std::unique_ptr<rangegame::Simulation> m_sim;
    std::vector<rangegame::Command> m_replay;
    std::vector<rangegame::TraceRow> m_trace;
    std::vector<Mesh> m_walls,m_targets,m_scores,m_timers;
    Mesh m_floor,m_crosshair,m_hitCrosshair,m_win,m_lose;
    Camera m_camera;
    Window* m_window=nullptr; // Mainが所有するウィンドウを借用。
    double m_accumulator=0, m_mouseYaw=0, m_mousePitch=0;
    size_t m_replayStep=0;
    int m_steps=0,m_flash=0;
    bool m_tabHeld=false,m_restartHeld=false,m_restartPending=false;
};
