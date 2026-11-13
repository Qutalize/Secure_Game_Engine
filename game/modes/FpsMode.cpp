#include "game/modes/FpsMode.h"

#include <cmath>
#include <cstring>
#include <new>

#include "engine/core/Logger.h"
#include "engine/core/Window.h"
#include "engine/renderer/Renderer.h"
#include "engine/memory/LinearAllocator.h"
#include "engine/network/NetworkManager.h"
#include "engine/math/Collision.h"

#include "game/World.h"
// #include "game/CheatModule.h" // [旧チート・無効化] 協調型 DLL 手渡し方式は撤去
#include "game/SecretSignatures.h" // 深度制御帯（RenderControlBlock）
#include "game/CharacterMesh.h"    // テーパー角柱（人型を滑らかに）
#include "cheat/HookCheat.h"        // ステージJ-02: 描画フック型チート（継ぎ目に差し込む）
#include "game/OverlayText.h"   // クリア画面のオーバーレイ

using namespace DirectX;

// ---------------------------------------------------------------------------
// シューティングバトル用の定数・ヘルパー（PROMPT-V3-03）
// ---------------------------------------------------------------------------
namespace {

constexpr float kPlayerMaxHealth = 100.0f;
const XMFLOAT3  kPlayerStart{ 0.0f, 0.0f, -15.0f }; // 広い闘技場の南側から開始

const XMFLOAT3  kEnemyHalf{ 0.45f, 0.9f, 0.45f }; // 敵の当たり判定半サイズ
constexpr float kEnemyMaxHealth = 100.0f;
constexpr float kEnemyPatrolSpeed = 1.8f; // m/s（巡回）
constexpr float kEnemyChaseSpeed  = 3.2f; // m/s（追跡）
constexpr float kEnemyRespawn   = 4.0f;   // 撃破後の再出現までの秒数
constexpr float kContactDist    = 1.6f;   // この距離以内で接触ダメージ
constexpr float kContactDps     = 25.0f;  // 接触ダメージ（毎秒）
constexpr float kWaypointReach  = 1.0f;   // 巡回端点への到達判定距離

// 敵は近距離で初めて反応する。広いアリーナの反対側から一斉に寄って来ないよう、
// 発見後の保持距離・射程も索敵距離に合わせて抑える。
constexpr float kDetectRange = 9.0f;   // この距離＋視線が通ればプレイヤーを発見→追跡
constexpr float kLoseRange   = 12.0f;  // この距離を超えると見失って巡回に戻る
constexpr float kFireRange   = 10.0f;  // 敵が撃ってくる最大距離
constexpr float kEnemyShotDamage   = 7.0f;  // 敵1発の被ダメージ
constexpr float kEnemyFireInterval = 1.2f;  // 敵の射撃間隔（秒）

constexpr float kShotDamage = 50.0f;   // プレイヤー1発のダメージ（2発で撃破）
constexpr float kShotRange  = 100.0f;  // プレイヤーの射程（m）

constexpr int   kClearKills   = 10;    // この撃破数でクリア
constexpr float kSpawnProtect = 2.5f;  // 復活直後の無敵時間（秒）

// 敵の初期配置＝巡回端点 A、および巡回端点 B（A↔B を往復）。
//
// ★モジュール式アリーナ（World::BuildFpsArenaModules）のレイアウトと対で設計してある★
//   中央に 13m x 10m の建物（x∈[-6.5,6.5] / z∈[-5,5]）があり、その周囲を回る「リング状の
//   通路」を巡回レーンにしている（東西 x=±9 / 南北 z=±8 / 北の縦道 x=0）。遮蔽物は
//   このレーンを避けて外側（|x|>11 または |z|>11）に置かれている。
//   ※ 敵 AI は経路探索を持たず目標へ直進するだけなので、レーン上に遮蔽物を足すと
//     壁に押し付けられて往復できなくなる。World 側の配置を変えるときはここも一緒に見直すこと。
struct SpawnCfg { XMFLOAT3 a; XMFLOAT3 b; };
const SpawnCfg kEnemySpawns[] = {
    { {   9.0f, 0.0f,   8.0f }, {   9.0f, 0.0f,  -8.0f } }, // 建物の東側を南北に往復
    { {  -9.0f, 0.0f,  -8.0f }, {  -9.0f, 0.0f,   8.0f } }, // 建物の西側を南北に往復
    { {  -8.0f, 0.0f,   8.0f }, {   8.0f, 0.0f,   8.0f } }, // 建物の北側を東西に往復
    { {   8.0f, 0.0f,  -8.0f }, {  -8.0f, 0.0f,  -8.0f } }, // 建物の南側（プレイヤー側）
    { {   0.0f, 0.0f,  12.0f }, {   0.0f, 0.0f,  17.0f } }, // 北の外周を短く往復
};

// 法線付きボックスを v/idx に追記する（中心 center・半サイズ half・色 color）。
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

// 単色の箱メッシュ（足元 y=0 原点、指定した高さ）。ESP 箱に使う。
Mesh MakeEspBox(Renderer& r, const XMFLOAT4& color) {
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    // 少し大きめ・全高 1.9m（足元 0 〜 1.9）。壁越しに光って見える。
    AddBox(v, idx, XMFLOAT3{ 0.0f, 0.95f, 0.0f }, XMFLOAT3{ 0.55f, 0.98f, 0.55f }, color);
    return r.CreateMesh(v, idx);
}

// 歩行アニメ定数（腕・脚を関節ピボットで前後に振る）。
constexpr float kHipY       = 0.81f; // 股関節の高さ（脚メッシュのピボット位置。ブーツ底が床に接地）
constexpr float kHipX       = 0.15f; // 股関節の左右オフセット
constexpr float kShoulderY  = 1.42f; // 肩の高さ（腕メッシュのピボット位置）
constexpr float kShoulderX  = 0.34f; // 肩の左右オフセット
constexpr float kLegSwing   = 0.55f; // 脚の振り幅（ラジアン）
constexpr float kArmSwing   = 0.42f; // 腕の振り幅（ラジアン。脚と逆位相）
constexpr float kWalkRate   = 4.5f;  // 歩行位相の進み（1m 移動あたり rad）

// 「ガード（衛兵）」風の敵の胴体メッシュ（脚を除く。足元 y=0 原点・正面 +Z）。
// 脚は別メッシュ(MakeGuardLeg)で股関節ピボット回転させて歩かせる。
// 敵（衛兵）の胴体＝テーパー角柱の人型トルソ（腰→胸→肩で太さが変わる）＋首＋頭＋
// ヘルメット＋バイザー＋背中の銃。腕/脚は別メッシュで関節スイングさせる。
Mesh MakeGuardBody(Renderer& r) {
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    const XMFLOAT4 cloth{ 0.19f, 0.23f, 0.31f, 1.0f };
    const XMFLOAT4 vest { 0.24f, 0.28f, 0.36f, 1.0f };
    const XMFLOAT4 skin { 0.86f, 0.71f, 0.58f, 1.0f };
    const XMFLOAT4 helm { 0.13f, 0.15f, 0.19f, 1.0f };
    const XMFLOAT4 visor{ 0.95f, 0.18f, 0.16f, 1.0f };
    const XMFLOAT4 gun  { 0.09f, 0.09f, 0.11f, 1.0f };
    // トルソ: 腰(細)→腹→胸→肩(広) をテーパー角柱で連続的に。
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 0.78f, 1.06f, 0.19f, 0.14f, 0.25f, 0.17f, cloth); // 腰→腹
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 1.06f, 1.40f, 0.25f, 0.17f, 0.34f, 0.19f, vest);  // 胸→肩
    // 首→頭→ヘルメット（先細り）。
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 1.40f, 1.52f, 0.10f, 0.10f, 0.085f, 0.085f, skin); // 首
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 1.52f, 1.76f, 0.15f, 0.16f, 0.16f, 0.16f, skin);   // 頭
    charmesh::AddTaper(v, idx, 0.0f, 0.0f, 1.72f, 1.92f, 0.20f, 0.20f, 0.12f, 0.12f, helm);   // ヘルメット
    AddBox(v, idx, XMFLOAT3{ 0.0f, 1.66f, 0.16f }, XMFLOAT3{ 0.12f, 0.045f, 0.03f }, visor);  // バイザー
    AddBox(v, idx, XMFLOAT3{ 0.06f, 1.16f, -0.22f }, XMFLOAT3{ 0.045f, 0.05f, 0.22f }, gun);  // 背中の銃
    return r.CreateMesh(v, idx);
}

