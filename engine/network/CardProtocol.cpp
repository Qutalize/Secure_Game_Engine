#include "engine/network/CardProtocol.h"
#include <algorithm>
namespace cardnet {
namespace {
void Put64(uint8_t* p,uint64_t n) { for(int i=0;i<8;++i) p[i]=static_cast<uint8_t>(n>>(8*i)); }
uint64_t Get64(const uint8_t* p,int count=8) { uint64_t n=0; for(int i=0;i<count;++i) n|=uint64_t(p[i])<<(8*i); return n; }
void Require(bool ok) { if(!ok) throw std::runtime_error("Invalid private-card snapshot"); }
}
std::array<uint8_t,RequestSize> Encode(Operation op,uint8_t arg,uint64_t seq,uint64_t rev,const std::array<uint8_t,16>& token) {
    std::array<uint8_t,RequestSize> b{};
    b[0]='A';b[1]='C';b[2]='A';b[3]='R';b[4]=1;b[5]=static_cast<uint8_t>(op);b[6]=arg;
    Put64(b.data()+8,seq);Put64(b.data()+16,rev);std::copy(token.begin(),token.end(),b.begin()+24);return b;
}
Snapshot Decode(std::span<const uint8_t> b) {
    Require(b.size()==ResponseSize);
    Require(b[0]=='A'&&b[1]=='C'&&b[2]=='A'&&b[3]=='R'&&b[4]==1&&b[5]==128&&b[6]<=6&&b[7]<4);
    Snapshot s;
    s.error=b[6];s.seat=b[7];s.nextSequence=Get64(b.data()+8);s.revision=Get64(b.data()+16);
    s.round=static_cast<uint32_t>(Get64(b.data()+24,4));s.turn=b[28];s.victim=b[29];s.loser=b[30];s.ownCount=b[31];
    std::copy_n(b.begin()+32,4,s.counts.begin());s.discarded=b[36];s.phase=b[37];s.humans=b[38];
    Require(s.nextSequence>0&&s.ownCount<=14&&s.discarded<=26&&s.phase<=2&&b[39]==0);
    Require((s.humans==1||s.humans==3||s.humans==7||s.humans==15)&&(s.humans&(1<<s.seat)));
    int total=2*s.discarded;
    for(auto n:s.counts) { Require(n<=14);total+=n; }
    Require(s.counts[s.seat]==s.ownCount);
    std::array<bool,53> seen{};
    for(size_t i=0;i<s.ownCount;++i) { auto card=b[40+i];Require(card<=52&&!seen[card]);seen[card]=true;s.own[i]=card; }
    for(size_t i=40+s.ownCount;i<b.size();++i) Require(b[i]==0);
    if(!s.round) Require(s.phase==0&&total==0&&s.turn==None&&s.victim==None&&s.loser==None);
    else {
        Require(total==53);
        if(s.loser==None) Require(s.phase!=2&&s.turn<4&&s.victim<4&&s.turn!=s.victim&&s.counts[s.turn]>0&&s.counts[s.victim]>0);
        else Require(s.loser<4&&s.turn==None&&s.victim==None&&s.discarded==26&&s.counts[s.loser]==1&&s.phase!=1);
    }
    return s;
}
}
