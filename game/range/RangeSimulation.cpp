#include "game/range/RangeSimulation.h"
#include "engine/core/Json.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <stdexcept>

namespace rangegame {
namespace {
constexpr double PI = 3.14159265358979323846;
constexpr size_t MAX_REPLAY_TICKS = 72000;
json::Value Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::string text((std::istreambuf_iterator<char>(in)), {}), error;
    if (text.size() > 4 * 1024 * 1024) throw std::runtime_error("range data too large");
    json::Value root;
    if (!json::Parse(text.data(), text.size(), root, &error) || !root.IsObject())
        throw std::runtime_error("invalid range JSON: " + error);
    return root;
}
double Number(const json::Value& node, const char* key, double lo, double hi) {
    const auto& value = node[key];
    if (!value.IsNumber() || !std::isfinite(value.number) || value.number < lo || value.number > hi)
        throw std::runtime_error(std::string("invalid field: ") + key);
    return value.number;
}
int Integer(const json::Value& node, const char* key, int lo, int hi) {
    const double value = Number(node, key, lo, hi);
    if (std::floor(value) != value) throw std::runtime_error("integer field required");
    return static_cast<int>(value);
}
double RayBox(double ox, double oy, double oz, double dx, double dy, double dz, const Box& b) {
    double nearT=0, farT=100;
    const double origins[]={ox,oy,oz}, dirs[]={dx,dy,dz}, centers[]={b.x,b.y,b.z}, sizes[]={b.sx,b.sy,b.sz};
    for (int axis=0; axis<3; ++axis) {
        const double lo=centers[axis]-sizes[axis]*0.5, hi=centers[axis]+sizes[axis]*0.5;
        if (std::abs(dirs[axis]) < 1e-12) {
            if (origins[axis]<lo || origins[axis]>hi) return 101;
        } else {
            double a=(lo-origins[axis])/dirs[axis], c=(hi-origins[axis])/dirs[axis];
            if (a>c) std::swap(a,c);
            nearT=std::max(nearT,a); farT=std::min(farT,c);
            if (nearT>farT) return 101;
        }
    }
    return nearT;
}
}
bool LoadSpec(const std::filesystem::path& path, Spec& spec, std::string& error) {
    try {
        auto root=Read(path); Spec next;
        next.version=Integer(root,"version",1,1);
        next.tickRate=Integer(root,"tickRate",120,120);
        next.durationTicks=Integer(root,"durationTicks",7200,7200); // v1は60秒、HUDも同じ仕様。
        next.cooldownTicks=Integer(root,"cooldownTicks",1,120);
        next.speed=Number(root,"speed",0.1,10); next.radius=Number(root,"radius",0.2,1);
        next.eyeHeight=Number(root,"eyeHeight",1,2); next.extent=Number(root,"extent",10,10);
        next.spawnX=Number(root,"spawnX",-next.extent+next.radius,next.extent-next.radius);
        next.spawnZ=Number(root,"spawnZ",-next.extent+next.radius,next.extent-next.radius);
        for (const char* key : {"walls","targets"}) {
            const auto& array=root[key];
            if (!array.IsArray() || array.Size()>64 || (std::string(key)=="targets" && array.Size()!=10))
                throw std::runtime_error("expected 10 targets and at most 64 walls");
            auto& dest=std::string(key)=="targets" ? next.targets : next.walls;
            for (const auto& v:array.items) {
                Box b;
                b.x=Number(v,"x",-50,50); b.y=Number(v,"y",0,10); b.z=Number(v,"z",-50,50);
                b.sx=Number(v,"sx",0.05,100); b.sy=Number(v,"sy",0.05,20); b.sz=Number(v,"sz",0.05,100);
                b.r=Number(v,"r",0,1); b.g=Number(v,"g",0,1); b.b=Number(v,"b",0,1);
                dest.push_back(b);
            }
        }
        spec=std::move(next); return true;
    } catch (const std::exception& ex) { error=ex.what(); return false; }
}
bool LoadReplay(const std::filesystem::path& path, std::vector<Command>& commands, std::string& error) {
    try {
        auto root=Read(path);
        Integer(root,"version",1,1);
        if (!root["segments"].IsArray()) throw std::runtime_error("segments required");
        std::vector<Command> result;
        for (const auto& row:root["segments"].items) {
            int ticks=Integer(row,"ticks",1,static_cast<int>(MAX_REPLAY_TICKS));
            if (result.size()+ticks>MAX_REPLAY_TICKS) throw std::runtime_error("replay exceeds 72000 ticks");
            Command c;
            c.moveX=Number(row,"moveX",-1,1); c.moveZ=Number(row,"moveZ",-1,1);
            c.yawDelta=Number(row,"yawDelta",-180,180); c.pitchDelta=Number(row,"pitchDelta",-160,160);
            if (!row["fire"].IsBool() || !row["restart"].IsBool()) throw std::runtime_error("boolean field required");
            c.fire=row["fire"].boolean; c.restart=row["restart"].boolean;
            result.insert(result.end(),ticks,c);
        }
        if (result.empty()) throw std::runtime_error("empty replay");
        commands=std::move(result); return true;
    } catch (const std::exception& ex) { error=ex.what(); return false; }
}
Simulation::Simulation(Spec spec):m_spec(std::move(spec)) { Reset(); }
const Spec& Simulation::Config() const { return m_spec; }
const State& Simulation::Current() const { return m_state; }
void Simulation::Reset() { m_state=State{}; m_state.x=m_spec.spawnX; m_state.z=m_spec.spawnZ; }
bool Simulation::CanStand(double x, double z) const {
    for (const auto& b:m_spec.walls)
        if (x>b.x-b.sx*.5-m_spec.radius && x<b.x+b.sx*.5+m_spec.radius &&
            z>b.z-b.sz*.5-m_spec.radius && z<b.z+b.sz*.5+m_spec.radius) return false;
    return true;
}
void Simulation::Step(const Command& c) {
    if (c.restart) { Reset(); return; }
    auto& s=m_state; s.lastHit=-1; s.fired=false;
    if (s.status!=0) return;
    s.yaw+=c.yawDelta;
    while (s.yaw>180) s.yaw-=360;
    while (s.yaw<=-180) s.yaw+=360;
    s.pitch=std::clamp(s.pitch+c.pitchDelta,-80.0,80.0);
    const double yaw=s.yaw*PI/180, pitch=s.pitch*PI/180;
    const double norm=std::max(1.0,std::sqrt(c.moveX*c.moveX+c.moveZ*c.moveZ));
    const double step=m_spec.speed/m_spec.tickRate/norm;
    // X→Z の順で分離。1tickの移動は膨張AABBの厚みより短く、壁を通り抜けない。
    const double nx=std::clamp(s.x+(c.moveX*std::cos(yaw)+c.moveZ*std::sin(yaw))*step,
                              -m_spec.extent+m_spec.radius,m_spec.extent-m_spec.radius);
    if (CanStand(nx,s.z)) s.x=nx;
    const double nz=std::clamp(s.z+(-c.moveX*std::sin(yaw)+c.moveZ*std::cos(yaw))*step,
                              -m_spec.extent+m_spec.radius,m_spec.extent-m_spec.radius);
    if (CanStand(s.x,nz)) s.z=nz;
    if (c.fire && s.tick>=s.nextShot) {
        s.fired=true; ++s.shots; s.nextShot=s.tick+m_spec.cooldownTicks;
        const double dx=std::sin(yaw)*std::cos(pitch), dy=std::sin(pitch), dz=std::cos(yaw)*std::cos(pitch);
        double nearest=101;
        for (const auto& wall:m_spec.walls)
            nearest=std::min(nearest,RayBox(s.x,m_spec.eyeHeight,s.z,dx,dy,dz,wall));
        int hit=-1;
        for (size_t i=0; i<m_spec.targets.size(); ++i) {
            if (s.mask & (1<<i)) continue;
            double distance=RayBox(s.x,m_spec.eyeHeight,s.z,dx,dy,dz,m_spec.targets[i]);
            // 同距離なら壁を優先。遮蔽物より奥の的は撃てない。
            if (distance<nearest) { nearest=distance; hit=static_cast<int>(i); }
        }
        if (hit>=0) { s.mask|=1<<hit; ++s.hits; s.lastHit=hit; }
    }
    ++s.tick;
    if (s.hits==static_cast<int>(m_spec.targets.size())) s.status=1;
    else if (s.tick>=m_spec.durationTicks) s.status=2;
}
bool SaveTrace(const std::filesystem::path& path,const std::vector<TraceRow>& rows) {
    if (path.empty()) return true;
    std::ofstream out(path); if (!out) return false;
    out.imbue(std::locale::classic()); out<<std::setprecision(17);
    out<<"step,tick,x,z,yaw,pitch,hits,shots,mask,status\n";
    for (const auto& row:rows) { const auto& s=row.state;
        out<<row.step<<','<<s.tick<<','<<s.x<<','<<s.z<<','<<s.yaw<<','<<s.pitch<<','<<s.hits<<','<<s.shots<<','<<s.mask<<','<<s.status<<'\n'; }
    out.flush(); return out.good();
}
}