// 腕 1 本（袖＝服色 / 手＝肌）。肩ピボット(原点)から下へ、上腕→前腕がテーパーで先細り＋手。
Mesh MakeGuardArm(Renderer& r) {
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    const XMFLOAT4 sleeve{ 0.19f, 0.23f, 0.31f, 1.0f };
    const XMFLOAT4 skin  { 0.86f, 0.71f, 0.58f, 1.0f };
    charmesh::AddTaper(v, idx, 0.0f, 0.0f,  -0.38f, 0.00f, 0.078f, 0.088f, 0.10f, 0.11f, sleeve); // 上腕
    charmesh::AddTaper(v, idx, 0.0f, 0.02f, -0.66f, -0.38f, 0.062f, 0.072f, 0.078f, 0.088f, sleeve); // 前腕
    AddBox(v, idx, XMFLOAT3{ 0.0f, -0.72f, 0.03f }, XMFLOAT3{ 0.066f, 0.07f, 0.08f }, skin);          // 手
    return r.CreateMesh(v, idx);
}

// 脚 1 本（ズボン＝テーパー＋ブーツ）。股関節ピボット(原点)から下へ、腿→脛→ブーツ。
Mesh MakeGuardLeg(Renderer& r) {
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    const XMFLOAT4 pants{ 0.15f, 0.17f, 0.23f, 1.0f };
    const XMFLOAT4 boot { 0.08f, 0.07f, 0.07f, 1.0f };
    charmesh::AddTaper(v, idx, 0.0f, 0.0f,  -0.44f, 0.00f, 0.105f, 0.125f, 0.13f, 0.14f, pants); // 腿
    charmesh::AddTaper(v, idx, 0.0f, 0.01f, -0.72f, -0.44f, 0.085f, 0.10f, 0.105f, 0.125f, pants); // 脛
    AddBox(v, idx, XMFLOAT3{ 0.0f, -0.75f, 0.05f }, XMFLOAT3{ 0.10f, 0.06f, 0.17f }, boot);        // ブーツ（前へ）
    return r.CreateMesh(v, idx);
}

// 敵の現在 AABB（中心は足元 + 半身長）
AABB EnemyAABB(const Enemy& e) {
    const XMFLOAT3 c{ e.position.x, e.position.y + kEnemyHalf.y, e.position.z };
    return AABB{
        { c.x - kEnemyHalf.x, c.y - kEnemyHalf.y, c.z - kEnemyHalf.z },
        { c.x + kEnemyHalf.x, c.y + kEnemyHalf.y, c.z + kEnemyHalf.z }
    };
}

