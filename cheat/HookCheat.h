#pragma once

#include <functional>

// ===========================================================================
// HookCheat — 描画フック型チートの「フック本体」（ステージJ-02）。
//
// SECURITY NOTE: これは自作 GameEngine.exe の検証専用の testbed である。現実の
// タイプA ウォールハックは 注入(段[1])→VMTフック(段[4]) で外部コードをプロセスに
// 送り込むが、その注入・汎用フックの武器化コードは typeA_wallhack_explained.md の
// 線引きどおり実装しない。ここでは「注入されたコードが描画をフックして敵ドローを
// 深度 ALWAYS にすり替える」振る舞いだけを、エンジンが用意した描画インターセプトの
// 継ぎ目(Renderer::SetDrawInterceptor)に first-party で差し込んで再現する。
// この継ぎ目とタグは本エンジン固有なので、他ゲームには一切通用しない（非汎用）。
//
// 役割:
//   Install() で継ぎ目に自分(Interceptor)を差し込む。継ぎ目は敵ドローごとに
//   Interceptor を呼ぶ。Interceptor は「敵タグかどうか」を判定し、発動中なら
//   その一手だけ深度 ALWAYS ですり替えて描く（＝ウォールハック）。
//   発動可否は activeProvider（外部 cheat_tester が書く depthCheatFlag の現在値を
//   毎フレーム返すコールバック）に従う。ゲームのゲームロジックは透視を一切知らない。
// ===========================================================================
class Renderer;

namespace hookcheat {

// 描画インターセプトの継ぎ目にフックを設置する。
//   activeProvider: 「チートが発動中か」を返す（外部フラグの現在値を反映）。
void Install(Renderer& renderer, std::function<bool()> activeProvider);

// フックを取り外して継ぎ目を既定（通常描画）へ戻す。
void Uninstall(Renderer& renderer);

} // namespace hookcheat
