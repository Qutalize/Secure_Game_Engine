#pragma once
#include <string>
#include <string_view>

namespace security {
struct Policy {
    bool shuffle = false;
    bool watchdog = false;
    bool depthGuard = false;
    bool tagMask = false;
    bool authority = false;
    bool privateHands = false;
};
Policy Defaults(std::string_view mode, bool remoteCard, bool hardened);
// Explicit lists replace defaults; misspellings/duplicates/unsupported dependencies fail.
Policy Parse(std::string_view list);
void Validate(const Policy&, std::string_view mode, bool remoteCard);
std::string Describe(const Policy&);
}