// a→b の視線が障害物に遮られていなければ true（LOS）。
bool HasLineOfSight(const XMFLOAT3& a, const XMFLOAT3& b,
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

// +Z を基準（yaw=0）とした向き。dir(x,z) から yaw を求める。
float YawFromDir(float dx, float dz) { return atan2f(dx, dz); }

// 単色の箱メッシュ（中心 center・半 half・色 color）。弾道/フラッシュ用。
Mesh MakeBoxMesh(Renderer& r, const XMFLOAT3& center, const XMFLOAT3& half,
                 const XMFLOAT4& color) {
    std::vector<Vertex> v; std::vector<uint32_t> idx;
    AddBox(v, idx, center, half, color);
    return r.CreateMesh(v, idx);
}

// ローカル +Z を dir に向け、長さ len・太さ thick にして start に置くワールド行列。
// 弾道（+Z 方向の単位箱 z:[0,1]）を「銃口→着弾点」へ伸ばすのに使う。
XMMATRIX AimMatrix(const XMFLOAT3& start, const XMFLOAT3& dir, float len, float thick) {
    XMVECTOR f = XMVector3Normalize(XMLoadFloat3(&dir));
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (fabsf(XMVectorGetX(XMVector3Dot(f, up))) > 0.99f)
        up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f); // 真上/真下を向くときの退避
    XMVECTOR r = XMVector3Normalize(XMVector3Cross(up, f));
    XMVECTOR u = XMVector3Cross(f, r);
    XMMATRIX rot;
    rot.r[0] = r; rot.r[1] = u; rot.r[2] = f;
    rot.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    return XMMatrixScaling(thick, thick, len) * rot *
           XMMatrixTranslation(start.x, start.y, start.z);
}

// 一人称ビューモデル（銃）の位置・向き。ViewModelTransform（描画用の world 行列）と
// ViewModelMuzzlePos（発砲エフェクトの起点）の両方がこの同じ基底/オフセットから計算する
// ことで、「銃の見た目の位置」と「マズルフラッシュ/弾道の発生位置」を一致させる。
// ※以前は発砲エフェクトをカメラ位置（画面中央＝照準側）から出していたため、画面右下に
//   表示される銃と位置が食い違って見える不具合があった。
struct ViewModelPose {
    DirectX::XMVECTOR pos, right, up, forward;
};

ViewModelPose ComputeViewModelPose(const Camera& camera) {
    ViewModelPose p{};
    const XMVECTOR eye = XMLoadFloat3(&camera.GetPosition());
    p.forward = camera.GetForward();
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    p.right = XMVector3Normalize(XMVector3Cross(worldUp, p.forward));
    p.up    = XMVector3Cross(p.forward, p.right);

    // オフセット（メートル）。右へ・下へ・前へ。見た目で調整する前提の初期値。
    constexpr float kRightOffset = 0.22f, kDownOffset = 0.18f, kFwdOffset = 0.35f;
    XMVECTOR pos = eye;
    pos = XMVectorAdd(pos, XMVectorScale(p.right, kRightOffset));
    pos = XMVectorSubtract(pos, XMVectorScale(p.up, kDownOffset));
    pos = XMVectorAdd(pos, XMVectorScale(p.forward, kFwdOffset));
    p.pos = pos;
    return p;
}

// AimMatrix と同じ「right/up/forward の直交基底」で world 行列を組む。ignoreDepth で
// 描画する前提なので、壁に近づいても銃がめり込んで消えることはない。
XMMATRIX ViewModelTransform(const Camera& camera) {
    const ViewModelPose p = ComputeViewModelPose(camera);
    XMMATRIX basis;
    basis.r[0] = p.right; basis.r[1] = p.up; basis.r[2] = p.forward;
    basis.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

    // モデルのローカル前方軸がカメラ前方と逆を向いて見えたら XM_PI に変更する（要目視調整）。
    constexpr float kGunYawOffset = 0.0f;
    return XMMatrixRotationY(kGunYawOffset) * basis * XMMatrixTranslationFromVector(p.pos);
}

// マズルフラッシュ／弾道の発生位置（銃口の近似座標）。ViewModelTransform と同じ配置に、
// 銃身ぶんの前方オフセットを足すだけ（メッシュのローカル座標までは追わず、見た目が
// 大きくズレない程度の近似で十分という判断）。命中判定のレイ（eye/dir）はここを使わず
// 従来どおりカメラ位置基準のまま＝照準（画面中央のクロスヘア）でのエイムは変えない。
XMFLOAT3 ViewModelMuzzlePos(const Camera& camera) {
    const ViewModelPose p = ComputeViewModelPose(camera);
    constexpr float kMuzzleForward = 0.55f; // 銃口までの追加前方オフセット（メートル）
    const XMVECTOR muzzle = XMVectorAdd(p.pos, XMVectorScale(p.forward, kMuzzleForward));
    XMFLOAT3 out;
    XMStoreFloat3(&out, muzzle);
    return out;
}

} // namespace

// unique_ptr<LinearAllocator> は不完全型のままヘッダで実体化できないため、
// LinearAllocator の完全定義が見えるこの cpp でコンストラクタ／デストラクタを定義する。
FpsMode::FpsMode() = default;
FpsMode::~FpsMode() = default;

// 敵とプレイヤーを初期状態へ戻す（開始 / 死亡リスポーン / 再プレイで共通）。
// 撃破数(m_kills)はここではリセットしない（呼び出し側で必要に応じて 0 にする）。
void FpsMode::ResetEncounter() {
    if (m_player.IsValid()) {
        m_player->health   = kPlayerMaxHealth;
        m_player->position = kPlayerStart;
    }
    m_spawnProtect = kSpawnProtect; // 復活直後の無敵
    for (Enemy& e : m_enemies) {
        e.position     = e.spawnPos;
        e.health       = kEnemyMaxHealth;
        e.alive        = true;
        e.state        = Enemy::State::Patrol;
        e.toB          = true;
        e.fireCooldown = 0.0f;
        e.hitFlash     = 0.0f;
        e.respawnTimer = 0.0f;
    }
}

