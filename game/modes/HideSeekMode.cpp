#include "game/modes/HideSeekMode.h"

#include <cmath>
#include <cstdio>
#include <random>

#include "engine/core/Logger.h"
#include "engine/core/Window.h"
#include "engine/core/Input.h"
#include "engine/renderer/Renderer.h"
#include "engine/math/Collision.h"

// #include "game/CheatModule.h" // [旧チート・無効化] 協調型 DLL 手渡しは撤去
#include "cheat/HookCheat.h"     // ステージJ-02: 描画フック型チート（継ぎ目に差し込む）
#include "game/CharacterMesh.h"  // テーパー角柱（人型を滑らかに）
#include "game/OverlayText.h"   // クリア画面のオーバーレイ
#include "engine/audio/AudioPlayer.h" // BGM＋効果音

using namespace DirectX;

// ---------------------------------------------------------------------------
// 定数・ヘルパー
// ---------------------------------------------------------------------------
namespace {

const XMFLOAT3  kSeekerStart{ 0.0f, 0.0f, -16.0f }; // 園庭の南側から開始

// hider は「子供役」なので、同じ Swat モデルを一様スケールで小柄にして使う。
// Swat の身長は約 1.85m なので 0.62 倍で約 1.15m ＝ 未就学児くらいの背丈になる。
// 当たり判定もこれに合わせて縮める（見た目と当たり判定がずれると、茂みの陰に
// 隠れているのに body がはみ出す／逆に見えているのに撃ち抜けない、が起きる）。
constexpr float kHiderScale = 0.62f;
const XMFLOAT3  kHiderHalf{ 0.26f, 0.58f, 0.26f };  // 子供サイズの当たり判定半サイズ
constexpr float kFindRange = 3.5f;                  // この距離以内＋視線が通れば発見

constexpr float kArenaBound     = 18.0f; // hider が徘徊できる範囲（外周壁の内側）
constexpr float kHiderWander    = 1.2f;  // 徘徊速度 m/s
constexpr float kHiderFlee      = 2.8f;  // 逃走速度 m/s
constexpr float kFleeRange      = 9.0f;  // seeker がこの距離＋視線で近づくと逃げる
constexpr float kWanderReach    = 1.0f;  // 目標点への到達判定
constexpr float kWanderInterval = 3.5f;  // 目標を選び直す間隔（秒）

// hider の初期の隠れ場所（足元 y=0）。
// ★World::BuildHideArenaModules の植栽配置と対で決めている★ 大きな茂みや木のそば
//   （ただし当たり判定には重ならない位置）に置くことで、「陰から覗いている」状態で
//   ゲームが始まる。World 側の配置を動かすときはここも一緒に見直すこと。
const XMFLOAT3 kHiderSpots[] = {
    { -13.5f, 0.0f,  -4.0f },   // 西の大茂み(-16,-4) の陰
    {  13.5f, 0.0f,   2.0f },   // 東の大茂み(16,2) の陰
    {  -5.0f, 0.0f, -12.5f },   // 南の大茂み(-5,-15) の陰
    {   8.0f, 0.0f,  12.5f },   // 北の大茂み(8,15) の陰
    { -12.0f, 0.0f,  13.5f },   // 北西の大茂み(-12,16) の陰
    {  -8.5f, 0.0f,   6.0f },   // 中央西の木(-10,6) の陰
    {  11.5f, 0.0f,  -2.0f },   // 東の木(13,-2) の陰
};

// 法線付きボックスを v/idx に追記（中心 center・半 half・色 color）。合成メッシュ用。
void AddBox(std::vector<Vertex>& v, std::vector<uint32_t>& idx,
            const XMFLOAT3& center, const XMFLOAT3& half, const XMFLOAT4& color) {
    const float hx = half.x, hy = half.y, hz = half.z;
    const float cx = center.x, cy = center.y, cz = center.z;
    struct Face { XMFLOAT3 n; XMFLOAT3 c[4]; };
    const Face faces[6] = {
        { {  0,  0, -1 }, { { -hx, -hy, -hz }, {  hx, -hy, -hz }, {  hx,  hy, -hz }, { -hx,  hy, -hz } } },
        { {  0,  0,  1 }, { {  hx, -hy,  hz }, { -hx, -hy,  hz }, { -hx,  hy,  hz }, {  hx,  hy,  hz } } },
        { { -1,  0,  0 }, { { -hx, -hy,  hz }, { -hx, -hy, -hz }, { -hx,  hy, -hz }, { -hx,  hy,  hz } } },
        { {  1,  0,  0 }, { {  hx, -hy, -hz }, {  hx, -hy,  hz }, {  hx,  hy,  hz }, {  hx,  hy, -hz } } },
        { {  0,  1,  0 }, { { -hx,  hy, -hz }, {  hx,  hy, -hz }, {  hx,  hy,  hz }, { -hx,  hy,  hz } } },
        { {  0, -1,  0 }, { { -hx, -hy,  hz }, {  hx, -hy,  hz }, {  hx, -hy, -hz }, { -hx, -hy, -hz } } },
    };
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(v.size());
        for (int i = 0; i < 4; ++i) {
            v.push_back(Vertex{ { f.c[i].x + cx, f.c[i].y + cy, f.c[i].z + cz }, f.n, color });
        }
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }
}

