#include "game/modes/CardMode.h"

#include <algorithm>
#include <cstdio>
#include <random>

#include "engine/core/Logger.h"
#include "engine/core/Input.h"          // IsKeyDown / VK_SPACE を使うため完全定義が必要
#include "engine/core/Window.h"         // HUD（ウィンドウタイトル）更新用
#include "engine/renderer/Renderer.h"
#include "game/CardArt.h"               // トランプ絵柄テクスチャの手続き生成（PROMPT-G-01）
// #include "game/CheatModule.h"        // [旧チート・無効化] 協調型 DLL 手渡しは撤去
#include "game/OverlayText.h"           // クリア画面のオーバーレイ
#include "engine/audio/AudioPlayer.h"   // BGM＋効果音

using namespace DirectX;

// ---------------------------------------------------------------------------
// 定数・ヘルパー
// ---------------------------------------------------------------------------
namespace {

constexpr float kCardHalfX  = 0.35f;  // カードの半幅(X)
constexpr float kCardHalfZ  = 0.5f;   // カードの半奥行(Z)
constexpr float kPlayerZ    = -2.4f;  // あなた(席0)の手札 Z（手前）
constexpr float kOpponentZ  =  3.4f;  // 正面 CPU(席2)の手札 Z（奥）
constexpr float kSideX      =  3.6f;  // 左右 CPU(席1=右 / 席3=左)の X
constexpr float kMidZ       =  0.5f;  // 左右 CPU 列の中心 Z
constexpr float kSelRaise   =  0.35f; // 選択中カードを持ち上げる高さ
constexpr float kCpuThink   =  0.7f;  // CPU が考える演出の秒数

const char* RankName(uint8_t r) {
    static const char* n[13] = { "2","3","4","5","6","7","8","9","10","J","Q","K","A" };
    return (r < 13) ? n[r] : "?";
}
const char* SuitName(uint8_t s) {
    static const char* n[4] = { "H","D","C","S" }; // Heart/Diamond/Club/Spade
    return (s < 4) ? n[s] : "?";
}

// 平らな頂点色クアッド（XZ 平面・法線は上向き）。選択ハイライト枠に使う。
Mesh MakeQuad(Renderer& r, float halfX, float halfZ, const XMFLOAT4& color) {
    const XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
    std::vector<Vertex> v = {
        { { -halfX, 0.0f, -halfZ }, up, color },
        { {  halfX, 0.0f, -halfZ }, up, color },
        { {  halfX, 0.0f,  halfZ }, up, color },
        { { -halfX, 0.0f,  halfZ }, up, color },
    };
    std::vector<uint32_t> idx = { 0, 2, 1, 0, 3, 2 };
    return r.CreateMesh(v, idx);
}

// テクスチャ付きの平らなクアッド（XZ 平面・法線は上向き・UV 付き）。
// UV は U:-X→+X（0→1）, V:+Z→-Z（0→1）で貼る。見下ろしカメラ（-Z 側から +Z を見る）
// のとき、テクスチャ上部(V=0)が画面奥＝上に来て数字が正立して読める。
Mesh MakeTexQuad(Renderer& r, float halfX, float halfZ) {
    const XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
    std::vector<TexVertex> v = {
        { { -halfX, 0.0f, -halfZ }, up, { 0.0f, 1.0f } },
        { {  halfX, 0.0f, -halfZ }, up, { 1.0f, 1.0f } },
        { {  halfX, 0.0f,  halfZ }, up, { 1.0f, 0.0f } },
        { { -halfX, 0.0f,  halfZ }, up, { 0.0f, 0.0f } },
    };
    std::vector<uint32_t> idx = { 0, 2, 1, 0, 3, 2 };
    return r.CreateTexturedMesh(v, idx);
}

int CardIndex(const Card& c) { return c.suit * 13 + c.rank; } // 非ジョーカー専用

// 手札 i 枚目の中央寄せオフセット（span 内に count 枚を詰める）。
float FitOffset(int count, int i, float span) {
    if (count <= 1) return 0.0f;
    const float spacing = std::min(0.7f, span / static_cast<float>(count - 1));
    return -(count - 1) * spacing * 0.5f + i * spacing;
}

// 4人ババ抜きの席配置。0=あなた(手前) / 1=右 / 2=正面(奥) / 3=左。
// 手前・奥は X 方向に、左右は Z 方向に手札を並べる（見下ろしカメラで4方向に配る）。
XMFLOAT3 SeatPos(int seat, int count, int i) {
    switch (seat) {
        case 1:  return { kSideX,  0.0f, kMidZ + FitOffset(count, i, 3.0f) }; // 右
        case 2:  return { FitOffset(count, i, 6.5f), 0.0f, kOpponentZ };      // 正面(奥)
        case 3:  return { -kSideX, 0.0f, kMidZ + FitOffset(count, i, 3.0f) }; // 左
        default: return { FitOffset(count, i, 6.5f), 0.0f, kPlayerZ };        // あなた(手前)
    }
}

// 乱数源（配り直し・CPU の引き先選択に使う）
std::mt19937& Rng() {
    static std::mt19937 r{ std::random_device{}() };
    return r;
}

} // namespace

