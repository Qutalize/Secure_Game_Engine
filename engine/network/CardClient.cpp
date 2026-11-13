#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "engine/network/CardClient.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace cardnet {
namespace {
using Clock=std::chrono::steady_clock;
void Wait(SOCKET s,bool writing,Clock::time_point deadline) {
    auto us=std::chrono::duration_cast<std::chrono::microseconds>(deadline-Clock::now()).count();
    if(us<=0) throw std::runtime_error("Card server timeout");
    timeval t{static_cast<long>(us/1000000),static_cast<long>(us%1000000)};
    fd_set set,errors;FD_ZERO(&set);FD_ZERO(&errors);FD_SET(s,&set);FD_SET(s,&errors);
    int ready=select(0,writing?nullptr:&set,writing?&set:nullptr,&errors,&t);
    if(ready<=0||FD_ISSET(s,&errors)) throw std::runtime_error("Card server unavailable or timed out");
}
void Transfer(SOCKET s,uint8_t* bytes,size_t size,bool writing,Clock::time_point deadline) {
    size_t done=0;
    while(done<size) {
        Wait(s,writing,deadline);
        int n=writing?send(s,reinterpret_cast<const char*>(bytes+done),static_cast<int>(size-done),0):
            recv(s,reinterpret_cast<char*>(bytes+done),static_cast<int>(size-done),0);
        if(n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) continue;
        if(n<=0) throw std::runtime_error("Card connection closed (authentication, seat or network failure)");
        done+=static_cast<size_t>(n);
    }
}
int Hex(char c) {
    if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;
    throw std::runtime_error("Invalid seat credential file");
}
}
Connection::~Connection(){Close();}
void Connection::Close() {
    m_tls.Close();
    if(m_socket!=~uintptr_t(0)){closesocket(static_cast<SOCKET>(m_socket));m_socket=~uintptr_t(0);}
    if(m_wsa){WSACleanup();m_wsa=false;}
}
Snapshot Connection::Connect(const Options& options) {
    Close();m_revision=0;m_seat=options.seat;
    if(m_seat>=4||options.host.empty()||!options.port)throw std::runtime_error("Invalid card endpoint or seat");
    std::ifstream file(options.tokenFile,std::ios::binary);
    // Exact-length credentials. Do not read an arbitrary-sized untrusted file into memory.
    std::array<char,33> text{};file.read(text.data(),text.size());
    if(file.gcount()!=32)throw std::runtime_error("Seat credential must contain exactly 32 hex characters");
    std::array<uint8_t,16> token{};
    for(int i=0;i<16;++i) token[i]=static_cast<uint8_t>(Hex(text[i*2])*16+Hex(text[i*2+1]));
    SecureZeroMemory(text.data(),text.size());
    auto hello=Encode(Operation::Hello,m_seat,0,0,token);SecureZeroMemory(token.data(),token.size());
    try {
        WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)!=0)throw std::runtime_error("WSAStartup failed");m_wsa=true;
        m_socket=static_cast<uintptr_t>(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
        if(m_socket==~uintptr_t(0))throw std::runtime_error("Card socket creation failed");
        SOCKET s=static_cast<SOCKET>(m_socket);u_long nonblocking=1;
        if(ioctlsocket(s,FIONBIO,&nonblocking)!=0)throw std::runtime_error("Card nonblocking socket failed");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(options.port);
        if(InetPtonA(AF_INET,options.host.c_str(),&address.sin_addr)!=1)throw std::runtime_error("--card-server requires a numeric IPv4 address");
        if(connect(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR) {
            if(WSAGetLastError()!=WSAEWOULDBLOCK)throw std::runtime_error("Card server connection failed");
            Wait(s,true,Clock::now()+std::chrono::seconds(2));
            int error=0,len=sizeof(error);
            if(getsockopt(s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len)!=0||error)
                throw std::runtime_error("Card server connection refused");
        }
        BOOL noDelay=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&noDelay),sizeof(noDelay));
        m_tls.Connect(m_socket,options.host,options.certificateFile);
        auto state=Transact(hello);SecureZeroMemory(hello.data(),hello.size());return state;
    } catch(...) { SecureZeroMemory(hello.data(),hello.size());Close();throw; }
}
Snapshot Connection::Transact(std::array<uint8_t,RequestSize>& request) {
    const auto deadline=Clock::now()+std::chrono::seconds(2);
    m_tls.Write(request,deadline);
    m_tls.Read(m_wire,deadline);
    auto state=Decode(m_wire);
    if(state.seat!=m_seat||state.revision<m_revision) throw std::runtime_error("Card snapshot seat/revision mismatch");
    m_revision=state.revision;return state;
}
Snapshot Connection::Exchange(Operation op,uint8_t arg,uint64_t seq,uint64_t rev) {
    auto request=Encode(op,arg,seq,rev);return Transact(request);
}
Client::~Client(){Stop();}
void Client::Start(const Options& options) {
    Stop();auto state=m_connection.Connect(options);
    {std::lock_guard lock(m_mutex);m_view={state,true,false,{}};m_command.reset();}
    m_stop=false;m_worker=std::thread(&Client::Run,this);
}
void Client::Stop() {
    m_stop=true;if(m_worker.joinable())m_worker.join();m_connection.Close();
    std::lock_guard lock(m_mutex);m_view.connected=false;m_view.pending=false;m_command.reset();
}
View Client::Read(){std::lock_guard lock(m_mutex);return m_view;}
bool Client::Queue(Operation op,uint8_t index,uint64_t rev) {
    std::lock_guard lock(m_mutex);
    if(!m_view.connected||m_view.pending||(op!=Operation::Draw&&op!=Operation::Restart))return false;
    m_command=Command{op,index,rev};m_view.pending=true;return true;
}
void Client::Run() {
    try {
        while(!m_stop.load()) {
            std::optional<Command> command;uint64_t seq;
            {std::lock_guard lock(m_mutex);command=m_command;m_command.reset();seq=m_view.state.nextSequence;}
            auto state=command?m_connection.Exchange(command->op,command->index,seq,command->revision):
                m_connection.Exchange(Operation::Poll);
            {std::lock_guard lock(m_mutex);m_view.state=state;
                if(command)m_view.pending=false;
                m_view.message=state.error?"Server rejected stale or invalid action; state refreshed":"";}
            for(int i=0;i<8&&!m_stop.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch(const std::exception& e) {
        std::lock_guard lock(m_mutex);m_view.connected=false;m_view.pending=false;m_view.message=e.what();
    }
}
}