bool FpsMode::Init(const GameContext& ctx) {
    m_renderer = ctx.renderer;
    m_net      = ctx.net;
    m_window   = ctx.window;
    // m_cheat = ctx.cheat; // [旧チート・無効化] 協調型 DLL は撤去（ゲームはチートを知らない）
    if (m_renderer == nullptr) {
        LOG_ERROR("FpsMode::Init: Renderer が null です");
        return false;
    }

    // パッケージ化（J-05）: 対策版(-DHARDENED=ON)は防御を既定 ON で起動する（通常版は OFF）。

    // --- World を LinearAllocator 上に確保（PROMPT-05/08 と同じ構成）---
    m_gameMemory = std::make_unique<LinearAllocator>(64ull * 1024 * 1024);
    void* worldMem = m_gameMemory->Allocate(sizeof(World), alignof(World));
    m_world = new (worldMem) World();
    m_world->Build(*m_renderer, WorldLayout::FpsArena); // PROMPT-G-02: 広い闘技場

    // ライティング（視覚向上）: 低めの暖色サン＋やや寒色の環境光で立体感/コントラストを出す。
    m_renderer->SetDirectionalLight(
        XMFLOAT3{ -0.5f, -0.85f, 0.35f }, XMFLOAT3{ 1.0f, 0.92f, 0.78f },
        /*intensity=*/1.15f, XMFLOAT3{ 0.30f, 0.34f, 0.44f });

    // --- Player を ShuffleAllocator::Handle で確保（毎フレーム別スロットへ移動）---
    m_playerAlloc = std::make_unique<ShuffleAllocator>(sizeof(Player), 64);
    m_player = m_playerAlloc->Allocate<Player>();
    m_player->playerId = 1;
    m_player->health   = kPlayerMaxHealth;
    m_player->position = kPlayerStart;
    LOG_INFO("Player health のアドレス（参考: シャッフルで毎フレーム変わる）= %p",
             static_cast<void*>(&m_player->health));

    // --- 照準（画面中央の 2D クロスヘア）---
    m_crosshair = m_renderer->CreateCrosshairMesh();

    // --- 敵（標的）の生成（PROMPT-V3-03 / G-02 巡回AI / I-03 歩行アニメ）---
    m_guardBodyMesh = MakeGuardBody(*m_renderer);
    m_guardArmMesh  = MakeGuardArm(*m_renderer);
    m_guardLegMesh  = MakeGuardLeg(*m_renderer);
    // [旧チート・無効化] 専用 ESP 箱は廃止。タイプA では敵メッシュ自体を深度ALWAYS で
    //   描いて壁越しにする（DrawEnemyMesh）。MakeEspBox は参照用に残置（未使用）。
    // m_enemyEspMesh = MakeEspBox(*m_renderer, XMFLOAT4{ 0.1f, 1.0f, 0.25f, 1.0f });

    // --- タイプA ウォールハックの「深度制御帯」を ShuffleAllocator 上に確保 ---
    // depthCheatFlag=0（通常）で初期化。外部 cheat_tester --wallhack がこの署名ブロックを
    // 走査で発見し WriteProcessMemory で非0 に書き換える＝敵が壁越しに見える。ゲームは
    // このフラグを毎フレーム読むだけで、チートの存在を一切知らない（無自覚な被害者）。
    // ※ ステージJ-02 では、これに加えて「描画インターセプトの継ぎ目(DrawEnemyTagged)を
    //   フックする」方式のチートも足す予定。本ブロック方式（外部書換）は当面併存させる。
    m_renderCtrl = m_ctrlAlloc.Allocate<secrets::RenderControlBlock>();
    if (!m_renderCtrl.IsValid()) {
        LOG_ERROR("FpsMode::Init: 描画制御ブロックの確保に失敗しました");
        return false;
    }
    {
        secrets::RenderControlBlock* rc = m_renderCtrl.Get();
        rc->magic          = secrets::kRenderCtrlMagic;
        rc->depthCheatFlag = 0;   // 既定＝通常（壁で隠れる）
        rc->reserved       = 0;
    }
    // 整合性ウォッチドッグに登録（改ざん検知＝防御①）。ブロックは移動するので
    // Provider が「現在のアドレス」を返す（難読化と改ざん検知の両立）。
    m_ctrlToken = m_watchdog.Register(
        "fps.renderCtrl",
        [this]() -> IntegrityWatchdog::Region {
            return { m_renderCtrl.Get(), sizeof(secrets::RenderControlBlock) };
        });

    // ステージJ-02: 描画フック型チート（testbed）を継ぎ目に設置する。以降、敵ドローは
    // このフック(Interceptor)を通り、外部 cheat_tester が depthCheatFlag を立てている間だけ
    // 敵が深度 ALWAYS で描かれる。ゲームのゲームロジックは透視を一切知らない。
    // 発動判定は「深度制御帯の現在値」を Handle 経由で読む（難読化で移動しても追随する）。
    hookcheat::Install(*m_renderer, [this]() {
        return m_renderCtrl.IsValid() && (m_renderCtrl.Get()->depthCheatFlag != 0);
    });
    m_security.Init(ctx.security,*m_renderer,m_watchdog,{m_ctrlToken},true);

    // 射撃フィードバック用メッシュ＆発砲音（PROMPT: 撃てているか分かるように）。
    m_tracerMesh = MakeBoxMesh(*m_renderer, XMFLOAT3{ 0.0f, 0.0f, 0.5f },
                               XMFLOAT3{ 0.5f, 0.5f, 0.5f },
                               XMFLOAT4{ 1.0f, 0.95f, 0.35f, 1.0f }); // 黄の弾道（単位箱）
    m_muzzleMesh = MakeBoxMesh(*m_renderer, XMFLOAT3{ 0.0f, 0.0f, 0.0f },
                               XMFLOAT3{ 0.05f, 0.05f, 0.05f },
                               XMFLOAT4{ 1.0f, 0.85f, 0.4f, 1.0f }); // マズルフラッシュ（小さめ）
    m_hitFlashMesh = MakeBoxMesh(*m_renderer,
                                 XMFLOAT3{ 0.0f, kEnemyHalf.y, 0.0f },
                                 XMFLOAT3{ kEnemyHalf.x * 1.15f, kEnemyHalf.y * 1.05f,
                                           kEnemyHalf.z * 1.15f },
                                 XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f }); // 被弾で白く光る

    // 一人称ビューモデル（銃）。読み込み失敗時は m_viewGun.IsLoaded()==false のままで、
    // Render 側が描画をスキップするだけ（安全側フォールバック）。
    m_viewGun.Load(*m_renderer, "assets/models/external/SM_Ak_47.glb");

    AudioPlayer::Get().PlayBgm(); // BGM 開始
    m_enemies.clear();
    for (const SpawnCfg& s : kEnemySpawns) {
        Enemy e;
        e.spawnPos = s.a;
        e.position = s.a;
        e.patrolA  = s.a;
        e.patrolB  = s.b;
        e.toB      = true;
        e.state    = Enemy::State::Patrol;
        e.health   = kEnemyMaxHealth;
        e.alive    = true;
        // 敵の Swat 化: glTF が読み込めていて GPU スキニングが使える環境でのみ Bind する。
        // 失敗時は e.anim が未 Bind のまま残り、Render 側が従来の箱モデルへフォールバックする。
        if (m_world->HasAnimatedCharacter()) {
            e.anim.Bind(&m_world->GltfCharacterModel(), m_world->GltfCharacterSkinIndex());
            e.anim.Play("Walk");
        }
        m_enemies.push_back(e);
    }
    ResetEncounter(); // 初期配置＋スポーン保護を有効化（即死ループ防止）
    LOG_INFO("敵を %zu 体配置しました（巡回→索敵→追撃 / %d 体撃破でクリア）",
             m_enemies.size(), kClearKills);

    // クリア画面のオーバーレイ（2D）。撃破目標に達したら表示する。
    m_clearOverlay = overlaytext::Build(
        *m_renderer, m_renderer->GetAspect(),
        { "CLEAR !", "", "R = RETRY", "ESC = QUIT" });

    // --- 操作方式: コントローラー主体。カーソルはロックしない（マウスは通常操作のまま）---
    // 別窓で cheat_tester を操作しながらゲームをプレイできるよう、カーソルを奪わない。
    // マウス視点は無効（コントローラー右スティックで視点操作）。移動=左スティック。
    if (m_window != nullptr) {
        m_window->GetInput().SetCursorLock(false, m_window->GetHandle());
    }

    LOG_INFO("FPS シューティング開始【操作＝コントローラー】移動=左スティック  視点=右スティック  "
             "発砲=R2  難読化=L1  監視=F6  深度門番=F7  終了=Options/PS。※マウスカーソルは通常操作（別窓のチート操作用）。");
    LOG_INFO("個別対策は SECURITY ログとHUDを参照。P/L1=難読化、F6=監視、F7=深度門番、M/R1=タグ。");
    return true;
}