bool CardMode::Init(const GameContext& ctx) {
    m_renderer = ctx.renderer;
    m_window   = ctx.window;
    // m_cheat = ctx.cheat; // [旧チート・無効化] ゲームはチートに情報を渡さない
    if (m_renderer == nullptr) {
        LOG_ERROR("CardMode::Init: Renderer が null です");
        return false;
    }

    // --- 共有クアッド（カード 1 枚・テーブル）と選択枠を生成 ---
    m_cardQuad      = MakeTexQuad(*m_renderer, kCardHalfX, kCardHalfZ);
    m_tableQuad     = MakeTexQuad(*m_renderer, 5.0f, 4.0f);
    m_highlightMesh = MakeQuad(*m_renderer, kCardHalfX + 0.07f, kCardHalfZ + 0.07f,
                               XMFLOAT4{ 1.0f, 0.95f, 0.2f, 1.0f }); // 黄の選択枠

    // クリア画面（勝ち/負け）。決着時に 2D オーバーレイで表示する。
    m_winOverlay  = overlaytext::Build(*m_renderer, m_renderer->GetAspect(),
                    { "YOU WIN !", "", "SPACE = RETRY", "ESC = QUIT" });
    m_loseOverlay = overlaytext::Build(*m_renderer, m_renderer->GetAspect(),
                    { "YOU LOSE", "", "SPACE = RETRY", "ESC = QUIT" });

    // --- 絵柄テクスチャを手続き生成（PROMPT-G-01）---
    // 表 52 種（数字＋スート）、裏面、ジョーカー、カジノ卓フェルト。
    {
        std::vector<uint8_t> px;
        for (int s = 0; s < 4; ++s) {
            for (int r = 0; r < 13; ++r) {
                cardart::RenderFace(r, s, px);
                const Card c{ static_cast<uint8_t>(r), static_cast<uint8_t>(s), false };
                m_faceTex[CardIndex(c)] =
                    m_renderer->CreateTexture(cardart::kFaceW, cardart::kFaceH, px.data());
            }
        }
        cardart::RenderBack(px);
        m_backTex  = m_renderer->CreateTexture(cardart::kFaceW, cardart::kFaceH, px.data());
        cardart::RenderJoker(px);
        m_jokerTex = m_renderer->CreateTexture(cardart::kFaceW, cardart::kFaceH, px.data());
        cardart::RenderFelt(256, 256, px);
        m_feltTex  = m_renderer->CreateTexture(256, 256, px.data());
    }

    // テーブルを見下ろす固定カメラ（マウス視点は使わない＝カーソルロックしない）。
    // 4人分の手札（手前/左/右/奥）が入るよう、やや高く・急角度の俯瞰にする。
    m_camera.SetPosition(XMFLOAT3{ 0.0f, 11.0f, -7.0f });
    m_camera.SetRotation(/*yaw*/ 0.0f, /*pitch*/ -1.02f); // 急角度で見下ろす

    // カード面の数字/絵柄がはっきり読めるよう、真上からの強めの照明にする。
    // （フェルトの陰影も自然に出る。card モード内でのみ設定＝他モードに影響しない）
    m_renderer->SetDirectionalLight(
        XMFLOAT3{ 0.0f, -1.0f, 0.2f }, XMFLOAT3{ 1.0f, 1.0f, 0.98f },
        /*intensity=*/0.7f, XMFLOAT3{ 0.7f, 0.7f, 0.72f });

    // Remote mode never deals locally or allocates an opponent secret block.
    if (!m_remoteOptions.host.empty()) {
        m_waitOverlay=overlaytext::Build(*m_renderer,m_renderer->GetAspect(),
            {"WAITING FOR SERVER", "", "NO LOCAL FALLBACK", "ESC = QUIT"});
        m_remote=std::make_unique<cardnet::Client>();
        m_remote->Start(m_remoteOptions);
        AudioPlayer::Get().PlayBgm();
        LOG_INFO("CARD REMOTE seat=%u: server authority; only own cards received",unsigned(m_remoteOptions.seat));
        return true;
    }

    // 伏せ札ブロックを ShuffleAllocator のスロットに確保（PROMPT-V3-07：難読化）。
    m_opponent = m_secretAlloc.Allocate<secrets::CardSecretBlock>();
    if (!m_opponent.IsValid()) {
        LOG_ERROR("CardMode::Init: 伏せ札ブロックの確保に失敗しました");
        return false;
    }
    // 整合性ウォッチドッグに伏せ札ブロックを登録（PROMPT-V3-07：改ざん検知）。
    m_secretToken = m_watchdog.Register(
        "card.opponentHand",
        [this]() -> IntegrityWatchdog::Region {
            return { m_opponent.Get(), sizeof(secrets::CardSecretBlock) };
        });

    Deal();              // 最初の1ゲームを配る（RefreshSecret で監視基準も設定）
    m_security.Init(ctx.security,*m_renderer,m_watchdog,{m_secretToken},false);
    AudioPlayer::Get().PlayBgm(); // BGM 開始

    LOG_INFO("ババ抜き開始（A/D: 相手の札を選ぶ / Space: 引く / P: 難読化 / F6: 監視 / ESC 終了）"
             "  ※外部 cheat_tester --card で伏せ札をメモリ走査（防御 OFF で読まれやすい）");
    return true;
}

