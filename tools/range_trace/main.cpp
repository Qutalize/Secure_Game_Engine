#include "game/range/RangeSimulation.h"
#include <cstdio>
int main(int argc,char** argv) {
    if (argc!=4) { std::fprintf(stderr,"range_trace SPEC REPLAY OUTPUT.csv\n"); return 2; }
    rangegame::Spec spec; std::vector<rangegame::Command> replay; std::string error;
    if (!rangegame::LoadSpec(argv[1],spec,error) || !rangegame::LoadReplay(argv[2],replay,error)) {
        std::fprintf(stderr,"%s\n",error.c_str()); return 2;
    }
    rangegame::Simulation sim(spec); std::vector<rangegame::TraceRow> rows;
    for (size_t i=0;i<replay.size();++i) { sim.Step(replay[i]); rows.push_back({static_cast<int>(i),sim.Current()}); }
    if (!rangegame::SaveTrace(argv[3],rows)) return 3;
    std::printf("range_trace: %zu ticks, hits=%d, status=%d\n",rows.size(),sim.Current().hits,sim.Current().status);
    return 0;
}