// 歩行アニメ定数（hider は小さめ）。
constexpr float kHiderHipY       = 0.65f; // 股関節の高さ（脚ピボット。靴底が床に接地）
constexpr float kHiderHipX       = 0.12f; // 股関節の左右オフセット
constexpr float kHiderShoulderY  = 1.16f; // 肩の高さ（腕ピボット）
constexpr float kHiderShoulderX  = 0.30f; // 肩の左右オフセット
constexpr float kHiderLegSwing   = 0.45f; // 脚の振り幅（ラジアン）
constexpr float kHiderArmSwing   = 0.38f; // 腕の振り幅（脚と逆位相）
constexpr float kHiderWalkRate = 5.0f;  // 歩行位相の進み（1m あたり rad）
constexpr float kFoundPopTime  = 0.4f;  // 発見時に跳ねる時間（秒）

// 「隠れる人」風の胴＝テーパー角柱トルソ（腰→胸→肩）＋首＋頭＋帽子＋顔。腕/脚は別メッシュ。
Mesh MakeHiderBody(Renderer& r, const XMFLOAT4& color) {
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    const XMFLOAT4 dark { color.x * 0.50f, color.y * 0.50f, color.z * 0.50f, 1.0f }; // 帽子
    const XMFLOAT4 skin { 0.86f, 0.71f, 0.58f, 1.0f };                              // 首/顔
    // トルソ: 腰(細)→胸→肩(広) をテーパーで連続的に。
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 0.62f, 0.92f, 0.17f, 0.13f, 0.22f, 0.16f, color); // 腰→胸
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 0.92f, 1.16f, 0.22f, 0.16f, 0.26f, 0.15f, color); // 胸→肩
    // 首→頭→帽子（先細り）＋顔。
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 1.16f, 1.26f, 0.08f, 0.08f, 0.07f, 0.07f, skin); // 首
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 1.26f, 1.46f, 0.14f, 0.15f, 0.15f, 0.15f, skin); // 頭
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 1.44f, 1.60f, 0.17f, 0.17f, 0.10f, 0.10f, dark); // 帽子(先細り)
    AddBox(v, idx, XMFLOAT3{ 0.0f, 1.34f, 0.14f }, XMFLOAT3{ 0.09f, 0.07f, 0.03f }, skin);  // 顔前面
    return r.CreateMesh(v, idx);
}

