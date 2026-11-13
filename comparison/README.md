> 整理後の入口は [現状まとめ](../CURRENT_IMPLEMENTATION.txt) と [移送手順](../MIGRATION.txt)。再ビルド例は新しい出力先を使います。

# 共通射撃場の実行と検証

次回イベントの比較用に、同じ「60秒で10個の的を撃つゲーム」をC++とUnityに実装した。
既存のFPSやUnity教材とは別の、小さな共通ゲームである。まずルールと入力列を揃え、
起動から準備完了までと更新間隔を比較できる状態を作る。自作版の基本画面とキーボード操作を確認済み。
両版の見た目の比較と人・実パッドによる操作感の評価はまだ完了していない。

## 遊ぶ

ビルド済みの場所は `output/packages/range-native/RangeGame.exe` と
`output/packages/range-unity-mono-control/RangeUnity.exe`。作業ディレクトリをリポジトリのルートにして実行する。

```powershell
& ./output/packages/range-native/RangeGame.exe
& ./output/packages/range-unity-mono-control/RangeUnity.exe
```

2行は別々に実行する。Unityの通常起動はD3D11を優先する。
このPCでD3D12を強制すると終了時にアクセス違反が再現したため、通常プレイでは強制しない。
自作版はD3D12なので、この起動方法の結果を同一描画APIの速度比較には使わない。詳細は [WARNINGS.md](WARNINGS.md)。

| 操作 | キーボードとマウス | コントローラー |
|---|---|---|
| 移動 | WASD | 左スティック |
| 視点 | Tabでマウスを捕捉して移動 | 右スティック |
| 発砲 | 捕捉中に左クリック | R2 |
| やり直し | R | △または北ボタン |
| 終了 | Esc | OptionsまたはStart |

上の10個の印が撃破数、右上の棒が残り時間。命中すると照準が緑になり、的が消える。
全的を撃つとCLEAR、60秒経過でTIME UP。音声・ジャンプ・オンライン対戦・チート対策はこの初版に含まない。
両版で同じパッドが同じ感度で使えることは、実機操作の確認項目として残っている。

## 共通仕様

| 項目 | v1の仕様 |
|---|---|
| マップ | 20m四方、自作の箱・壁・10的。座標・寸法・色は `data/range.json` |
| 時間 | 120Hzの固定更新、7,200ステップで60秒 |
| 移動 | 5m/s、斜め移動を正規化。X→Zの順に壁との衝突を判定 |
| 射撃 | 視線レイ、近い的を優先。同距離なら壁を優先。24ステップの連射間隔 |
| カメラ | 垂直FOV70度、near 0.1m、far 500m、目線高1.6m |
| 描画 | 1600×900、VSync ON、MSAA・影・フォグ・後処理なし。面ごとの頂点色 |
| HUD | 共通の配置と5×7フォント。Unityの字形はC++のOverlayText.cppから生成 |

固定更新は「描画が何fpsでも、ゲームの計算は同じ時間刻みで進める」仕組み。
両版は同じ入力列を同じ順番で計算する。描画スレッドやGPUへの命令の出し方まで同じという意味ではない。
Unityは独立したC#移植で、Unity Physicsにゲームルールを任せていない。

## ビルド

Windows、VS2022のC++環境、Windows SDK、CMake、Python、Unity 6000.3.7f1を使用する。
`python` と `cmake` がPATHに無い場合は、その実行ファイルの絶対パスを指定する。

```powershell
python comparison/make_data.py
python comparison/prepare_unity.py
cmake -S . -B build-range -G "Visual Studio 17 2022" -A x64 -DGAME_PROFILE=range -DHARDENED=OFF
cmake --build build-range --config Release --target game range_trace
cmake --install build-range --config Release --prefix output/packages/range-native
```

Unityは `comparison/unity` を開いてビルドするか、以下でバッチビルドする。
既存のUnity教材プロジェクトは変更しない。Input System 1.18.0が必要。

