#pragma once

#include <filesystem>
#include <string>
#include <vector>

// 描画・OS・入力機器に依存しない射撃場のルール。Unity側RangeSimulation.csと対で検証する。
namespace rangegame {
struct Box {
    double x=0, y=0, z=0, sx=1, sy=1, sz=1, r=1, g=1, b=1;
};
struct Spec {
    int version=0, tickRate=120, durationTicks=7200, cooldownTicks=24;
    double speed=5, radius=0.3, eyeHeight=1.6, extent=10, spawnX=0, spawnZ=-8;
    std::vector<Box> walls, targets;
};
struct Command {
    double moveX=0, moveZ=0, yawDelta=0, pitchDelta=0;
    bool fire=false, restart=false;
};
struct State {
    int tick=0, hits=0, shots=0, mask=0, nextShot=0, status=0, lastHit=-1;
    double x=0, z=0, yaw=0, pitch=0;
    bool fired=false;
};
struct TraceRow { int step=0; State state; };
class Simulation {
public:
    explicit Simulation(Spec spec);
    void Reset();
    void Step(const Command& command);
    const Spec& Config() const;
    const State& Current() const;
private:
    bool CanStand(double x, double z) const;
    Spec m_spec;
    State m_state;
};
bool LoadSpec(const std::filesystem::path& path, Spec& spec, std::string& error);
bool LoadReplay(const std::filesystem::path& path, std::vector<Command>& commands, std::string& error);
bool SaveTrace(const std::filesystem::path& path, const std::vector<TraceRow>& rows);
}
