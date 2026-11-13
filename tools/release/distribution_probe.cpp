#include "engine/security/DistributionIntegrity.h"
#include <chrono>
#include <cstdio>
#include <thread>
int main(int argc,char** argv) {
    if(argc<2||argc>3)return 2;
    try {
        auto start=std::chrono::steady_clock::now();distribution::Guard verified(argv[1]);
        auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        printf("VERIFIED files=%zu bytes=%llu elapsed_ms=%.6f\n",verified.FileCount(),static_cast<unsigned long long>(verified.Bytes()),ms);fflush(stdout);
        if(argc==3&&std::string(argv[2])=="--hold")std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    } catch(const std::exception& e){fprintf(stderr,"REJECTED: %s\n",e.what());return 1;}
}