```powershell
$rangeUnity = 'C:/Program Files/Unity/Hub/Editor/6000.3.7f1/Editor/Unity.exe'
$rangeRoot = (Get-Location).Path
$rangeBuild = Start-Process -FilePath $rangeUnity -WindowStyle Hidden -PassThru -Wait -ArgumentList @(
    '-batchmode', '-nographics', '-quit',
    '-projectPath', ('"' + $rangeRoot + '/comparison/unity"'),
    '-executeMethod', 'RangeComparison.Editor.RangeBuild.Build',
    '-rangeBuildOutput', ('"' + $rangeRoot + '/output/packages/range-unity-mono-new/RangeUnity.exe"'),
    '-logFile', ('"' + $rangeRoot + '/output/range-validation/unity-build.log"'))
if ($rangeBuild.ExitCode -ne 0) { throw 'Unity build failed; inspect log' }
```

`RangeBuild.Build` は空のシーンにRangeGameを配置し、Windows x64 / Mono / 非Development / Gamma / ロゴなしでビルドする。
D3D11を既定としてD3D12も含める。`stripEngineCode` のネイティブコード削減はIL2CPP向けで、
このMono版をUnityの最小サイズ構成とは呼ばない。容量や起動の結果を一般的なエンジン優劣へ拡張しない。

## 状態を照合する

`RangeBuild.Build` はC#側の3シナリオの状態CSVも出力する。C++側を実行して全ステップを照合する。

```powershell
New-Item -ItemType Directory -Force output/range-validation | Out-Null
foreach ($scenario in @('validation','victory','tour')) {
    & ./build-range/Release/range_trace.exe comparison/data/range.json "comparison/data/$scenario.json" "output/range-validation/native_$scenario.csv"
    if ($LASTEXITCODE -ne 0) { throw 'Native simulation failed' }
}
python comparison/compare_traces.py --scenarios output/range-validation --out output/range-validation/parity.json
python -m unittest discover -s comparison -p test_range.py -v
python -m unittest discover -s tools/benchmark -v
```

位置・角度は絶対誤差1e-8以下、ステップ番号・命中・発砲・的の残り・終了状態は完全一致を要求する。
一致だけでなく、壁越し射撃が0命中、連射数、壁で停止、再開、10発でクリア、60秒で時間切れという期待値も確認する。
実際のPlayerにも `--replay PATH --state-out PATH --exit-after-replay` を渡して同じCSVを作れる。
入力列の末尾では状態を保持し、`--exit-after-replay` を付けた場合だけ自動終了する。

## 起動と更新間隔を測る

**現在、このPCのUnity D3D12比較は終了クラッシュが未解決のため保留。**
`cases.range.json` は同じD3D12を要求する仕様を保持し、`known_issue` により既定では実行しない。
以下は原因調査を続ける場合の短時間コマンド。失敗を含む診断結果であり、性能評価には使用しない。

```powershell
python tools/benchmark/run.py comparison/cases.range.json --allow-unstable --output output/benchmarks/range-01 --runs 3 --warmup-runs 0 --seconds 2 --warmup-seconds 0.25 --timeout 15
python tools/benchmark/plot.py output/benchmarks/range-01/summary.json
```

計測中は他のゲームやビルドを同時実行しない。起動順はラウンドごとに入れ替える。
GPU・ドライバー・リフレッシュレート・電源モードをケースの条件へ記録し、両方のGPU名をログで確認する。
最初の予熱起動を集計から除外するが、OSキャッシュは制御していないためコールド起動とは呼ばない。

Unityはシーン構築が終わった場所から `EventBenchmark.MarkSceneReady()` を呼ぶ。
CSVの値は更新入口までとCPU側のループ間隔であり、画面到達・入力遅延・GPU時間ではない。
CSVが完成していても終了がタイムアウトした試行は失敗のまま残す。
`timeout_stage=after_sample_end` は計測後の終了待ち、`before_complete_trace` は完全な記録が得られる前の停止を表す。

## 自作エンジンの構成選択

| GAME_PROFILE | ゲーム本体 | 初期化と配布 |
|---|---|---|
| all | 既存fps/card/hide＋range | 既存の全素材。range選択時は簡素な描画経路のみ初期化 |
| fps | 既存FPSのみ | CardMode/HideSeekMode/RangeModeを除外。FPSが読む7外部GLBと2フォールバックだけ同梱 |
| range | 共通射撃場のみ | 音声・通信・暗号・監視・モデルローダーをリンク対象から除外。4シェーダー＋共通JSON |

```powershell
cmake -S . -B build-fps -G "Visual Studio 17 2022" -A x64 -DGAME_PROFILE=fps -DHARDENED=OFF
cmake --build build-fps --config Release --target game cheat_tester
cmake --install build-fps --config Release --prefix output/packages/fps-native
```