void CardMode::Deal() {
    // 52 枚 + ジョーカー1枚 = 53 枚の山札を作ってシャッフル。
    std::vector<Card> deck;
    deck.reserve(53);
    for (int s = 0; s < 4; ++s)
        for (int r = 0; r < 13; ++r)
            deck.push_back(Card{ static_cast<uint8_t>(r), static_cast<uint8_t>(s), false });
    deck.push_back(Card{ 0, 0, /*isJoker=*/true });
    std::shuffle(deck.begin(), deck.end(), Rng());

    // 4人へ順に配る（14/13/13/13 枚）。
    for (auto& h : m_hands) h.clear();
    m_finished.fill(false);
    for (size_t i = 0; i < deck.size(); ++i)
        m_hands[i % kNumPlayers].push_back(deck[i]);

    // 配った直後に各自の手札からペアを捨てる（ババ抜きの開始処理）。
    m_discardPairs = 0;
    for (int p = 0; p < kNumPlayers; ++p) {
        RemovePairs(m_hands[p]);
        if (m_hands[p].empty()) m_finished[p] = true; // 配った時点で上がり（稀）
    }

    m_turn     = m_finished[0] ? NextActive(0) : 0; // あなたから開始
    m_victim   = (m_turn >= 0) ? NextActive(m_turn) : -1;
    m_selIndex = 0;
    m_cpuTimer = (m_turn != 0) ? kCpuThink : 0.0f;
    m_gameOver = false;
    m_loser    = -1;

    RefreshSecret();   // 「引く相手」の手札を署名ブロックへ反映＋監視基準を更新
    CheckGameOver();   // 万一、配った時点で決着が付く場合に備える

    LOG_INFO("配りました（4人ババ抜き）。あなた %zu / CPU %zu/%zu/%zu 枚（捨てペア %d 組）",
             m_hands[0].size(), m_hands[1].size(), m_hands[2].size(), m_hands[3].size(),
             m_discardPairs);
}