void FpsMode::UpdateHud() {
    if (m_window == nullptr) return;
    // 簡易 HUD（ウィンドウタイトル）。HP・撃破数・防御(J-03)・マスク(J-04)状態を表示。
    wchar_t title[200];
    swprintf_s(title, L"FPS  HP %.0f  撃破 %d  %ls %ls",
               (m_player.IsValid() ? m_player->health : 0.0f),
               m_kills,
               m_security.Label(),
               L"");
    SetWindowTextW(m_window->GetHandle(), title);
}

void FpsMode::Update(float dt, const Input& input) {
    // ステージL-04b: glTF モデルのアニメーションを進める（World::Build で配置した展示物）。
    if (m_world != nullptr) {
        m_world->Update(dt);
    }

    // 独立トグル。Start/StopはControls内でロックの外から行う。
    // Start()/Stop() は監視スレッドを join するため AcquireLock() の外で呼ぶ（デッドロック回避）。
    m_security.Update(input);

    // 難読化（防御 ON のときだけ毎フレーム別アドレスへ移動）。監視スレッドと排他する。
    // ※ 難読化は「フラグ書換を空振りさせる」時間稼ぎ。ウォールハックを確実に潰すのは
    //   J-03 の門番（敵ドローの ALWAYS を描画直前に拒絶）。根本解決はサーバー権威(ステージF)。
    if (m_security.State().shuffle) {
        auto lock = m_watchdog.AcquireLock();
        m_ctrlAlloc.ShuffleSlots();
    }

    // 透視チートは base には無い（外部 cheat_tester が担う）。ここでは何もしない。

    // 射撃エフェクトの寿命を減らす（弾道/マズルの表示時間）。
    if (m_tracerTimer > 0.0f) m_tracerTimer -= dt;
    if (m_spawnProtect > 0.0f) m_spawnProtect -= dt;

    // クリア中は「再プレイ(R/△)」だけ受け付け、敵AI/被弾を止める（視点移動は可）。
    const bool retryNow = input.IsKeyDown('R') || input.IsPadDown(GamepadButton::Triangle);
    if (m_cleared) {
        if (retryNow && !m_prevRetry) {
            m_kills = 0;
            m_cleared = false;
            ResetEncounter();
            LOG_INFO("再プレイします。");
        }
        m_prevRetry = retryNow;
        m_player->Update(dt, input, m_world->GetObstacles());
        if (m_security.State().shuffle) m_playerAlloc->ShuffleSlots();
        m_camera.SetPosition(m_player->EyePosition());
        m_camera.SetRotation(m_player->yaw, m_player->pitch);
        UpdateHud();
        return;
    }
    m_prevRetry = retryNow;

    m_player->Update(dt, input, m_world->GetObstacles());

    // アンチチート準備: プレイヤーデータを毎フレーム別スロットへ移動
    if (m_security.State().shuffle) m_playerAlloc->ShuffleSlots();

    // カメラをプレイヤー目線に追従
    m_camera.SetPosition(m_player->EyePosition());
    m_camera.SetRotation(m_player->yaw, m_player->pitch);

    // --- 敵 AI（巡回→索敵→追撃）・遠距離射撃・接触ダメージ・撃破後リスポーン ---
    const std::vector<AABB>& obstacles = m_world->GetObstacles();
    XMFLOAT3 playerPos = m_player->position;
    const XMFLOAT3 playerEye = m_player->EyePosition();

    // 被弾でプレイヤー HP を減らす。スポーン保護中は無効。0 以下で「やり直し」＝
    // プレイヤーを初期位置へ復活させ、敵も配置し直す（リスポーン直後の即死ループ防止）。
    auto damagePlayer = [&](float amount) {
        if (m_spawnProtect > 0.0f) return; // 復活直後の無敵
        m_player->health -= amount;
        if (m_player->health <= 0.0f) {
            LOG_WARN("やられた！初期位置にリスポーンします（HP 回復・敵も再配置・%.1f秒 無敵）",
                     kSpawnProtect);
            ResetEncounter();
            playerPos = m_player->position; // = kPlayerStart（残りの敵判定もこの位置基準に）
        }
    };

    for (Enemy& e : m_enemies) {
        if (e.fireCooldown > 0.0f) e.fireCooldown -= dt;
        if (e.hitFlash > 0.0f)     e.hitFlash -= dt;

        if (!e.alive) {
            e.respawnTimer -= dt;
            if (e.respawnTimer <= 0.0f) {
                e.position = e.spawnPos;
                e.health   = kEnemyMaxHealth;
                e.alive    = true;
                e.state    = Enemy::State::Patrol;
                LOG_INFO("敵がリスポーンしました (%.1f, %.1f)", e.position.x, e.position.z);
            }
            continue;
        }

        const float dxp = playerPos.x - e.position.x;
        const float dzp = playerPos.z - e.position.z;
        const float distP = sqrtf(dxp * dxp + dzp * dzp);
        const XMFLOAT3 eEye{ e.position.x, e.position.y + 1.4f, e.position.z };
        const bool los = HasLineOfSight(eEye, playerEye, obstacles);

        // 状態遷移（巡回⇄追跡）
        if (e.state == Enemy::State::Patrol) {
            if (distP <= kDetectRange && los) {
                e.state = Enemy::State::Chase;
                LOG_INFO("敵がプレイヤーを発見（追跡開始）");
            }
        } else { // Chase
            if (distP > kLoseRange || (!los && distP > kDetectRange)) {
                e.state = Enemy::State::Patrol; // 見失って巡回へ戻る
            }
        }

        // 目標地点（追跡＝プレイヤー / 巡回＝端点 A↔B の往復）
        float tx, tz, speed;
        if (e.state == Enemy::State::Chase) {
            tx = playerPos.x; tz = playerPos.z; speed = kEnemyChaseSpeed;
        } else {
            const XMFLOAT3& wp = e.toB ? e.patrolB : e.patrolA;
            tx = wp.x; tz = wp.z; speed = kEnemyPatrolSpeed;
            const float wdx = tx - e.position.x, wdz = tz - e.position.z;
            if (sqrtf(wdx * wdx + wdz * wdz) <= kWaypointReach) e.toB = !e.toB;
        }

        // 移動（障害物回避）＋向きの更新
        const float mdx = tx - e.position.x, mdz = tz - e.position.z;
        const float mdist = sqrtf(mdx * mdx + mdz * mdz);
        if (mdist > 1e-3f) {
            const float step = speed * dt;
            const float px = e.position.x, pz = e.position.z; // 移動前
            XMFLOAT3 center{ e.position.x, e.position.y + kEnemyHalf.y, e.position.z };
            XMFLOAT3 vel{ mdx / mdist * step, 0.0f, mdz / mdist * step };
            XMFLOAT3 resolved = ResolveAABBCollision(center, kEnemyHalf, vel, obstacles);
            e.position.x = resolved.x;
            e.position.z = resolved.z;
            e.yaw = YawFromDir(mdx, mdz);
            // 実際に動いた距離ぶんだけ歩行位相を進める（止まれば脚も止まる）。
            const float mvx = e.position.x - px, mvz = e.position.z - pz;
            e.walkPhase += sqrtf(mvx * mvx + mvz * mvz) * kWalkRate;
        }
        if (e.state == Enemy::State::Chase && distP > 1e-3f) {
            e.yaw = YawFromDir(dxp, dzp); // 追跡中はプレイヤーへ正対
        }

        // 敵の Swat 化: 巡回=歩く(Walk)、追跡=走る(Run)。同じクリップを指定し続けても
        // AnimationPlayer::Play() は別クリップへの切替時だけ頭出しするので毎フレーム
        // 呼んで安全（未 Bind なら IsValid()==false で Update も Play も何もしない）。
        e.anim.Play(e.state == Enemy::State::Chase ? "Run" : "Walk");
        e.anim.Update(dt);

        // 遠距離射撃（追跡中・射程内・視線あり・クールダウン明け）
        if (e.state == Enemy::State::Chase && los && distP <= kFireRange &&
            e.fireCooldown <= 0.0f) {
            e.fireCooldown = kEnemyFireInterval;
            damagePlayer(kEnemyShotDamage);
        }

        // 接触ダメージ（近接）
        if (distP <= kContactDist) damagePlayer(kContactDps * dt);
    }

    // --- 射撃（左クリック or コントローラー R2 の押下エッジ）---
    const bool fireNow = input.IsKeyDown(VK_LBUTTON) || input.IsPadDown(GamepadButton::R2);
    if (fireNow && !m_prevFire) FireShot();
    m_prevFire = fireNow;

    // クリア判定（撃破数が目標に到達）。
    if (!m_cleared && m_kills >= kClearKills) {
        m_cleared = true;
        LOG_INFO("★CLEAR！ %d 体撃破！ R（または △）で再プレイ / ESC で終了。", m_kills);
    }

    UpdateHud();

    if (m_net != nullptr) {
        // 座標をサーバーへ送信
        m_net->SendPlayerState(m_player->playerId, m_player->position,
                               m_player->yaw, m_player->pitch);

        // 受信パケットを取り込み（他プレイヤー座標を保持。描画は将来対応）
        while (auto pkt = m_net->GetNextPacket()) {
            if (pkt->type == PacketType::PlayerState &&
                pkt->bytes.size() >= sizeof(PacketHeader) + sizeof(PlayerStatePayload)) {
                PlayerStatePayload p;
                memcpy(&p, pkt->bytes.data() + sizeof(PacketHeader), sizeof(p));
                if (p.playerId != m_player->playerId) {
                    m_remotePlayers[p.playerId] = p;
                }
            }
        }
    }
}

