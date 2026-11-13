#pragma once
#include "engine/security/SecurityPolicy.h"
#include <array>
#include <initializer_list>
#include <string>
#include <vector>
class Input;
class Renderer;
class IntegrityWatchdog;
namespace security {
class Controls {
public:
    void Init(Policy, Renderer&, IntegrityWatchdog&, std::initializer_list<int> tokens, bool hasEnemyDraws);
    void Update(const Input&);
    const Policy& State() const { return m_state; }
    const wchar_t* Label() const { return m_label.c_str(); }
private:
    void Apply(bool previousWatchdog);
    Policy m_state;
    Renderer* m_renderer=nullptr;
    IntegrityWatchdog* m_watchdog=nullptr;
    std::vector<int> m_tokens;
    std::array<bool,4> m_previous{};
    bool m_enemy=false;
    std::wstring m_label;
};
}