// hider の腕1本（袖＝服色 / 手＝肌）。肩ピボットから下へ、上腕→前腕がテーパー＋手。
Mesh MakeHiderArm(Renderer& r, const XMFLOAT4& color) {
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    const XMFLOAT4 sleeve{ color.x * 0.85f, color.y * 0.85f, color.z * 0.85f, 1.0f };
    const XMFLOAT4 skin  { 0.86f, 0.71f, 0.58f, 1.0f };
    charmesh::AddTaper(v, idx, 0.0f, 0.0f,  -0.30f, 0.00f, 0.058f, 0.070f, 0.075f, 0.088f, sleeve); // 上腕
    charmesh::AddTaper(v, idx, 0.0f, 0.02f, -0.52f, -0.30f, 0.048f, 0.058f, 0.058f, 0.070f, sleeve); // 前腕
    AddBox(v, idx, XMFLOAT3{ 0.0f, -0.57f, 0.03f }, XMFLOAT3{ 0.052f, 0.06f, 0.07f }, skin);          // 手
    return r.CreateMesh(v, idx);
}

// hider の脚1本（ズボン＝テーパー＋靴）。股関節ピボットから下へ、腿→脛→靴。
Mesh MakeHiderLeg(Renderer& r) {
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    const XMFLOAT4 pants{ 0.22f, 0.20f, 0.14f, 1.0f };
    const XMFLOAT4 shoe { 0.10f, 0.08f, 0.06f, 1.0f };
    charmesh::AddTaper(v, idx, 0.0f, 0.0f,  -0.34f, 0.00f, 0.075f, 0.095f, 0.095f, 0.11f, pants); // 腿
    charmesh::AddTaper(v, idx, 0.0f, 0.01f, -0.56f, -0.34f, 0.06f, 0.08f, 0.075f, 0.095f, pants); // 脛
    AddBox(v, idx, XMFLOAT3{ 0.0f, -0.59f, 0.04f }, XMFLOAT3{ 0.085f, 0.06f, 0.13f }, shoe);       // 靴
    return r.CreateMesh(v, idx);
}

// a→b の視線が障害物に遮られていなければ true。
bool LineClear(const XMFLOAT3& a, const XMFLOAT3& b,
               const std::vector<AABB>& obstacles) {
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 1e-3f) return true;
    const XMFLOAT3 dir{ dx / dist, dy / dist, dz / dist };
    for (const AABB& o : obstacles) {
        float t = 0.0f;
        if (RayIntersectAABB(a, dir, o, t) && t > 0.05f && t < dist - 0.1f) return false;
    }
    return true;
}

float YawFromDir(float dx, float dz) { return atan2f(dx, dz); }

// 乱数源（徘徊の目標選び）。
std::mt19937& Rng() {
    static std::mt19937 r{ std::random_device{}() };
    return r;
}
float RandRange(float lo, float hi) {
    std::uniform_real_distribution<float> d(lo, hi);
    return d(Rng());
}

} // namespace

