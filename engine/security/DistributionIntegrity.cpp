#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "engine/security/DistributionIntegrity.h"
#include "DistributionPublicKey.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace distribution {
namespace {
constexpr uint64_t MaxFile=512ull*1024*1024;
constexpr char Header[]="GE-DISTRIBUTION 1\n";
void Check(NTSTATUS value,const char* operation) {
    if(value<0)throw std::runtime_error(std::string("Distribution crypto failure: ")+operation);
}
struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;
    File()=default;
    File(File&& other) noexcept:handle(other.handle){other.handle=INVALID_HANDLE_VALUE;}
    File& operator=(File&&)=delete;
    File(const File&)=delete;
    ~File(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
struct Algorithm {
    BCRYPT_ALG_HANDLE handle=nullptr;
    explicit Algorithm(LPCWSTR name){Check(BCryptOpenAlgorithmProvider(&handle,name,nullptr,0),"algorithm");}
    ~Algorithm(){if(handle)BCryptCloseAlgorithmProvider(handle,0);}
};
struct Hash {
    BCRYPT_HASH_HANDLE handle=nullptr;
    explicit Hash(Algorithm& algorithm){Check(BCryptCreateHash(algorithm.handle,&handle,nullptr,0,nullptr,0,0),"hash create");}
    ~Hash(){if(handle)BCryptDestroyHash(handle);}
    void Add(std::span<const uint8_t> bytes){Check(BCryptHashData(handle,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0),"hash data");}
    std::array<uint8_t,32> Finish(){std::array<uint8_t,32> result{};Check(BCryptFinishHash(handle,result.data(),32,0),"hash finish");return result;}
};
struct Key {
    BCRYPT_KEY_HANDLE handle=nullptr;
    ~Key(){if(handle)BCryptDestroyKey(handle);}
};
std::string Lower(std::string text){for(char& c:text)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return text;}
void ValidateName(const std::string& name) {
    if(name.empty()||name.size()>240)throw std::runtime_error("Invalid distribution path length");
    for(char c:name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||c=='/'))
        throw std::runtime_error("Invalid distribution path character");
    size_t begin=0;
    while(begin<name.size()) {
        auto end=name.find('/',begin);if(end==std::string::npos)end=name.size();
        auto part=Lower(name.substr(begin,end-begin));auto stem=part.substr(0,part.find('.'));
        if(part.empty()||part=="."||part==".."||part.back()=='.'||stem=="con"||stem=="prn"||stem=="aux"||stem=="nul"||
            (stem.size()==4&&(stem.starts_with("com")||stem.starts_with("lpt"))&&stem[3]>='1'&&stem[3]<='9'))
            throw std::runtime_error("Unsafe distribution path component");
        begin=end+1;
    }
    if(name.back()=='/')throw std::runtime_error("Distribution file path ends with slash");
}
std::vector<uint8_t> Hex(const std::string& s) {
    if(s.size()%2)throw std::runtime_error("Invalid hex length");std::vector<uint8_t> result;
    auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("Invalid lowercase hex");};
    for(size_t i=0;i<s.size();i+=2)result.push_back(static_cast<uint8_t>(digit(s[i])*16+digit(s[i+1])));
    return result;
}
void NoReparse(const std::filesystem::path& path) {
    DWORD flags=GetFileAttributesW(path.c_str());
    if(flags==INVALID_FILE_ATTRIBUTES||(flags&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Missing path or reparse point in distribution");
}
File Open(const std::filesystem::path& path,uint64_t max,uint64_t& size) {
    NoReparse(path);File file;
    file.handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file.handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Distribution file missing, locked for writing, or unreadable: "+path.filename().string());
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
    if(!GetFileInformationByHandle(file.handle,&info)||info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)||
       !GetFileSizeEx(file.handle,&length)||length.QuadPart<0||static_cast<uint64_t>(length.QuadPart)>max)
        throw std::runtime_error("Invalid distribution file or size");
    size=static_cast<uint64_t>(length.QuadPart);return file;
}
DWORD Read(File& file,std::span<uint8_t> buffer) {
    DWORD got=0;if(!ReadFile(file.handle,buffer.data(),static_cast<DWORD>(buffer.size()),&got,nullptr))throw std::runtime_error("Distribution read failed");return got;
}
std::vector<uint8_t> Small(File& file,size_t size) {
    std::vector<uint8_t> result(size);size_t done=0;
    while(done<size){auto n=Read(file,std::span(result).subspan(done));if(!n)throw std::runtime_error("Truncated distribution metadata");done+=n;}
    return result;
}
}
struct Guard::Impl {
    std::filesystem::path root;
    std::vector<File> files;
    std::set<std::string> names;
    uint64_t bytes=0;
};
Guard::Guard(const std::filesystem::path& directory):m_impl(std::make_unique<Impl>()) {
    auto& p=*m_impl;p.root=std::filesystem::absolute(directory).lexically_normal();NoReparse(p.root);
    uint64_t size=0;auto manifestFile=Open(p.root/"distribution.manifest",512*1024,size);auto manifest=Small(manifestFile,static_cast<size_t>(size));
    auto signatureFile=Open(p.root/"distribution.sig",64,size);if(size!=64)throw std::runtime_error("Invalid distribution signature size");
    auto signature=Small(signatureFile,64);Algorithm sha(BCRYPT_SHA256_ALGORITHM);Hash manifestHash(sha);manifestHash.Add(manifest);auto digest=manifestHash.Finish();
    Algorithm ecdsa(BCRYPT_ECDSA_P256_ALGORITHM);auto xy=Hex(PublicKeyHex);
    if(xy.size()!=64)throw std::runtime_error("Invalid embedded distribution public key");
    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB)+xy.size());BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC,32};
    memcpy(blob.data(),&header,sizeof(header));std::copy(xy.begin(),xy.end(),blob.begin()+sizeof(header));Key key;
    Check(BCryptImportKeyPair(ecdsa.handle,nullptr,BCRYPT_ECCPUBLIC_BLOB,&key.handle,blob.data(),static_cast<ULONG>(blob.size()),0),"public key");
    if(BCryptVerifySignature(key.handle,nullptr,digest.data(),32,signature.data(),64,0)<0)throw std::runtime_error("Distribution signature rejected");
    // Authenticate the bounded metadata before trusting any of its filenames or sizes.
    std::string body(manifest.begin(),manifest.end());if(!body.starts_with(Header)||body.back()!='\n')throw std::runtime_error("Invalid distribution manifest header");
    struct Entry {std::string name;uint64_t size;std::vector<uint8_t> hash;};std::vector<Entry> entries;
    size_t pos=sizeof(Header)-1;
    while(pos<body.size()) {
        auto end=body.find('\n',pos);auto line=body.substr(pos,end-pos);pos=end+1;
        auto separator=line.find(' ',65);
        if(line.size()<68||line[64]!=' '||separator==std::string::npos)throw std::runtime_error("Invalid distribution manifest record");
        std::string number=line.substr(65,separator-65),name=line.substr(separator+1);ValidateName(name);
        uint64_t count=0;auto parsed=std::from_chars(number.data(),number.data()+number.size(),count);
        if(number.empty()||parsed.ec!=std::errc{}||parsed.ptr!=number.data()+number.size()||count>MaxFile||std::to_string(count)!=number)
            throw std::runtime_error("Invalid distribution record size");
        auto lower=Lower(name);
        if(lower=="distribution.manifest"||lower=="distribution.sig"||!p.names.insert(lower).second)throw std::runtime_error("Duplicate or reserved distribution path");
        if(entries.size()>=4096)throw std::runtime_error("Too many distribution records");
        entries.push_back({name,count,Hex(line.substr(0,64))});
    }
    if(entries.empty())throw std::runtime_error("Empty distribution manifest");
    std::set<std::string> actual;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(p.root)) {
        NoReparse(entry.path());
        if(entry.is_regular_file()) {
            auto name=entry.path().lexically_relative(p.root).generic_string();ValidateName(name);
            const auto lower=Lower(name);
            if((lower=="distribution.manifest"||lower=="distribution.sig")&&name!=lower)
                throw std::runtime_error("Distribution metadata must use its canonical filename");
            name=lower;
            if(name!="distribution.manifest"&&name!="distribution.sig") {
                if(!actual.insert(name).second)throw std::runtime_error("Case collision in distribution");
            }
        } else if(!entry.is_directory())throw std::runtime_error("Unexpected distribution file type");
    }
    if(actual!=p.names)throw std::runtime_error("Distribution file inventory mismatch (missing or extra file)");
    p.files.reserve(entries.size()+2);p.files.push_back(std::move(manifestFile));p.files.push_back(std::move(signatureFile));
    std::array<uint8_t,64*1024> buffer{};
    for(auto& entry:entries) {
        // Recheck all directory components before opening the leaf (junctions are unsupported).
        auto current=p.root;
        for(const auto& component:std::filesystem::path(entry.name).parent_path()){current/=component;NoReparse(current);}
        auto file=Open(p.root/entry.name,MaxFile,size);if(size!=entry.size)throw std::runtime_error("Distribution file size mismatch: "+entry.name);
        Hash hash(sha);uint64_t total=0;
        while(auto count=Read(file,buffer)){hash.Add(std::span(buffer).first(count));total+=count;}
        auto value=hash.Finish();
        if(total!=entry.size||!std::equal(value.begin(),value.end(),entry.hash.begin()))throw std::runtime_error("Distribution file hash mismatch: "+entry.name);
        p.bytes+=total;p.files.push_back(std::move(file));
    }
}
Guard::~Guard()=default;
bool Guard::Contains(const std::filesystem::path& path) const {
    auto absolute=std::filesystem::absolute(path).lexically_normal();auto relative=absolute.lexically_relative(m_impl->root).generic_string();
    return m_impl->names.contains(Lower(relative));
}
size_t Guard::FileCount()const{return m_impl->names.size();}
uint64_t Guard::Bytes()const{return m_impl->bytes;}
std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> buffer(32768);auto size=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(!size||size>=buffer.size())throw std::runtime_error("Cannot locate distribution executable");
    return std::filesystem::path(std::wstring(buffer.data(),size)).parent_path();
}
}