void CardMode::RemovePairs(std::vector<Card>& hand) {
    // 同ランク2枚を見つけて捨てるのを、見つからなくなるまで繰り返す。
    // ジョーカーはペアにならない（最後まで残る＝ババ）。
    bool removedAny = true;
    while (removedAny) {
        removedAny = false;
        for (size_t i = 0; i < hand.size() && !removedAny; ++i) {
            if (hand[i].isJoker) continue;
            for (size_t j = i + 1; j < hand.size(); ++j) {
                if (hand[j].isJoker) continue;
                if (hand[i].rank == hand[j].rank) {
                    // j を先に消す（i より後ろなので i のインデックスは保たれる）
                    hand.erase(hand.begin() + j);
                    hand.erase(hand.begin() + i);
                    ++m_discardPairs;
                    removedAny = true;
                    break;
                }
            }
        }
    }
}

int CardMode::NextActive(int from) const {
    // from の「次」から時計回りに、手札が残っている別プレイヤーを探す（自分自身は除く）。
    for (int k = 1; k < kNumPlayers; ++k) {
        const int p = (from + k) % kNumPlayers;
        if (!m_hands[p].empty()) return p;
    }
    return -1; // 他に手札を持つプレイヤーがいない
}

int CardMode::ActiveCount() const {
    int n = 0;
    for (const auto& h : m_hands) if (!h.empty()) ++n;
    return n;
}

// drawer が victim の idx 枚目を引く。引いた札が手札の同ランクと揃えばペアで捨てる
// （本式ババ抜き：引いてペアが成立した時だけ手札を減らせる）。
void CardMode::DoDraw(int drawer, int victim, int idx) {
    if (drawer < 0 || victim < 0) return;
    if (idx < 0 || idx >= static_cast<int>(m_hands[victim].size())) return;

    const Card c = m_hands[victim][static_cast<size_t>(idx)];
    m_hands[victim].erase(m_hands[victim].begin() + idx);
    m_hands[drawer].push_back(c);
    AudioPlayer::Get().PlayCard();

    const int before = m_discardPairs;
    RemovePairs(m_hands[drawer]);
    const bool paired = (m_discardPairs > before);

    if (drawer == 0) {
        if (c.isJoker)      LOG_WARN("あなたはジョーカー(ババ)を引いてしまいました…");
        else if (paired)    LOG_INFO("あなたは %s%s を引き、ペアが揃って捨てました（捨てペア計 %d 組）",
                                     RankName(c.rank), SuitName(c.suit), m_discardPairs);
        else                LOG_INFO("あなたは %s%s を引きました（ペアにならず手札に加わる）",
                                     RankName(c.rank), SuitName(c.suit));
    }

    // 手札を出し切ったプレイヤーは上がり（安全）。
    if (m_hands[victim].empty()) m_finished[victim] = true;
    if (m_hands[drawer].empty()) m_finished[drawer] = true;
}

void CardMode::AdvanceTurn() {
    // 手番を次の生存プレイヤーへ。引く相手（victim）も取り直す。
    m_turn   = NextActive(m_turn);
    m_victim = (m_turn >= 0) ? NextActive(m_turn) : -1;
    m_selIndex = 0;
    RefreshSecret();
}

void CardMode::CheckGameOver() {
    // 手札が残っているのが1人になったら決着。その1人がババ（ジョーカー）持ち＝負け。
    if (m_gameOver) return;
    if (ActiveCount() <= 1) {
        m_gameOver = true;
        m_loser = -1;
        for (int p = 0; p < kNumPlayers; ++p)
            if (!m_hands[p].empty()) { m_loser = p; break; }
        if (m_loser == 0)
            LOG_WARN("★あなたの負け…ジョーカー(ババ)が手元に残りました（Space で新ゲーム）");
        else
            LOG_INFO("★あなたの勝ち！（安全に上がり）ババを持ったのは CPU%d（Space で新ゲーム）",
                     m_loser);
    }
}

