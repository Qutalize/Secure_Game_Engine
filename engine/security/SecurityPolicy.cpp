#include "engine/security/SecurityPolicy.h"
#include <stdexcept>
#include <set>
namespace security {
Policy Defaults(std::string_view mode, bool remote, bool hardened) {
    Policy p;
    if (remote) { p.authority=p.privateHands=true; return p; }
    if (mode=="card") p.shuffle=p.watchdog=true;
    if (hardened && (mode=="fps" || mode=="hide"))
        p.shuffle=p.watchdog=p.depthGuard=p.tagMask=true;
    return p;
}
Policy Parse(std::string_view list) {
    Policy p;
    if (list=="none") return p;
    if (list.empty()) throw std::invalid_argument("Empty --security list");
    std::set<std::string_view> seen;
    while (true) {
        auto comma=list.find(',');
        auto name=list.substr(0,comma);
        if (!seen.insert(name).second) throw std::invalid_argument("Duplicate security feature");
        if (name=="shuffle") p.shuffle=true;
        else if (name=="watchdog") p.watchdog=true;
        else if (name=="depth-guard") p.depthGuard=true;
        else if (name=="tag-mask") p.tagMask=true;
        else if (name=="authority") p.authority=true;
        else if (name=="private-hands") p.privateHands=true;
        else throw std::invalid_argument("Unknown security feature: "+std::string(name));
        if (comma==std::string_view::npos) break;
        list.remove_prefix(comma+1);
    }
    return p;
}
void Validate(const Policy& p, std::string_view mode, bool remote) {
    if (mode!="fps" && mode!="hide" && mode!="card" && mode!="range")
        throw std::invalid_argument("Unknown game mode");
    if (remote && mode!="card") throw std::invalid_argument("--card-server requires --mode card");
    if (p.privateHands && !p.authority) throw std::invalid_argument("private-hands requires authority");
    if ((p.authority || p.privateHands) && !remote)
        throw std::invalid_argument("authority/private-hands require --card-server and seat credentials");
    if (remote && (!p.authority || !p.privateHands))
        throw std::invalid_argument("Remote card requires authority,private-hands; no unfiltered mode");
    if ((mode=="range" || remote) && (p.shuffle || p.watchdog || p.depthGuard || p.tagMask))
        throw std::invalid_argument("This mode has no local secret-block/draw defense; choose supported features");
    if (mode=="card" && (p.depthGuard || p.tagMask))
        throw std::invalid_argument("Card has no tagged enemy draws: depth-guard/tag-mask are unsupported");
}
std::string Describe(const Policy& p) {
    return "shuffle="+std::to_string(p.shuffle)+" watchdog="+std::to_string(p.watchdog)+
        " depth-guard="+std::to_string(p.depthGuard)+" tag-mask="+std::to_string(p.tagMask)+
        " authority="+std::to_string(p.authority)+" private-hands="+std::to_string(p.privateHands);
}
}