FPS版の音声・UDP通信・PBR・影・スキン・既存防御は維持する。独立した全機能選択にはまだなっていない。
FPS素材には外部ライセンスのモデルがあるため、この出力はローカル検証用。射撃場の素材は自作。
`range + HARDENED=ON` は対策未実装なのに対策版と表示しないよう構成時に拒絶する。
用途ごとに別のビルド先・配布先を使う。異なるプロファイルの出力を同じフォルダーへ重ねない。

## 確認状況

2026-10-03にReleaseの3プロファイルとUnity Playerをビルド。3シナリオ13,582ステップのC++/C#状態が一致し、
Player間でもクリアまでの1,114ステップが一致した。射撃場6テストと計測16テストがある。
UnityのD3D12終了タイムアウトは、スタックからデバイス破棄中のアクセス違反と確認した。
D3D11を優先する版へ再ビルドし、通常起動5/5回でCSV保存・終了コード0を確認した。D3D12内部の根本修正は未完了。
警告調査の詳細は `WARNINGS.md`、実測ログは `output/range-validation` と `output/benchmarks` に残す。

2026-10-04はComputer Useで自作射撃場の3D画面、Rのリトライ、Wの移動、ESC終了を確認した。
短いキー押下を更新間で見落とすInputの不具合を修正した。実パッドと人によるA/B評価は未実施。
手順と記録項目は [PLAYTEST.md](PLAYTEST.md)。対策の独立選択とカード権威サーバー/TLSは実装済み。

配布保護版 `output/packages/range-protected-final` を追加し、通常版と3シナリオ13,582ステップ、
最終Playerの1,114ステップが一致した。比較の条件は `cases.protection.json`。
これは自作同士の保護有無比較で、UnityとのD3D12比較の保留は継続する。

```powershell
python tools/benchmark/run.py comparison/cases.protection.json --output output/benchmarks/protection-repeat-next --runs 7 --warmup-runs 1 --seconds 5 --warmup-seconds 2 --timeout 25
```

今回の結果は [VALIDATION_20261004.md](../docs/reference/VALIDATION_20261004.md)。イベントPCでの長時間測定と別PC評価は残る。

同日の追加保護版は`output/packages/range-signed-v2-final`。署名付きファイル一覧を起動前に検証するため、Windows CNGにも依存する。
従来Playerとの1114ステップ一致と、署名検証あり/なし各7回＋予熱各1回の正常終了を確認した。比較条件は`cases.signed.json`。
署名検証・起動コストの数字は [RELEASE_PROTECTION.md](../docs/reference/RELEASE_PROTECTION.md)に記録した。
CSVやログを署名付き配布フォルダの中へ出力しない。次の起動時に余分なファイルとして拒否される。

Unity側の`RangeComparison.Editor.RangeBuild.BuildProtected`でIL2CPP/Medium stripping/Releaseの実Playerを生成・検証した。
支援モジュールはEditor本体の`C:/Program Files/Unity/Hub/Editor/6000.3.7f1`へ導入済み。Dドライブの既存Unityプロジェクトは変更していない。
raw出力は`output/packages/range-unity-il2cpp-raw`、配布は`range-unity-il2cpp-protected`、同じコードのMono対照は`range-unity-mono-control`。
`-rangeBuildOutput C:/.../new-directory/RangeUnity.exe`で新しい出力先を選び、`tools/release/package_unity.py`で配布コピーを作る。
生成C++/PDB/内部バックアップは配布外に保持する。詳しい再現手順・監査は [RELEASE_PROTECTION.md](../docs/reference/RELEASE_PROTECTION.md#第3段階unity-il2cppの実ビルド配布比較まで完了)。
IL2CPPは3シナリオ13582ステップ一致、Mono対照もクリア1114ステップ一致。条件`cases.unity-protection.json`で同じD3D11の反復16/16回も正常終了。
今回のUnity内での構成比較は、自作版DX12との比較の保留を解除するものではない。旧D3D12終了クラッシュは未解決として扱う。

ビルドAPIと起動設定の根拠は [BuildPipeline.BuildPlayer](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/BuildPipeline.BuildPlayer.html)、
[Player起動引数](https://docs.unity3d.com/6000.0/Documentation/Manual/PlayerCommandLineArguments.html)、
[SplashScreen.show](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/PlayerSettings.SplashScreen-show.html) を参照。