void CardMode::RefreshSecret() {
    // 「現在の手番プレイヤーが引く相手(m_victim)」の手札を署名ブロックへ反映する。
    // あなたの番なら＝あなたが次に引く相手の手札＝ジョーカーの在り処が透ける（cheat_tester 対象）。
    secrets::CardSecretBlock* sec = m_opponent.Get();
    if (sec == nullptr) return;
    sec->magic = secrets::kCardMagic;

    const std::vector<Card>* vh =
        (m_victim >= 0 && m_victim < kNumPlayers) ? &m_hands[m_victim] : nullptr;
    const uint32_t n = vh
        ? static_cast<uint32_t>(vh->size() < secrets::kMaxHandCards ? vh->size()
                                                                    : secrets::kMaxHandCards)
        : 0u;
    sec->count = n;
    for (uint32_t i = 0; i < secrets::kMaxHandCards; ++i) {
        if (vh != nullptr && i < n) {
            sec->cards[i].rank = (*vh)[i].rank;
            sec->cards[i].suit = (*vh)[i].isJoker ? static_cast<uint8_t>(0xFF) : (*vh)[i].suit;
        } else {
            sec->cards[i].rank = 0;
            sec->cards[i].suit = 0;
        }
    }
    // 正規の更新なので監視基準を取り直す（Update が AcquireLock 保持中に呼ぶ前提）。
    if(m_security.State().watchdog) m_watchdog.Commit(m_secretToken);
}

void CardMode::Update(float dt, const Input& input) {
    if(m_remote) { UpdateRemote(input); return; }
    // P=難読化、F6=監視、F7=深度門番、M=タグマスク。
    // Start()/Stop() は監視スレッドを join するため AcquireLock() の外で呼ぶ（デッドロック回避）。
    m_security.Update(input);

    // 伏せ札ブロックを移動/更新する間は監視スレッドを排他する。
    auto lock = m_watchdog.AcquireLock();

    // 難読化（PROMPT-V3-07 / H-03）: 防御 ON のときだけ毎フレーム別スロットへ移動。
    if (m_security.State().shuffle) m_secretAlloc.ShuffleSlots();

    // 透視チートは base には無い（外部 game_cheat.dll が担う）。O トグルは撤去。

    if (m_gameOver) {
        const bool dealNow = input.IsKeyDown(VK_SPACE) || input.IsPadDown(GamepadButton::Cross);
        if (dealNow && !m_prevDraw) Deal();
        m_prevDraw = dealNow;
        UpdateHud();
        return;
    }

    // 手番が無効（決着間際の安全策）なら決着判定して抜ける。
    if (m_turn < 0 || m_victim < 0) { CheckGameOver(); UpdateHud(); return; }

    if (m_turn == 0) {
        // === あなたの番: 引く相手(m_victim)の伏せ札を1枚選んで引く ===
        const int n = static_cast<int>(m_hands[m_victim].size());
        if (n <= 0) { AdvanceTurn(); CheckGameOver(); UpdateHud(); return; }
        if (m_selIndex >= n) m_selIndex = n - 1;
        if (m_selIndex < 0)  m_selIndex = 0;

        // A/D または D-pad 左右で相手の札を選ぶ（左右ループ）
        const bool l = input.IsKeyDown('A') || input.IsPadDown(GamepadButton::DpadLeft);
        const bool r = input.IsKeyDown('D') || input.IsPadDown(GamepadButton::DpadRight);
        if (l && !m_prevLeft)  m_selIndex = (m_selIndex + n - 1) % n;
        if (r && !m_prevRight) m_selIndex = (m_selIndex + 1) % n;
        m_prevLeft  = l;
        m_prevRight = r;

        // Space または ✕ ボタンで選んだ札を引く
        const bool drawNow = input.IsKeyDown(VK_SPACE) || input.IsPadDown(GamepadButton::Cross);
        if (drawNow && !m_prevDraw) {
            DoDraw(0, m_victim, m_selIndex);
            CheckGameOver();
            if (!m_gameOver) {
                AdvanceTurn();
                if (m_turn != 0) m_cpuTimer = kCpuThink; // 次が CPU なら考える演出
            }
        }
        m_prevDraw = drawNow;
    } else {
        // === CPU の番: 一定時間後に引く相手からランダムに1枚引く ===
        // 押しっぱなしのキーが次のあなたの番で誤作動しないよう、エッジ状態を更新。
        m_prevDraw  = input.IsKeyDown(VK_SPACE) || input.IsPadDown(GamepadButton::Cross);
        m_prevLeft  = input.IsKeyDown('A') || input.IsPadDown(GamepadButton::DpadLeft);
        m_prevRight = input.IsKeyDown('D') || input.IsPadDown(GamepadButton::DpadRight);

        m_cpuTimer -= dt;
        if (m_cpuTimer <= 0.0f) {
            const int vn = static_cast<int>(m_hands[m_victim].size());
            if (vn > 0) {
                std::uniform_int_distribution<int> dist(0, vn - 1);
                DoDraw(m_turn, m_victim, dist(Rng()));
            }
            CheckGameOver();
            if (!m_gameOver) {
                AdvanceTurn();
                if (m_turn != 0) m_cpuTimer = kCpuThink; // 続けて CPU なら再び演出
            }
        }
    }

    UpdateHud();
}