void FpsMode::FireShot() {
    // カメラ（プレイヤー目線）からの前方レイで最近接の敵に命中判定（ヒットスキャン）。
    const XMFLOAT3 eye = m_player->EyePosition();
    XMFLOAT3 dir;
    XMStoreFloat3(&dir, m_camera.GetForward());

    // 射撃フィードバック: 弾道/マズルは画面に見えているビューモデルの銃口位置から出す
    // （命中判定のレイ自体はカメラ基準のまま＝クロスヘアでのエイムは変えない）。
    m_tracerStart = ViewModelMuzzlePos(m_camera);
    m_tracerDir   = dir;
    m_tracerTimer = 0.06f;
    AudioPlayer::Get().PlayGunshot();

    // 弾は障害物（壁）を貫通しない。まずレイが最初に当たる壁までの距離を求める。
    float wallT = kShotRange;
    for (const AABB& o : m_world->GetObstacles()) {
        float t = 0.0f;
        if (RayIntersectAABB(eye, dir, o, t) && t >= 0.0f && t < wallT) {
            wallT = t;
        }
    }

    int   best  = -1;
    float bestT = kShotRange;
    for (size_t i = 0; i < m_enemies.size(); ++i) {
        if (!m_enemies[i].alive) continue;
        float t = 0.0f;
        if (RayIntersectAABB(eye, dir, EnemyAABB(m_enemies[i]), t) &&
            t >= 0.0f && t < bestT) {
            bestT = t;
            best  = static_cast<int>(i);
        }
    }

    // 壁が敵より手前なら、弾は壁で止まる＝敵には当たらない（貫通しない）。
    if (best < 0 || wallT < bestT) {
        // 命中なし。弾道は「壁の着弾点まで」（壁が無ければ 40m ぶん）を表示する。
        m_tracerLen = (wallT < kShotRange) ? wallT : 40.0f;
        LOG_INFO("発砲（命中なし%s）", (wallT < bestT && best >= 0) ? "・障害物に遮られた" : "");
        return;
    }

    m_tracerLen = bestT; // 弾道は着弾点まで
    Enemy& e = m_enemies[best];
    e.health  -= kShotDamage;
    e.hitFlash = 0.12f;  // 被弾を白フラッシュで表示
    AudioPlayer::Get().PlayHit();
    if (e.health <= 0.0f) {
        e.alive        = false;
        e.respawnTimer = kEnemyRespawn;
        ++m_kills;
        LOG_INFO("敵を撃破！（%.1fm 先 / 撃破数 %d / %.1f 秒後にリスポーン）",
                 bestT, m_kills, kEnemyRespawn);
    } else {
        LOG_INFO("命中 %.1fm（敵 残りHP %.0f / ダメージ %.0f）",
                 bestT, e.health, kShotDamage);
    }
}

