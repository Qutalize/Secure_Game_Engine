#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000006 // Windows 10 1809: SCH_CREDENTIALS (TLS 1.3 capable).
#include <winsock2.h>
#include <windows.h>
#include <winternl.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include "engine/network/TlsTransport.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstdio>

namespace cardnet {
namespace {
constexpr size_t kMaxEncryptedBytes=64*1024;
void Check(SECURITY_STATUS s,const char* step) {
    if(s!=SEC_E_OK) {char text[180];sprintf_s(text,"TLS %s failed (0x%08lx)",step,static_cast<unsigned long>(s));throw std::runtime_error(text);}
}
void Wait(SOCKET socket,bool write,TlsTransport::Deadline deadline) {
    auto left=std::chrono::duration_cast<std::chrono::microseconds>(deadline-std::chrono::steady_clock::now()).count();
    if(left<=0)throw std::runtime_error("TLS deadline exceeded");
    timeval t{static_cast<long>(left/1000000),static_cast<long>(left%1000000)};
    fd_set ready,errors;FD_ZERO(&ready);FD_ZERO(&errors);FD_SET(socket,&ready);FD_SET(socket,&errors);
    if(select(0,write?nullptr:&ready,write?&ready:nullptr,&errors,&t)<=0 || FD_ISSET(socket,&errors))
        throw std::runtime_error("TLS socket closed or timed out");
}
struct Tokens {
    SecBuffer data[2]={{0,SECBUFFER_TOKEN,nullptr},{0,SECBUFFER_ALERT,nullptr}};
    SecBufferDesc desc{SECBUFFER_VERSION,2,data};
    ~Tokens(){for(auto& b:data)if(b.pvBuffer)FreeContextBuffer(b.pvBuffer);}
};
}
struct TlsTransport::Impl {
    SOCKET socket=INVALID_SOCKET;
    CredHandle credential{};CtxtHandle context{};
    SecPkgContext_StreamSizes sizes{};
    std::vector<uint8_t> encrypted,plain;
    Impl(){SecInvalidateHandle(&credential);SecInvalidateHandle(&context);}
    ~Impl(){if(SecIsValidHandle(&context))DeleteSecurityContext(&context);if(SecIsValidHandle(&credential))FreeCredentialsHandle(&credential);}
    void Send(std::span<const uint8_t> bytes,Deadline deadline) {
        size_t sent=0;
        while(sent<bytes.size()) {
            Wait(socket,true,deadline);
            int n=send(socket,reinterpret_cast<const char*>(bytes.data()+sent),static_cast<int>(bytes.size()-sent),0);
            if(n==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)continue;
            if(n<=0)throw std::runtime_error("TLS send failed");sent+=n;
        }
    }
    void More(Deadline deadline) {
        if(encrypted.size()>=kMaxEncryptedBytes)throw std::runtime_error("TLS input limit exceeded");
        Wait(socket,false,deadline);
        std::array<uint8_t,16384> b{};
        int n=recv(socket,reinterpret_cast<char*>(b.data()),static_cast<int>(std::min(b.size(),kMaxEncryptedBytes-encrypted.size())),0);
        if(n==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)return;
        if(n<=0)throw std::runtime_error("TLS connection closed");
        encrypted.insert(encrypted.end(),b.begin(),b.begin()+n);
    }
    void KeepExtra(size_t count) {
        if(count>encrypted.size())throw std::runtime_error("Invalid TLS extra buffer");
        encrypted.erase(encrypted.begin(),encrypted.end()-count);
    }
};
TlsTransport::TlsTransport()=default;
TlsTransport::~TlsTransport()=default;
void TlsTransport::Close(){m_impl.reset();}
void TlsTransport::Connect(uintptr_t socket,const std::string& host,const std::filesystem::path& certificate) {
    Close();
    std::ifstream file(certificate,std::ios::binary);
    std::vector<uint8_t> pin(kMaxEncryptedBytes+1);file.read(reinterpret_cast<char*>(pin.data()),pin.size());
    auto count=file.gcount();
    if(count<=0||count>static_cast<std::streamsize>(kMaxEncryptedBytes))throw std::runtime_error("Missing or oversized --card-cert DER file");
    pin.resize(static_cast<size_t>(count));
    m_impl=std::make_unique<Impl>();auto& p=*m_impl;p.socket=static_cast<SOCKET>(socket);
    TLS_PARAMETERS limits{};
    limits.grbitDisabledProtocols=SP_PROT_TLS1_0_CLIENT|SP_PROT_TLS1_1_CLIENT|SP_PROT_SSL3_CLIENT|SP_PROT_SSL2_CLIENT;
    SCH_CREDENTIALS credentials{};credentials.dwVersion=SCH_CREDENTIALS_VERSION;
    credentials.dwFlags=SCH_CRED_MANUAL_CRED_VALIDATION|SCH_CRED_NO_DEFAULT_CREDS;
    credentials.cTlsParameters=1;credentials.pTlsParameters=&limits;
    TimeStamp expiry{};
    Check(AcquireCredentialsHandleA(nullptr,const_cast<char*>(UNISP_NAME_A),SECPKG_CRED_OUTBOUND,nullptr,&credentials,nullptr,nullptr,&p.credential,&expiry),"credentials");
    constexpr ULONG flags=ISC_REQ_ALLOCATE_MEMORY|ISC_REQ_CONFIDENTIALITY|ISC_REQ_INTEGRITY|ISC_REQ_STREAM|
        ISC_REQ_REPLAY_DETECT|ISC_REQ_SEQUENCE_DETECT|ISC_REQ_MANUAL_CRED_VALIDATION|ISC_REQ_USE_SUPPLIED_CREDS;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    bool first=true;ULONG attributes=0;
    for(;;) {
        SecBuffer input[2]={{static_cast<unsigned long>(p.encrypted.size()),SECBUFFER_TOKEN,p.encrypted.data()},{0,SECBUFFER_EMPTY,nullptr}};
        SecBufferDesc incoming{SECBUFFER_VERSION,2,input};Tokens output;
        auto status=InitializeSecurityContextA(&p.credential,first?nullptr:&p.context,const_cast<char*>(host.c_str()),flags,0,0,
            first?nullptr:&incoming,0,&p.context,&output.desc,&attributes,&expiry);
        first=false;
        if(output.data[0].cbBuffer)p.Send({static_cast<const uint8_t*>(output.data[0].pvBuffer),output.data[0].cbBuffer},deadline);
        if(status==SEC_E_INCOMPLETE_MESSAGE){p.More(deadline);continue;}
        p.KeepExtra(input[1].BufferType==SECBUFFER_EXTRA?input[1].cbBuffer:0);
        if(status==SEC_E_OK)break;
        if(status!=SEC_I_CONTINUE_NEEDED)Check(status,"handshake");
        if(p.encrypted.empty())p.More(deadline);
    }
    // Schannel stream TLS does not advertise ISC_RET_INTEGRITY separately on all OS versions.
    // TLS record authentication is provided by Schannel; require encryption and a modern protocol.
    if(!(attributes&ISC_RET_CONFIDENTIALITY))throw std::runtime_error("TLS encryption not negotiated");
    SecPkgContext_ConnectionInfo info{};Check(QueryContextAttributes(&p.context,SECPKG_ATTR_CONNECTION_INFO,&info),"protocol");
    if(info.dwProtocol!=SP_PROT_TLS1_2_CLIENT&&info.dwProtocol!=SP_PROT_TLS1_3_CLIENT)
        throw std::runtime_error("TLS 1.2 or newer required");
    if(info.dwCipherStrength<128)throw std::runtime_error("TLS cipher strength is insufficient");
    PCCERT_CONTEXT remote=nullptr;Check(QueryContextAttributes(&p.context,SECPKG_ATTR_REMOTE_CERT_CONTEXT,&remote),"certificate");
    // Exact certificate pin is the server identity; no trust-on-first-use or system-store changes.
    bool matches=remote && remote->cbCertEncoded==pin.size() &&
        std::equal(pin.begin(),pin.end(),remote->pbCertEncoded) && CertVerifyTimeValidity(nullptr,remote->pCertInfo)==0;
    if(remote)CertFreeCertificateContext(remote);
    if(!matches)throw std::runtime_error("TLS server certificate pin mismatch or certificate expired");
    Check(QueryContextAttributes(&p.context,SECPKG_ATTR_STREAM_SIZES,&p.sizes),"stream sizes");
    printf("CARD TLS verified certificate pin; protocol=%s\n",info.dwProtocol==SP_PROT_TLS1_3_CLIENT?"TLS1.3":"TLS1.2");
}
void TlsTransport::Write(std::span<const uint8_t> bytes,Deadline deadline) {
    auto& p=*m_impl;
    if(bytes.size()>p.sizes.cbMaximumMessage)throw std::runtime_error("TLS output too large");
    std::vector<uint8_t> record(p.sizes.cbHeader+bytes.size()+p.sizes.cbTrailer);
    std::copy(bytes.begin(),bytes.end(),record.begin()+p.sizes.cbHeader);
    SecBuffer b[4]={{p.sizes.cbHeader,SECBUFFER_STREAM_HEADER,record.data()},
        {static_cast<unsigned long>(bytes.size()),SECBUFFER_DATA,record.data()+p.sizes.cbHeader},
        {p.sizes.cbTrailer,SECBUFFER_STREAM_TRAILER,record.data()+p.sizes.cbHeader+bytes.size()},{0,SECBUFFER_EMPTY,nullptr}};
    SecBufferDesc desc{SECBUFFER_VERSION,4,b};Check(EncryptMessage(&p.context,0,&desc,0),"encrypt");
    for(int i=0;i<3;++i)p.Send({static_cast<const uint8_t*>(b[i].pvBuffer),b[i].cbBuffer},deadline);
}
void TlsTransport::Read(std::span<uint8_t> bytes,Deadline deadline) {
    auto& p=*m_impl;size_t copied=0;
    while(copied<bytes.size()) {
        if(!p.plain.empty()) {
            auto n=std::min(bytes.size()-copied,p.plain.size());std::copy_n(p.plain.begin(),n,bytes.begin()+copied);
            p.plain.erase(p.plain.begin(),p.plain.begin()+n);copied+=n;continue;
        }
        if(p.encrypted.empty())p.More(deadline);
        SecBuffer b[4]={{static_cast<unsigned long>(p.encrypted.size()),SECBUFFER_DATA,p.encrypted.data()},
            {0,SECBUFFER_EMPTY,nullptr},{0,SECBUFFER_EMPTY,nullptr},{0,SECBUFFER_EMPTY,nullptr}};
        SecBufferDesc desc{SECBUFFER_VERSION,4,b};auto status=DecryptMessage(&p.context,&desc,0,nullptr);
        if(status==SEC_E_INCOMPLETE_MESSAGE){p.More(deadline);continue;}
        if(status==SEC_I_CONTEXT_EXPIRED)throw std::runtime_error("TLS peer closed");
        // Fail closed on renegotiation; this protocol uses short connections and no client certificates.
        Check(status,"decrypt");size_t extra=0;
        for(auto& buffer:b) {
            if(buffer.BufferType==SECBUFFER_DATA&&buffer.cbBuffer) {
                auto* data=static_cast<uint8_t*>(buffer.pvBuffer);p.plain.insert(p.plain.end(),data,data+buffer.cbBuffer);
            } else if(buffer.BufferType==SECBUFFER_EXTRA)extra=buffer.cbBuffer;
        }
        p.KeepExtra(extra);
    }
}
}