bool HideSeekMode::Init(const GameContext& ctx) {
    m_renderer = ctx.renderer;
    m_window   = ctx.window;
    // m_cheat = ctx.cheat; // [旧チート・無効化] ゲームはチートに情報を渡さない
    if (m_renderer == nullptr) {
        LOG_ERROR("HideSeekMode::Init: Renderer が null です");
        return false;
    }

    // パッケージ化（J-05）: 対策版(-DHARDENED=ON)は防御を既定 ON で起動する（通常版は OFF）。

    // 広いかくれんぼ場（床＋多様なカバー）を構築（PROMPT-G-03）。
    m_world.Build(*m_renderer, WorldLayout::HideArena);

    // ライティング（視覚向上）: やわらかい昼光＋明るめの環境光（草地が映える）。
    m_renderer->SetDirectionalLight(
        XMFLOAT3{ 0.35f, -1.0f, 0.45f }, XMFLOAT3{ 1.0f, 0.97f, 0.88f },
        /*intensity=*/1.05f, XMFLOAT3{ 0.40f, 0.45f, 0.44f });

    // hider の胴+頭（未発見=黄, 発見済=緑, ESP=赤）と脚（共通）。
    m_hiderBodyMesh      = MakeHiderBody(*m_renderer, XMFLOAT4{ 0.95f, 0.85f, 0.15f, 1.0f });
    m_hiderFoundBodyMesh = MakeHiderBody(*m_renderer, XMFLOAT4{ 0.15f, 0.85f, 0.35f, 1.0f });
    m_hiderArmMesh       = MakeHiderArm(*m_renderer, XMFLOAT4{ 0.95f, 0.85f, 0.15f, 1.0f });
    m_hiderLegMesh       = MakeHiderLeg(*m_renderer);
    // ESP（ウォールハック）用の赤い胴。深度無視で描くと壁越しに光って見える。
    m_hiderEspMesh   = MakeHiderBody(*m_renderer, XMFLOAT4{ 1.0f, 0.15f, 0.15f, 1.0f });

    // 画面中央の照準（2D クロスヘア）
    m_crosshair = m_renderer->CreateCrosshairMesh();

    // クリア画面（全員発見）。R/△ で再挑戦・ESC で終了。
    m_clearOverlay = overlaytext::Build(
        *m_renderer, m_renderer->GetAspect(),
        { "CLEAR !", "", "R = RETRY", "ESC = QUIT" });

    // hider を配置（徘徊の初期目標＝現在地。wanderTimer をばらけさせる）
    m_hiders.clear();
    for (const XMFLOAT3& s : kHiderSpots) {
        Hider h;
        h.position    = s;
        h.target      = s;
        h.wanderTimer = RandRange(0.5f, kWanderInterval);
        h.found       = false;
        // hider の Swat 化: m_world.Build() は既にこの直前で呼ばれているので、glTF の
        // 読み込み成否はここで確定している。失敗時は未 Bind のまま＝Render が箱へフォールバック。
        if (m_world.HasAnimatedCharacter()) {
            h.anim.Bind(&m_world.GltfCharacterModel(), m_world.GltfCharacterSkinIndex());
            h.anim.Play("Walk");
        }
        m_hiders.push_back(h);
    }

    // 秘匿ブロックを ShuffleAllocator のスロットに確保（PROMPT-V3-07：難読化）。
    // 以降このブロックは毎フレーム別アドレスへ移動し、メモリスキャナは固定
    // アドレスを掴めない。値は m_hider ハンドル経由でのみ触る。
    m_hider = m_secretAlloc.Allocate<secrets::HiderSecretBlock>();
    if (!m_hider.IsValid()) {
        LOG_ERROR("HideSeekMode::Init: hider 座標ブロックの確保に失敗しました");
        return false;
    }

    // 署名付きブロックへ全 hider 座標をコピー（チート検証ツールの読み取り対象）。
    // 位置は固定なのでここで一度だけ書けばよい。
    secrets::HiderSecretBlock* sec = m_hider.Get();
    sec->magic = secrets::kHiderMagic;
    sec->count = static_cast<uint32_t>(
        m_hiders.size() < secrets::kMaxHiders ? m_hiders.size() : secrets::kMaxHiders);
    for (uint32_t i = 0; i < sec->count; ++i) {
        sec->positions[i] = { m_hiders[i].position.x,
                              m_hiders[i].position.y,
                              m_hiders[i].position.z };
    }

    // 整合性ウォッチドッグに hider 座標ブロックを登録（PROMPT-V3-07：改ざん検知）。
    // アドレスは毎フレーム動くので、チェック時に Handle から現在地を取り直す。
    m_secretToken = m_watchdog.Register(
        "hide.hiderPositions",
        [this]() -> IntegrityWatchdog::Region {
            return { m_hider.Get(), sizeof(secrets::HiderSecretBlock) };
        });

    // 透視用の深度制御帯（FPS と同じ署名）。cheat_tester --wallhack がここを書き換える。
    m_renderCtrl = m_secretAlloc.Allocate<secrets::RenderControlBlock>();
    if (!m_renderCtrl.IsValid()) {
        LOG_ERROR("HideSeekMode::Init: 深度制御帯の確保に失敗しました");
        return false;
    }
    {
        secrets::RenderControlBlock* rc = m_renderCtrl.Get();
        rc->magic          = secrets::kRenderCtrlMagic;
        rc->depthCheatFlag = 0;   // 既定＝通常（hider は障害物の陰なら隠れる）
        rc->reserved       = 0;
    }
    m_ctrlToken = m_watchdog.Register(
        "hide.renderCtrl",
        [this]() -> IntegrityWatchdog::Region {
            return { m_renderCtrl.Get(), sizeof(secrets::RenderControlBlock) };
        });


    // ステージJ-02: 描画フック型チート（testbed）を継ぎ目に設置。hider ドローはこのフックを
    // 通り、外部 cheat_tester が depthCheatFlag を立てている間だけ深度 ALWAYS で壁越しになる。
    hookcheat::Install(*m_renderer, [this]() {
        return m_renderCtrl.IsValid() && (m_renderCtrl.Get()->depthCheatFlag != 0);
    });
    m_security.Init(ctx.security,*m_renderer,m_watchdog,{m_secretToken,m_ctrlToken},true);

    AudioPlayer::Get().PlayBgm();      // BGM 開始
    LOG_INFO("個別対策は SECURITY ログとHUDを参照。P/L1=難読化、F6=監視、F7=深度門番、M/R1=タグ。");

    // 操作方式: コントローラー主体。カーソルはロックしない（別窓のチート操作を邪魔しない）。
    // 視点はコントローラー右スティック（マウス視点は無効）。
    if (m_window != nullptr) {
        m_window->GetInput().SetCursorLock(false, m_window->GetHandle());
    }

    Reset();
    return true;
}

