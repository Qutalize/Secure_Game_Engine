#include "engine/security/SecurityPolicy.h"
#include "engine/network/CardProtocol.h"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <vector>
void Check(bool ok){if(!ok)throw std::runtime_error("Test assertion failed");}
void Reject(const std::function<void()>& fn) {bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}Check(rejected);}
int main() {
    try {
        using namespace security;
        for(unsigned mask=0;mask<16;++mask) {
            Policy p{bool(mask&1),bool(mask&2),bool(mask&4),bool(mask&8)};
            Validate(p,"fps",false);Validate(p,"hide",false);
            if(mask&12)Reject([&]{Validate(p,"card",false);});else Validate(p,"card",false);
            if(mask)Reject([&]{Validate(p,"range",false);});else Validate(p,"range",false);
        }
        for(auto input:{"","none,shuffle","watchdog,watchdog","shuffle,","visibility-filter"})Reject([&]{Parse(input);});
        auto p=Parse("shuffle,tag-mask");Check(p.shuffle&&p.tagMask&&!p.watchdog&&!p.depthGuard);
        Reject([]{Validate(Parse("private-hands"),"card",false);});
        Reject([]{Validate(Parse("authority"),"card",true);});
        Reject([]{Validate(Parse("none"),"card",true);});
        Validate(Parse("authority,private-hands"),"card",true);
        Check(Defaults("card",false,false).watchdog);Check(!Defaults("fps",false,false).shuffle);
        Check(Defaults("fps",false,true).depthGuard);Check(Defaults("card",true,false).privateHands);
        auto wire=cardnet::Encode(cardnet::Operation::Draw,13,0x0102030405060708ull,0x1112131415161718ull);
        Check(wire[5]==3&&wire[6]==13&&wire[8]==8&&wire[15]==1&&wire[16]==0x18&&wire[23]==0x11);
        Check(std::all_of(wire.begin()+24,wire.end(),[](auto b){return b==0;}));
        std::array<uint8_t,64> reply{};reply[0]='A';reply[1]='C';reply[2]='A';reply[3]='R';reply[4]=1;reply[5]=128;
        reply[8]=1;reply[28]=reply[29]=reply[30]=255;reply[38]=1;
        Check(cardnet::Decode(reply).phase==0);
        for(size_t n=0;n<64;++n)Reject([&]{cardnet::Decode(std::span(reply.data(),n));});
        auto bad=reply;bad[63]=52;Reject([&]{cardnet::Decode(bad);});
        bad=reply;bad[31]=15;Reject([&]{cardnet::Decode(bad);});
        bad=reply;bad[32]=1;Reject([&]{cardnet::Decode(bad);});
        puts("PASS security policy matrix, dependencies, wire endian and snapshot rejection");return 0;
    }catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