void FpsMode::Render(Renderer& renderer) {
    renderer.BeginFrame(m_camera);
    m_world->Render(renderer);

    // 生存している敵を描画（足元原点のガードメッシュを yaw 回転して配置）。
    // ステージJ-02: 敵は専用入口 DrawEnemyTagged 経由（Enemy タグ）で描くだけ。ゲームは
    // 透視の有無を一切判断しない（wallhack フラグを読まない）。描画インターセプトの継ぎ目に
    // 差し込まれたフック(cheat/HookCheat)が、外部 cheat_tester の発動フラグに従って敵ドローを
    // 深度 ALWAYS にすり替える。フック未設置なら常に通常の深度(LESS)＝壁で隠れる。
    for (const Enemy& e : m_enemies) {
        if (!e.alive) continue;
        if (m_world->HasAnimatedCharacter()) {
            // Swat 化: DrawEnemyTaggedSkinned 経由（World::DrawGltfCharacterTagged 内）で
            // 描くので、ウォールハック実演・防御・段[5]検知は箱モデルと同様にそのまま効く。
            m_world->DrawGltfCharacterTagged(renderer, e.position, e.yaw, e.anim);
            continue;
        }
        // フォールバック: glTF 未読込／PBR・スキニング非対応環境では従来の箱組み立てモデル。
        const XMMATRIX world = XMMatrixRotationY(e.yaw) *
            XMMatrixTranslation(e.position.x, e.position.y, e.position.z);
        renderer.DrawEnemyTagged(m_guardBodyMesh, world);
        // 脚を股関節・腕を肩で前後に振る（腕は脚と逆位相＝自然な歩行）。
        const float legSw = sinf(e.walkPhase) * kLegSwing;
        const float armSw = sinf(e.walkPhase) * kArmSwing;
        renderer.DrawEnemyTagged(m_guardLegMesh,
            XMMatrixRotationX(legSw)  * XMMatrixTranslation(-kHipX, kHipY, 0.0f) * world);
        renderer.DrawEnemyTagged(m_guardLegMesh,
            XMMatrixRotationX(-legSw) * XMMatrixTranslation( kHipX, kHipY, 0.0f) * world);
        renderer.DrawEnemyTagged(m_guardArmMesh,
            XMMatrixRotationX(-armSw) * XMMatrixTranslation(-kShoulderX, kShoulderY, 0.0f) * world);
        renderer.DrawEnemyTagged(m_guardArmMesh,
            XMMatrixRotationX(armSw)  * XMMatrixTranslation( kShoulderX, kShoulderY, 0.0f) * world);
    }

    // 被弾フラッシュ（命中直後に敵を白く光らせる＝被ダメージが分かる）。
    for (const Enemy& e : m_enemies) {
        if (!e.alive || e.hitFlash <= 0.0f) continue;
        renderer.DrawMesh(m_hitFlashMesh,
            XMMatrixTranslation(e.position.x, e.position.y, e.position.z));
    }

    // 弾道（黄の線）＋マズルフラッシュ（発砲直後の短時間だけ）。
    if (m_tracerTimer > 0.0f) {
        renderer.DrawMesh(m_tracerMesh,
            AimMatrix(m_tracerStart, m_tracerDir, m_tracerLen, 0.03f));
        renderer.DrawMesh(m_muzzleMesh,
            XMMatrixTranslation(m_tracerStart.x, m_tracerStart.y, m_tracerStart.z),
            /*ignoreDepth=*/true);
    }

    // 一人称ビューモデル（銃）。カメラ相対に配置し ignoreDepth で常に手前に描く
    // （壁に近づいてもめり込んで消えない）。読み込み失敗時は何も描かれないだけ。
    m_viewGun.Draw(renderer, ViewModelTransform(m_camera), /*ignoreDepth=*/true);

    // [旧チート・無効化] 協調型 ESP（ゲームが敵座標を DLL に手渡し、DLL の espOn 指示で
    //   深度無視の緑箱を描く方式）は撤去。ゲームはチートに情報を渡さない。ウォールハックは
    //   上の DrawEnemyMesh(wallhack) が depthCheatFlag（外部から書換）に従って行う。
    //
    //   参考（旧コード）:
    //     if (m_cheat != nullptr) { cheat::HostState st{}; ...; m_cheat->Tick(st, cmd);
    //         if (cmd.espOn) { ...DrawMesh(m_enemyEspMesh, ..., /*ignoreDepth=*/true); } }

    // 段[5] ステート整合性検証（アンチチート検知器）。敵ドローで実際に使われた深度比較が
    // ALWAYS に改ざんされていないかを照合し、検知したらログを出す。
    DetectDepthTamper(renderer);

    // クリア中はクリア画面、そうでなければ照準を最前面に描く。
    if (m_cleared) renderer.DrawOverlay(m_clearOverlay);
    else           renderer.DrawOverlay(m_crosshair);
    renderer.EndFrame();
}

