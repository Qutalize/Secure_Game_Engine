#include "engine/network/CardClient.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <charconv>
namespace {
unsigned Number(const std::string& s,unsigned max) {
    unsigned value=0;auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),value);
    if(error!=std::errc{}||end!=s.data()+s.size()||value>max)throw std::runtime_error("Invalid numeric option");
    return value;
}
}
int main(int argc,char** argv) {
    try {
        cardnet::Options options;std::filesystem::path outPath,timingPath;bool play=false;
        unsigned rounds=1,frames=100,seconds=60;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];if(arg=="--autoplay"){play=true;continue;}
            if(++i>=argc)throw std::runtime_error("Missing option value");std::string value=argv[i];
            if(arg=="--host")options.host=value;
            else if(arg=="--port")options.port=static_cast<uint16_t>(Number(value,65535));
            else if(arg=="--seat")options.seat=static_cast<uint8_t>(Number(value,3));
            else if(arg=="--token-file")options.tokenFile=value;
            else if(arg=="--cert")options.certificateFile=value;
            else if(arg=="--out")outPath=value;
            else if(arg=="--timings")timingPath=value;
            else if(arg=="--rounds")rounds=Number(value,100);
            else if(arg=="--frames")frames=Number(value,100000);
            else if(arg=="--seconds")seconds=Number(value,600);
            else throw std::runtime_error("Unknown probe option");
        }
        if(outPath.empty()||std::filesystem::exists(outPath)||!rounds||!frames||!seconds)
            throw std::runtime_error("Use a new --out file and positive limits");
        std::ofstream out(outPath,std::ios::binary);
        if(!out)throw std::runtime_error("Cannot open wire output");
        std::ofstream timings;
        if(!timingPath.empty()) {
            if(std::filesystem::exists(timingPath))throw std::runtime_error("Use a new timing output");
            timings.open(timingPath);if(!timings)throw std::runtime_error("Cannot open timing output");
            timings<<"sample,operation,elapsed_ms\n";timings.precision(9);
        }
        auto record=[&](unsigned index,const char* op,auto begin) {
            if(timings.is_open()) {
                auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
                timings<<index<<','<<op<<','<<elapsed<<'\n';
                if(!timings)throw std::runtime_error("Timing output failed");
            }
        };
        cardnet::Connection client;auto began=std::chrono::steady_clock::now();auto state=client.Connect(options);
        record(0,"connect_tls_hello",began);
        auto save=[&]{const auto& b=client.LastWire();out.write(reinterpret_cast<const char*>(b.data()),b.size());
            if(!out)throw std::runtime_error("Wire output failed");};save();
        auto start=std::chrono::steady_clock::now();unsigned count=1,choice=options.seat+17;
        while(std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds)) {
            if(play&&state.phase==2&&state.round>=rounds) {
                printf("CARD PROBE COMPLETE seat=%u round=%u frames=%u\n",unsigned(state.seat),state.round,count);return 0;
            }
            if(!play&&count>=frames)return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto exchangeBegin=std::chrono::steady_clock::now();const char* operation="poll";
            if(play&&state.phase==2){operation="restart";state=client.Exchange(cardnet::Operation::Restart,0,state.nextSequence,state.revision);}
            else if(play&&state.phase==1&&state.turn==state.seat) {
                operation="draw";
                choice=choice*1664525u+1013904223u;
                auto index=static_cast<uint8_t>(choice%state.counts[state.victim]);
                state=client.Exchange(cardnet::Operation::Draw,index,state.nextSequence,state.revision);
            } else state=client.Exchange(cardnet::Operation::Poll);
            record(count,operation,exchangeBegin);
            save();++count;
        }
        fprintf(stderr,"Card probe timed out before completion\n");return 3;
    } catch(const std::exception& e){fprintf(stderr,"Card probe failed: %s\n",e.what());return 1;}
}