void CardMode::UpdateHud() {
    if (m_window == nullptr) return;
    if(m_remote) {
        wchar_t title[260];
        swprintf_s(title,L"オンライン・ババ抜き 席%u  自分 %d / 相手 %d/%d/%d  [サーバー権威・自分の札のみ受信] %ls",
            unsigned(m_remoteOptions.seat),m_remoteCounts[0],m_remoteCounts[1],m_remoteCounts[2],m_remoteCounts[3],
            !m_remoteView.connected?L"接続断・操作停止":!m_remoteReady?L"他プレイヤー待ち":
            m_gameOver?L"決着・Spaceで再開":m_turn==0?L"あなたの番・A/D選択、Spaceで引く":L"他プレイヤーの番");
        SetWindowTextW(m_window->GetHandle(),title);return;
    }
    wchar_t phase[64];
    if (m_gameOver)
        swprintf_s(phase, (m_loser == 0) ? L"あなたの負け…(ババ)"
                                         : L"あなたの勝ち! (ババは CPU%d)", m_loser);
    else if (m_turn == 0)
        swprintf_s(phase, L"あなたの番 (A/D 選択・Space で CPU%d から引く)", m_victim);
    else
        swprintf_s(phase, L"CPU%d の番…", m_turn);

    wchar_t title[220];
    swprintf_s(title, L"4人ババ抜き  あなた %zu / CPU %zu/%zu/%zu 枚  捨てペア %d組  %ls  %ls",
               m_hands[0].size(), m_hands[1].size(), m_hands[2].size(), m_hands[3].size(),
               m_discardPairs, phase,
               m_security.Label());
    SetWindowTextW(m_window->GetHandle(), title);
}

void CardMode::Render(Renderer& renderer) {
    // [旧チート・無効化] 協調型の伏せ札透視（ゲームが相手手札を DLL に手渡し、revealCards
    //   指示で表向きに描く方式）は撤去。ゲームはチートに情報を渡さない。相手手札の実値は
    //   秘匿ブロック(m_opponent)としてメモリに存在し続けるので、外部 cheat_tester が自力で
    //   走査して読み取る（＝ゲーム無協力の情報透視。描画は伏せたまま）。
    renderer.BeginFrame(m_camera);

    // テーブル（フェルトテクスチャ）
    renderer.DrawTexturedMesh(m_tableQuad, m_feltTex,
                              XMMatrixTranslation(0.0f, -0.02f, 0.5f));

    // 4人分の手札を席位置に描く。あなた(席0)は表向き、CPU(1..3)は裏向き。
    // あなたの番は「引く相手(m_victim)」の選択中カードをハイライト＋持ち上げる。
    for (int seat = 0; seat < kNumPlayers; ++seat) {
        const std::vector<Card>& hand = m_hands[seat];
        const int cnt = m_remote ? m_remoteCounts[seat] : static_cast<int>(hand.size());
        for (int i = 0; i < cnt; ++i) {
            // Remote opponents are counts only; never construct fake/secret Card arrays.
            const Card* c = seat==0 ? &hand[i] : nullptr;
            XMFLOAT3 pos = SeatPos(seat, cnt, i);

            const bool selected =
                (!m_gameOver && m_turn == 0 && seat == m_victim && i == m_selIndex);
            if (selected) {
                renderer.DrawMesh(m_highlightMesh,
                                  XMMatrixTranslation(pos.x, 0.0f, pos.z));
                pos.y = kSelRaise;
            }

            // 席0（あなた）だけ表向き。CPU は伏せ札（＝ジョーカーは cheat_tester でのみ透ける）。
            const Texture* tex = &m_backTex;
            if (c) tex = c->isJoker ? &m_jokerTex : &m_faceTex[CardIndex(*c)];
            renderer.DrawTexturedMesh(m_cardQuad, *tex,
                                      XMMatrixTranslation(pos.x, pos.y, pos.z));
        }
    }

    // 決着時はクリア画面（勝ち/負け＋再プレイ/終了）を最前面に描く。
    if (m_remote && !m_remoteReady) renderer.DrawOverlay(m_waitOverlay);
    else if (m_gameOver) {
        renderer.DrawOverlay((m_loser == 0) ? m_loseOverlay : m_winOverlay);
    }

    renderer.EndFrame();
}