void HideSeekMode::Reset() {
    m_player.position = kSeekerStart;
    m_player.yaw   = 0.0f;
    m_player.pitch = 0.0f;
    m_prevPos = kSeekerStart;

    m_elapsed    = 0.0f;
    m_distance   = 0.0f;
    m_foundCount = 0;
    m_finished   = false;

    // hider を初期の隠れ場所へ戻し、徘徊状態も初期化する。
    const int spotCount = static_cast<int>(sizeof(kHiderSpots) / sizeof(kHiderSpots[0]));
    for (int i = 0; i < static_cast<int>(m_hiders.size()); ++i) {
        Hider& h = m_hiders[i];
        h.position    = kHiderSpots[i % spotCount];
        h.target      = h.position;
        h.wanderTimer = RandRange(0.5f, kWanderInterval);
        h.yaw         = 0.0f;
        h.found       = false;
    }

    LOG_INFO("かくれんぼ開始！ %d 人を探せ（WASD 移動 / マウス視点 / "
             "R リスタート / P 防御ON/OFF / T 改ざん実演 / ESC 終了）",
             static_cast<int>(m_hiders.size()));
}

void HideSeekMode::Update(float dt, const Input& input) {
    // ステージL-04b: glTF モデルのアニメーションを進める。
    m_world.Update(dt);

    // R または △ ボタンでリスタート（押下エッジ）
    const bool restartNow = input.IsKeyDown('R') || input.IsPadDown(GamepadButton::Triangle);
    if (restartNow && !m_prevRestart) Reset();
    m_prevRestart = restartNow;

    // P=難読化、F6=監視、F7=深度門番、M=タグマスク。
    // Start()/Stop() は監視スレッド join のため AcquireLock() の外で呼ぶ（デッドロック回避）。
    m_security.Update(input);

    // 透視 ESP は base には無い（外部 game_cheat.dll が担う）。O トグルは撤去。

    // T の押下エッジ（改ざん実演。実際の書き換えは秘匿ブロック更新の中で行う）
    const bool tamperNow  = input.IsKeyDown('T');
    const bool tamperEdge = (tamperNow && !m_prevTamper);
    m_prevTamper = tamperNow;

    const std::vector<AABB>& obstacles = m_world.GetObstacles();

    // 探す側の移動（既存 Player ロジック＝WASD＋マウス＋障害物衝突）
    m_player.Update(dt, input, obstacles);
    m_camera.SetPosition(m_player.EyePosition());
    m_camera.SetRotation(m_player.yaw, m_player.pitch);

    // 累積移動距離（XZ）。クリア後は加算しない。
    const float ddx = m_player.position.x - m_prevPos.x;
    const float ddz = m_player.position.z - m_prevPos.z;
    if (!m_finished) m_distance += sqrtf(ddx * ddx + ddz * ddz);
    m_prevPos = m_player.position;

    const XMFLOAT3 eye = m_player.EyePosition();

    // 発見リアクション（跳ね）の寿命を減らす。
    for (Hider& h : m_hiders) if (h.foundPop > 0.0f) h.foundPop -= dt;

    // --- hider の AI 移動（徘徊＋seeker接近で逃走）。PROMPT-G-03 ---
    // 未発見の hider のみ動く（発見済み・クリア後は静止）。
    if (!m_finished) {
        for (Hider& h : m_hiders) {
            if (h.found) continue;

            const XMFLOAT3 hc{ h.position.x, h.position.y + kHiderHalf.y, h.position.z };
            const float sdx = h.position.x - m_player.position.x;
            const float sdz = h.position.z - m_player.position.z;
            const float sdist = sqrtf(sdx * sdx + sdz * sdz);
            const bool fleeing =
                (sdist < kFleeRange && sdist > 1e-3f && LineClear(eye, hc, obstacles));

            float tx, tz, speed;
            if (fleeing) {
                // seeker と反対方向へ逃げる
                tx = h.position.x + sdx / sdist * 4.0f;
                tz = h.position.z + sdz / sdist * 4.0f;
                speed = kHiderFlee;
            } else {
                // 徘徊: 目標へ。到達 or タイマー切れで新しい目標を選ぶ。
                h.wanderTimer -= dt;
                const float wdx = h.target.x - h.position.x;
                const float wdz = h.target.z - h.position.z;
                if (sqrtf(wdx * wdx + wdz * wdz) <= kWanderReach || h.wanderTimer <= 0.0f) {
                    h.target = { RandRange(-kArenaBound, kArenaBound), 0.0f,
                                 RandRange(-kArenaBound, kArenaBound) };
                    h.wanderTimer = kWanderInterval;
                }
                tx = h.target.x; tz = h.target.z; speed = kHiderWander;
            }

            const float mdx = tx - h.position.x, mdz = tz - h.position.z;
            const float mdist = sqrtf(mdx * mdx + mdz * mdz);
            if (mdist > 1e-3f) {
                const float step = speed * dt;
                const float px = h.position.x, pz = h.position.z; // 移動前
                XMFLOAT3 center{ h.position.x, h.position.y + kHiderHalf.y, h.position.z };
                XMFLOAT3 vel{ mdx / mdist * step, 0.0f, mdz / mdist * step };
                XMFLOAT3 resolved = ResolveAABBCollision(center, kHiderHalf, vel, obstacles);
                h.position.x = resolved.x;
                h.position.z = resolved.z;
                // 場外へ出ないようクランプ
                if (h.position.x < -kArenaBound) h.position.x = -kArenaBound;
                if (h.position.x >  kArenaBound) h.position.x =  kArenaBound;
                if (h.position.z < -kArenaBound) h.position.z = -kArenaBound;
                if (h.position.z >  kArenaBound) h.position.z =  kArenaBound;
                h.yaw = YawFromDir(mdx, mdz);
                // 実際に動いた距離ぶんだけ歩行位相を進める。
                const float mvx = h.position.x - px, mvz = h.position.z - pz;
                h.walkPhase += sqrtf(mvx * mvx + mvz * mvz) * kHiderWalkRate;
            }

            // hider の Swat 化: 逃走中=走る(Run)、徘徊中=歩く(Walk)。未 Bind なら
            // AnimationPlayer::Play/Update は何もしない（IsValid()==false で早期 return）。
            if (m_world.HasAnimatedCharacter()) {
                h.anim.Play(fleeing ? "Run" : "Walk");
                h.anim.Update(dt);
            }
        }
    }

    // --- 発見判定（目から hider へのレイが障害物に遮られず kFindRange 以内）---
    if (!m_finished) {
        m_elapsed += dt;
        for (Hider& h : m_hiders) {
            if (h.found) continue;
            const XMFLOAT3 c{ h.position.x, h.position.y + kHiderHalf.y, h.position.z };
            const float dx = c.x - eye.x, dy = c.y - eye.y, dz = c.z - eye.z;
            const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist > kFindRange || dist < 1e-3f) continue;
            if (!LineClear(eye, c, obstacles)) continue;

            h.found = true;
            h.foundPop = kFoundPopTime; // 発見リアクション（跳ねる）
            AudioPlayer::Get().PlayFound();
            ++m_foundCount;
            LOG_INFO("発見！ %d/%d （%.1f 秒経過）",
                     m_foundCount, static_cast<int>(m_hiders.size()), m_elapsed);
            if (m_foundCount == static_cast<int>(m_hiders.size())) {
                m_finished = true;
                LOG_INFO("★全員発見！ タイム %.1f 秒 / 移動距離 %.1f m（R で再挑戦）",
                         m_elapsed, m_distance);
            }
        }
    }

    // --- 秘匿ブロックの更新（難読化＋live 座標反映＋改ざん実演）を監視スレッド排他で ---
    {
        auto lock = m_watchdog.AcquireLock();
        // 難読化（PROMPT-V3-07 / H-03）: 防御 ON のときだけ毎フレーム別スロットへ移動。
        if (m_security.State().shuffle) m_secretAlloc.ShuffleSlots();

        secrets::HiderSecretBlock* sec = m_hider.Get();
        if (sec != nullptr) {
            // 動く hider の現在座標を署名ブロックへ反映（cheat_tester が読む対象）。
            sec->magic = secrets::kHiderMagic;
            const uint32_t n = static_cast<uint32_t>(
                m_hiders.size() < secrets::kMaxHiders ? m_hiders.size()
                                                      : secrets::kMaxHiders);
            sec->count = n;
            for (uint32_t i = 0; i < n; ++i) {
                sec->positions[i] = { m_hiders[i].position.x,
                                      m_hiders[i].position.y,
                                      m_hiders[i].position.z };
            }
            // 防御 ON のときだけ、正規更新として監視基準を取り直す（live 座標が基準）。
            if (m_security.State().watchdog) m_watchdog.Commit(m_secretToken);

            // T 改ざん実演: Commit 後に不正書き換え（Commit なし）＝監視周期で検知され得る。
            // SECURITY NOTE: 自作プロセス内での検証用シミュレーション（外部攻撃ではない）。
            // ※ 動く標的では毎フレーム再 Commit されるため、ステージH で対策トグルとして整理する。
            if (tamperEdge && n > 0) {
                sec->positions[0].x += 5.0f;
                LOG_WARN("【改ざん実演】hider[0] の座標を不正に書き換えました（Commit なし）。");
            }
        }

        // ※ ステージJ-03 以降、hider 透視の無効化は「描画直前に深度 ALWAYS を拒絶する門番」
        //   (Renderer::SetEnemyDepthGuard) が担う。ここでの depthCheatFlag 修復は撤去した
        //   （難読化＝アドレス移動は上の ShuffleSlots で継続。整合性監視は Watchdog が担う）。
    }

    UpdateHud();
}

