// ===========================================================================
// HookCheat 実装（ステージJ-02）。詳細は HookCheat.h のヘッダコメント参照。
//
// SECURITY NOTE: 自作 GameEngine.exe 専用の検証用 testbed。注入(段[1])・汎用 VMT
// フック(段[4])の武器化コードは実装しない。ここは「継ぎ目に差し込んだフックが敵ドローを
// 深度 ALWAYS にすり替える」段[5]の振る舞いだけを first-party に再現する。
// ===========================================================================
#include "cheat/HookCheat.h"

#include "engine/renderer/Renderer.h"

namespace {

// 「フックが発動中か」を返すコールバック（外部 cheat_tester が書く depthCheatFlag の
// 現在値を反映する）。Install で受け取り、Interceptor が毎ドロー参照する。
std::function<bool()> g_activeProvider;

// 描画インターセプトの継ぎ目に差し込まれる本体（＝注入コードの代役）。
// Renderer::DrawEnemyTagged が敵ドローごとにこれを呼ぶ。ここで:
//   ① 敵ドロー判定（継ぎ目が渡す tagToken を素の敵値 kEnemyTagPlain と単純比較する）
//   ② 発動中かつ敵なら、その一手だけ深度 ALWAYS ですり替える（＝壁越し）
//   ③ そうでなければ通常の深度(LESS)で描く（次ドローに影響を残さない＝即復元に相当）
// シグネチャは Renderer::DrawInterceptor と一致させること。
//
// J-04（防御②B）: マスク ON だと tagToken はフレーム鍵で XOR されるため、この単純比較は
// 一致しなくなる＝フックは「どのドローが敵か」を特定できず ALWAYS を要求できない（透視できない）。
// エンジンは鍵を知っているので SubmitTaggedDraw 内でトークンを正しく復号し描画/防御を続ける。
//
// 「敵キャラの Swat 化」で material/texture/pbr/skinPaletteAddr が増えたが、このフックは
// 材質を判断材料にしない（深度モードの決定は敵タグの有無だけで決まる）。受け取った値を
// そのまま SubmitTaggedDraw へ転送するだけで、Lit の箱キャラでもスキン付き Swat でも
// 同じロジックで壁越し描画が成立する＝敵の見た目を差し替えてもフックの実装は変更不要。
void Interceptor(Renderer& r, const Mesh& mesh, const DirectX::XMMATRIX& world,
                 uint32_t tagToken, MaterialType material, const Texture* texture,
                 const PbrMaterial* pbr, D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr,
                 void* /*ctx*/) {
    const bool active  = static_cast<bool>(g_activeProvider) && g_activeProvider();
    const bool isEnemy = (tagToken == Renderer::kEnemyTagPlain); // 素朴な識別（マスクで妨害される）
    const DepthMode mode =
        (active && isEnemy) ? DepthMode::Always : DepthMode::Normal;
    r.SubmitTaggedDraw(mesh, world, tagToken, mode, material, texture, pbr, skinPaletteAddr);
}

} // namespace

void hookcheat::Install(Renderer& renderer, std::function<bool()> activeProvider) {
    g_activeProvider = std::move(activeProvider);
    renderer.SetDrawInterceptor(&Interceptor, nullptr);
}

void hookcheat::Uninstall(Renderer& renderer) {
    renderer.SetDrawInterceptor(nullptr, nullptr);
    g_activeProvider = nullptr;
}
