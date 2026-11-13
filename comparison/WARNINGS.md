# 射撃場のクラッシュ・警告の調査

2026-10-04追記：IL2CPP保護PlayerとMono対照Playerを同じD3D11で作り、反復16/16回と4回の入力再生で正常終了を確認した。
この追加試験でD3D12の終了アクセス違反を修理・再検証したわけではない。D3D11を使う運用を維持する。

IL2CPP Playerのログには、`IDXGISwapChain::GetFrameStatistics`から適切な時刻が得られず、CPU側の時刻計算へ切り替える診断が残った。
例：`output/release-validation/unity-protection-player/il2cpp-victory.log`。今回の状態13582ステップ一致と正常終了はこの診断があっても成立した。
DXGIの時刻取得に関する原因の特定や修理はしていない。更新CSVは独立にQPCで取ったCPU更新間隔で、GPU表示時刻を計測したものではない。
この診断と以下のD3D12終了例外を同一原因とする証拠もない。追加比較結果は [配布保護](../docs/reference/RELEASE_PROTECTION.md)を参照。

2026-10-03。ユーザー提供画像は警告ではなく、RangeUnity.exeの読み取りアクセス違反。
従来「終了待ちタイムアウト」と記録した失敗について、Windowsの記録と停止中のスタックから原因箇所を特定した。

## 画像のエラー：D3D12デバイス破棄中のアクセス違反

- 画像の命令アドレス `0x00007FFDD19EA1F5`、参照先 `0x00000214629D52F8` は、
  Systemイベント26（2026-10-03 14:12:31）の記録と完全一致。13:57:48にも同じ命令で発生した。
- 両時刻は `unity-fallback-verified/000_unity_d3d12`、`range-final-smoke/001_unity_range` の
  `sample_end`・終了ログの直後。単に計測が長引いていたのではない。
- 同一セッションで採取したモジュール一覧から、命令は配布版 `D3D12/D3D12Core.dll` の
  `+0xA1F5`。同梱ランタイムのバージョンは `1.618.1.0.20250923.2`。
- 再現時に、その診断ツール自身が起動した子プロセスに接続して全スレッドのスタックとミニダンプを保存。
  障害スレッドは同じ `D3D12Core.dll+0xA1F5` から例外処理へ入り、
  `UnhandledExceptionFilter → WerpLaunchAeDebug → WaitForMultipleObjects` で待機していた。
- インストール済みUnityの一致するPDBで呼出元を解決すると、
  `Shutdown → DestroyGfxDevice → GfxDeviceD3D12::~GfxDeviceD3D12 → D3D12Core`。
  **UnityのD3D12描画デバイスを破棄する終了処理で発生したアクセス違反**と確認できた。

保存先は `output/range-validation/crash/` の `events.json`、`modules.json`、
`d3d12-visible-late-02.txt` / `.txt.dmp`、`unity-symbols.json`。
PDBのないDLLの大きなexport相対オフセットは関数名の特定根拠にしない。
ダンプは自分の診断用Playerだけを対象にローカル保存し、外部へ送信していない。

このPCはIntel Iris Xe、Unity 6000.3.7f1、Intelドライバー32.0.101.6737。
Unityが通常はD3D11を選ぶ環境で、比較用に `-force-d3d12` を指定した経路に再現する。
破棄されたオブジェクトの所有権・無効になった理由まではUnity/DirectX内部の調査が必要で、
ドライバーだけの不具合や、RAM不足・物理メモリ故障と断定する証拠はない。
ゲームルールのC#例外やInfo Queue警告とは区別する。

### 対応と確認

`RangeBuild.Build` の通常起動API優先順を **D3D11 → D3D12** に変更し、Playerを再ビルド。
通常プレイの手順からD3D12強制を外した。D3D12内部の不具合を修理したとは扱わず、発生経路を避ける対応。
D3D12比較ケースには `known_issue` を記録し、ランチャーは既定で実行を止める。
原因調査を続ける場合だけ `--allow-unstable` で実行できる。D3D11へ黙って切り替えて同条件比較とすることはない。

再ビルド前の切り分け試験（`output/benchmarks/crash-stability`、各5回）：

| 条件 | 正常終了 | 終了失敗 |
|---|---:|---:|
| D3D11指定 | 5 | 0 |
| D3D12指定 | 2 | 3 |
| D3D12＋単一描画スレッド | 4 | 1 |