// 段[5] 検知器: 直近の敵ドローでバインドされた深度比較関数を照合する。
// 正規の深度ステート（DepthFunc=LESS / LESS_EQUAL）から ALWAYS へすり替えられて
// いたら、タイプA ウォールハックが成立している＝深度制御帯が改ざんされたとみなす。
//
// 注: 本エンジンは自プロセス内で「実際にバインドした深度関数」を読み戻して照合する。
//   現実のクロスプロセス型アンチチートは、コマンドリスト/PSO 記述子を独立に検査して
//   同じ観測（敵ドローで DepthFunc が LESS 系の外）を得る（typeA doc の段[5]検知）。
void FpsMode::DetectDepthTamper(Renderer& renderer) {
    // 「フックが要求した深度」(req) と「実際に使われた深度」(used) を比べる。
    //   used==ALWAYS            → ウォールハック成立（防御 OFF）
    //   req==ALWAYS && used!=ALWAYS → 門番(J-03)が拒絶＝ウォールハック不成立（防御 ON）
    const D3D12_COMPARISON_FUNC req  = renderer.GetRequestedEnemyDepthFunc();
    const D3D12_COMPARISON_FUNC used = renderer.GetLastEnemyDepthFunc();
    const bool usedAlways = (used == D3D12_COMPARISON_FUNC_ALWAYS);
    const bool reqAlways  = (req  == D3D12_COMPARISON_FUNC_ALWAYS);

    const int state = usedAlways ? 1 : (reqAlways ? 2 : 0);
    if (state != m_wallhackState) {
        if (state == 1) {
            LOG_WARN("[アンチチート/段5] 深度ステート改ざんを検知：敵ドローが ALWAYS で描かれた"
                     "（＝タイプA ウォールハック成立）。P で防御 ON にすると拒絶されます。");
        } else if (state == 2) {
            LOG_INFO("[防御/段5-事前(J-03)] フックが深度 ALWAYS を要求→門番が拒絶して LESS を"
                     "再適用（＝ウォールハック不成立）。");
        } else if (m_wallhackState != 0) {
            LOG_INFO("[アンチチート/段5] 深度ステートが正常（LESS）に戻りました。");
        }
    }
    m_wallhackState = state;
}

void FpsMode::Shutdown() {
    // フックを継ぎ目から外す（provider が捕捉する this/Handle が壊れる前に）。
    if (m_renderer != nullptr) hookcheat::Uninstall(*m_renderer);
    // 監視スレッドを止めてから（join）アロケータ/ハンドルを壊す（寿命規約）。
    m_watchdog.Stop();
    // LinearAllocator 上のオブジェクトは明示的に破棄する
    // （アリーナ自体の解放は m_gameMemory のデストラクタに任せる）
    if (m_world != nullptr) {
        m_world->~World();
        m_world = nullptr;
    }
    // m_player / m_playerAlloc / m_gameMemory は unique_ptr/値メンバなので自動解放。
}