void CardMode::Shutdown() {
    if(m_remote) m_remote->Stop();
    // 監視スレッドを先に止める（Provider が参照する m_secretAlloc / m_opponent が
    // 破棄される前に）。デストラクタでも Stop されるが明示する。
    m_watchdog.Stop();
    // メッシュは ComPtr / std::array / std::vector で自動解放。明示処理は不要。
}

void CardMode::UpdateRemote(const Input& input) {
    m_remoteView=m_remote->Read();
    const auto& state=m_remoteView.state;
    m_remoteReady=m_remoteView.connected && state.phase!=0;
    if(state.revision!=m_remoteRevision) {
        m_remoteRevision=state.revision;
        auto relative=[&](uint8_t seat){return seat==cardnet::None?-1:(int(seat)+4-int(state.seat))%4;};
        // m_hands[1..3] stay empty throughout a remote session.
        m_hands[0].clear();
        for(size_t i=0;i<state.ownCount;++i) {
            uint8_t id=state.own[i];
            m_hands[0].push_back(Card{static_cast<uint8_t>(id%13),static_cast<uint8_t>(id/13),id==52});
        }
        for(int i=0;i<4;++i) m_remoteCounts[i]=state.counts[(state.seat+i)%4];
        m_turn=relative(state.turn);m_victim=relative(state.victim);m_loser=relative(state.loser);
        m_gameOver=state.phase==2;m_discardPairs=state.discarded;
        LOG_INFO("CARD VIEW revision=%llu round=%u phase=%u own=%u counts=%u/%u/%u/%u",
            static_cast<unsigned long long>(state.revision),state.round,unsigned(state.phase),unsigned(state.ownCount),
            unsigned(state.counts[0]),unsigned(state.counts[1]),unsigned(state.counts[2]),unsigned(state.counts[3]));
    }
    const bool left=input.IsKeyDown('A')||input.IsPadDown(GamepadButton::DpadLeft);
    const bool right=input.IsKeyDown('D')||input.IsPadDown(GamepadButton::DpadRight);
    const bool draw=input.IsKeyDown(VK_SPACE)||input.IsPadDown(GamepadButton::Cross);
    if(m_remoteReady && !m_remoteView.pending) {
        if(m_gameOver) {
            if(m_remoteOptions.autoplay) { LOG_INFO("CARD REMOTE COMPLETE round=%u",state.round);PostQuitMessage(0); }
            else if(draw&&!m_prevDraw) m_remote->Queue(cardnet::Operation::Restart,0,state.revision);
        } else if(m_turn==0 && m_victim>=0) {
            int count=m_remoteCounts[m_victim];
            if(count>0) {
                m_selIndex=std::clamp(m_selIndex,0,count-1);
                if(left&&!m_prevLeft) m_selIndex=(m_selIndex+count-1)%count;
                if(right&&!m_prevRight) m_selIndex=(m_selIndex+1)%count;
                if(m_remoteOptions.autoplay) m_selIndex=static_cast<int>((m_autoChoice++ * 7u)%count);
                if(m_remoteOptions.autoplay || (draw&&!m_prevDraw))
                    m_remote->Queue(cardnet::Operation::Draw,static_cast<uint8_t>(m_selIndex),state.revision);
            }
        }
    }
    m_prevLeft=left;m_prevRight=right;m_prevDraw=draw;
    UpdateHud();
}