`-force-gfx-direct` でも再発したため、単一スレッド化を解決策に採用していない。
非表示の診断起動はデバッガー付き7回・後から接続する方式4回で再発しなかったが、
通常表示へ揃えると2回目で停止を採取できた。起動条件で再現性が変わるので、非表示実行の成功を修正の証明にしない。
[UnityのPlayer起動引数](https://docs.unity3d.com/6000.3/Documentation/Manual/PlayerCommandLineArguments.html)

再ビルド後は、描画APIの強制引数なしの通常起動5/5回でD3D11の選択・計測CSV保存・終了コード0を確認。
確認先は `output/benchmarks/unity-default-fixed`。
短時間の起動・終了確認であり、長時間プレイや同一描画APIでの性能比較の結果ではない。

## GPU切り替えと描画初期化失敗

このPCはIntel Iris Xe、ドライバー32.0.101.6737。Unity Playerは
`D3D12 API denied by user filter` と `Vendor: Intel / Device Type: Integrated` を記録する。
プロジェクトの `d3D12DeviceFilterListAsset` は未指定で、ユーザーが作ったフィルターではない。
メッセージ中のuserという表現だけで、開発者が設定したものと判断しない。
プロジェクト設定の診断は `output/range-validation/unity-device-filters.json`、実際の判定はPlayerログを参照する。

初版はビルド対象の描画APIをD3D12だけにした。この状態でD3D11を要求すると
`Forced GfxDevice 'Direct3D 11' was not built from editor, shaders will not be available` に続いて
`InitializeEngineGraphics failed` を記録し、計測開始前に停止した。3回の再現ログを
`output/benchmarks/unity-warning-repro` に保存。D3D12側は同じ診断試行の3回とも成功した。
通常起動がD3D11へ戻る環境にも、同じシェーダー欠落の問題がある。

**修正:** `RangeBuild.Build` のAPI一覧をD3D12・D3D11に変更し、フォールバック側のシェーダーも生成する。
ビルド成功ログは `output/range-validation/unity-build-fallback-verified.log`。
GPU選択ルールを消して警告を隠す変更はしていない。

修正後の通常起動（実際はD3D11）3/3回、D3D11指定3/3回で、描画初期化・CSV保存・プロセス終了が成功した。
D3D12指定は2/3回成功、1回は計測後の終了待ちが20秒を超えた。ログは
`output/benchmarks/unity-fallback-verified`。これによりシェーダー欠落の修正と、終了待ちの別問題を区別できた。

診断用に残した `cases.range.json` では、公式の `-force-d3d12` 引数を使う（既定では実行停止）。
さらにゲーム側で実際のAPI・1600×900・Gammaを検査し、不一致は `scene_ready` より前に終了コード2で失敗させる。
普通に遊ぶ場合はD3D11を優先する。D3D11で動いた結果をD3D12の性能結果として扱わない。
[UnityのPlayer起動引数](https://docs.unity3d.com/6000.0/Documentation/Manual/PlayerCommandLineArguments.html)

## Info Queueの取得失敗

`d3d12: failed to query info queue interface (0x80004002)` は、UnityのD3D12初期化経路が
診断メッセージ用インターフェースを取得できなかったという記録。
Info Queueはデバッグレイヤーが有効でなければ取得できない仕様なので、非Development版での出力と整合する。
Unity内部の該当C++コードは確認できていないため、内部の呼出し条件まで断定しない。
[MicrosoftのID3D12InfoQueue仕様](https://learn.microsoft.com/en-us/windows/win32/api/d3d12sdklayers/nn-d3d12sdklayers-id3d12infoqueue)

同じ警告を出した実行でも、`RANGE SETTINGS: api=Direct3D12` と準備完了、計測終了、正常な終了コードを確認した。
この1行をゲーム描画の失敗とは扱わない。警告を消す目的で性能測定へデバッグレイヤーを追加することもしない。

## 計測後の終了タイムアウト

`output/benchmarks/range-final-smoke/001_unity_range` は、準備・3.016秒分の計測・`sample_end` を含むCSV保存と、
Input Systemの終了ログまで進んだ一方、プロセス終了待ちが60秒を超えた。
これは上のシェーダー欠落のような「描画初期化前の失敗」と異なる。
同じInfo Queue警告の出る他の試行は成功しているため、その警告をタイムアウト原因と断定できない。

フォールバック修正後にもD3D12指定で1件再発（`unity-fallback-verified/000_unity_d3d12`）。
こちらもCSVに1.016秒の計測とsample_endを記録済みで、`after_sample_end` に分類された。
当初はスタックを採取できていなかったが、画像提供後の追加調査で上記のD3D12デバイス破棄中の例外を確認した。
成功するまで試行を捨てて性能を報告しない。

ランチャーを修正して、CSVが完了した後のタイムアウトを `timeout_stage=after_sample_end`、
完全な記録を得る前を `before_complete_trace` として区別するようにした。
どちらも失敗試行のまま保持する。既存の失敗記録も削除・成功扱いへの変更はしていない。
ランチャーは自身の子プロセスに限ってエラーダイアログを抑制するエラーモードを渡し、Popen直後に元へ戻す。
終了コードも16進で保存する。ただし今回のAeDebug待機はこれでもタイムアウトになったため、
この設定だけでクラッシュを解消したり、全クラッシュを終了コードで捕捉できるとは扱わない。
システムのレジストリ・WER設定は変更しない。既知の不安定ケースの拒否と設定復元も含め、計測の16テストが成功した。
[Microsoftの子プロセスのエラーモード解説](https://devblogs.microsoft.com/oldnewthing/20160204-00/?p=92972)

## Unityライセンスと画面確認

バッチビルド冒頭の `Access token is unavailable; failed to update` の後には、ライセンス情報の取得・更新成功と
Playerビルド成功が続いている。今回のビルドを停止させたエラーではない。認証設定は変更していない。

Computer Useの `Computer Use app approval timed out` は画面確認のためのアプリ承認待ちが期限切れになったもの。
ゲームのビルドエラーとは別で、画面・操作感を確認済みとはしていない。

## 計測結果の扱い

`range-smoke` はD3D11への自動切替を発見した段階、`range-d3d12-smoke` はロゴ表示のある段階。
`range-final-smoke` はロゴなしだが上記の終了タイムアウト1件を含む。
いずれも短い基盤確認で、性能優位の証明に使用しない。
フォールバック修正後の確認先は `output/benchmarks/unity-fallback-verified`、画像提供後の調査先は本書冒頭を参照。
見た目・実際の入力遅延・長時間安定性は別の確認が必要。