void HideSeekMode::UpdateHud() {
    if (m_window == nullptr) return;
    wchar_t title[220];
    swprintf_s(title, L"かくれんぼ  発見 %d/%d  タイム %.1fs  距離 %.1fm  %ls %ls%ls",
               m_foundCount, static_cast<int>(m_hiders.size()),
               m_elapsed, m_distance,
               m_security.Label(),
               L"",
               m_finished ? L"  [クリア! R で再挑戦]" : L"");
    SetWindowTextW(m_window->GetHandle(), title);
}

void HideSeekMode::Render(Renderer& renderer) {
    renderer.BeginFrame(m_camera);

    // アリーナ（床＋障害物）。隠し直方体(RenderHidden)は呼ばない。
    m_world.Render(renderer);

    // hider を描画（未発見=黄, 発見済=緑）。胴+頭＋振れる脚。発見時は少し跳ねる。
    // ステージJ-02: hider は専用入口 DrawEnemyTagged 経由（Enemy タグ）で描くだけ。ゲームは
    // 透視の有無を判断しない。継ぎ目に差し込まれたフックが、外部 cheat_tester の発動フラグに
    // 従って hider ドローを深度 ALWAYS にすり替える（フック未設置なら常に通常の深度で隠れる）。
    for (const Hider& h : m_hiders) {
        float hop = 0.0f;
        if (h.foundPop > 0.0f) {
            const float p = 1.0f - h.foundPop / kFoundPopTime; // 0..1
            hop = sinf(p * 3.14159265f) * 0.3f;
        }
        if (m_world.HasAnimatedCharacter()) {
            // Swat 化: DrawEnemyTaggedSkinned 経由で描くので、ウォールハック実演・防御・
            // 段[5]検知は箱モデルと同様にそのまま効く。発見リアクションは Y の跳ねだけ流用
            // （専用クリップは持たせず、既存の「見つかった瞬間ぴょんと跳ねる」演出を踏襲）。
            const XMFLOAT3 pos{ h.position.x, h.position.y + hop, h.position.z };
            m_world.DrawGltfCharacterTagged(renderer, pos, h.yaw, h.anim, kHiderScale);
            continue;
        }
        const XMMATRIX world = XMMatrixRotationY(h.yaw) *
            XMMatrixTranslation(h.position.x, h.position.y + hop, h.position.z);
        renderer.DrawEnemyTagged(h.found ? m_hiderFoundBodyMesh : m_hiderBodyMesh, world);
        // 脚を股関節・腕を肩で振る（未発見のみ。腕は脚と逆位相）。
        const float legSw = h.found ? 0.0f : sinf(h.walkPhase) * kHiderLegSwing;
        const float armSw = h.found ? 0.0f : sinf(h.walkPhase) * kHiderArmSwing;
        renderer.DrawEnemyTagged(m_hiderLegMesh,
            XMMatrixRotationX(legSw)  * XMMatrixTranslation(-kHiderHipX, kHiderHipY, 0.0f) * world);
        renderer.DrawEnemyTagged(m_hiderLegMesh,
            XMMatrixRotationX(-legSw) * XMMatrixTranslation( kHiderHipX, kHiderHipY, 0.0f) * world);
        renderer.DrawEnemyTagged(m_hiderArmMesh,
            XMMatrixRotationX(-armSw) * XMMatrixTranslation(-kHiderShoulderX, kHiderShoulderY, 0.0f) * world);
        renderer.DrawEnemyTagged(m_hiderArmMesh,
            XMMatrixRotationX(armSw)  * XMMatrixTranslation( kHiderShoulderX, kHiderShoulderY, 0.0f) * world);
    }

    // [旧チート・無効化] 協調型 ESP（ゲームが全 hider 座標を DLL に手渡し、espOn 指示で
    //   深度無視の赤箱を描く方式）は撤去。ゲームはチートに情報を渡さない。hider 座標の実値は
    //   秘匿ブロック(m_hider)としてメモリに存在し続けるので、外部 cheat_tester が自力で走査して
    //   読み取る（＝ゲーム無協力の位置透視。描画は行わない）。
    //   参考（旧コード）:
    //     if (m_cheat != nullptr) { cheat::HostState st{}; ...; m_cheat->Tick(st, cmd);
    //         if (cmd.espOn) { ...DrawMesh(m_hiderEspMesh, world, /*ignoreDepth=*/true); } }

    // クリア中はクリア画面、そうでなければ照準を最前面に描く。
    if (m_finished) renderer.DrawOverlay(m_clearOverlay);
    else            renderer.DrawOverlay(m_crosshair);
    renderer.EndFrame();
}

void HideSeekMode::Shutdown() {
    // フックを継ぎ目から外す（provider が捕捉する this/Handle が壊れる前に）。
    if (m_renderer != nullptr) hookcheat::Uninstall(*m_renderer);
    // 監視スレッドを先に止める（Provider が参照する m_secretAlloc / m_hider が
    // 破棄される前に）。デストラクタでも Stop されるが明示する。
    m_watchdog.Stop();
    // World・Mesh・vector は値/ComPtr/標準コンテナで自動解放。明示処理は不要。
}
