#include "game/SecurityControls.h"
#include "engine/core/Input.h"
#include "engine/core/Logger.h"
#include "engine/renderer/Renderer.h"
#include "engine/security/IntegrityWatchdog.h"
namespace security {
void Controls::Init(Policy p, Renderer& r, IntegrityWatchdog& w, std::initializer_list<int> tokens, bool enemy) {
    m_state=p; m_renderer=&r; m_watchdog=&w; m_tokens=tokens; m_enemy=enemy;
    Apply(false);
}
void Controls::Apply(bool previousWatchdog) {
    // Start/Stop must stay outside AcquireLock: Stop joins the reader thread.
    if (m_state.watchdog!=previousWatchdog) {
        if (m_state.watchdog) {
            { auto lock=m_watchdog->AcquireLock(); for(int token:m_tokens) m_watchdog->Commit(token); }
            m_watchdog->Start();
        } else m_watchdog->Stop();
    }
    m_renderer->SetEnemyDepthGuard(m_state.depthGuard);
    m_renderer->SetDrawTagMask(m_state.tagMask);
    m_label=L"[移動:"+std::to_wstring(m_state.shuffle)+L" 監視:"+std::to_wstring(m_state.watchdog)+
        L" 深度:"+std::to_wstring(m_state.depthGuard)+L" タグ:"+std::to_wstring(m_state.tagMask)+L"]";
    LOG_INFO("SECURITY %s [P/L1 shuffle, F6 watchdog, F7 depth-guard, M/R1 tag-mask]",Describe(m_state).c_str());
}
void Controls::Update(const Input& in) {
    const bool down[]={in.IsKeyDown('P')||in.IsPadDown(GamepadButton::L1),in.IsKeyDown(VK_F6),
        in.IsKeyDown(VK_F7),in.IsKeyDown('M')||in.IsPadDown(GamepadButton::R1)};
    bool* flags[]={&m_state.shuffle,&m_state.watchdog,&m_state.depthGuard,&m_state.tagMask};
    const bool previousWatchdog=m_state.watchdog;
    bool changed=false;
    for (int i=0;i<4;++i) {
        if (down[i]&&!m_previous[i] && (i<2 || m_enemy)) { *flags[i]=!*flags[i]; changed=true; }
        m_previous[i]=down[i];
    }
    if (changed) Apply(previousWatchdog);
}
}
