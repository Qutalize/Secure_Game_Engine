# イベント向け比較計測

自作ゲームと Unity の Windows 配布版を同じ時計と CSV 形式で測るための基盤です。[共通射撃場](../../comparison/README.md)を両方に実装し、状態照合とPlayer実行まで確認しました。見た目・操作感の確認と終了安定性の検証は継続中です。既存 FPS と Unity 教材をそのまま測っても性能優劣の根拠にはなりません。イベント全体の方針は [ENGINE_DIFFERENTIATION.md](../../docs/reference/ENGINE_DIFFERENTIATION.md) を参照してください。

Unity D3D12版は、このPCで終了時のアクセス違反を確認したため比較を保留しています。
`known_issue` のあるケースは既定で停止し、原因調査時のみ `--allow-unstable` で実行します。
通常プレイはD3D11を優先します。[画像のエラーとスタックの調査](../../comparison/WARNINGS.md)を参照してください。

## Windowsの終了クラッシュを調べる

`windows_crash_probe.cpp` は自分で起動する子プロセスだけを調べる補助ツールです。
初期化時から監視する方式と、`--late-attach` で6秒経っても終了しない場合に接続する方式があります。
後者は1秒程度の短い自動終了テスト向けです。通常の長時間ゲームに使うと、プレイ中のスタックになります。
既定では子のウィンドウを隠すため、通常のプレイ表示と揃える場合は `--show-player` を指定します。
終了しない子は指定秒数で停止します。接続方式の制限時間は接続後から数えます。

ビルド（VS2022のx64 Native Toolsコマンドプロンプトで、リポジトリルートから）：

```bat
cl /EHsc /std:c++20 tools\benchmark\windows_crash_probe.cpp /Fooutput\windows_crash_probe.obj /Feoutput\windows_crash_probe.exe /link dbghelp.lib
output\windows_crash_probe.exe --late-attach --show-player output\crash.txt 10 プロジェクトルート\output\packages\range-unity\RangeUnity.exe -force-d3d12 --metrics プロジェクトルート\output\crash.csv --benchmark-seconds 1 --benchmark-warmup 0.25 -logFile プロジェクトルート\output\crash-player.log
```

障害/停止時はスタック・モジュール位置・ミニダンプをローカルへ保存します。ローカルシンボルのみを参照します。
デバッガーの接続や表示状態は挙動に影響するため、このツールの時間をベンチマーク結果として使用しません。
エラー処理の待機もタイムアウトになり得るので、`sample_end` があっても成功には変更しません。

## 測れるもの

| 値 | 定義 |
|---|---|
| 起動から準備完了まで | ランチャーがプロセスを起動する呼出しの直前から、ゲームが明示した `scene_ready` まで |
| 起動から最初の更新まで | 同じ起点から、準備完了後の `first_update_begin` まで |
| フレーム間隔 | 準備後のウォームアップを除いた更新開始間の生の経過時間。中央値・p95・p99・最大・33.333ms超の件数 |
| 起動の内訳 | 自作側はウィンドウ、描画初期化、音声、pak確認、通信初期化、シーン準備の累積時刻 |
| 配布容量 | ケースに `distribution` を指定した場合、そのフォルダー内の全ファイルの論理サイズ合計 |

`scene_ready` はプログラム上の宣言です。画面の初回表示、物理入力から光が出るまでの遅延、実際に遊べることを観測した結果ではありません。Unity は更新コールバック、自作側はメインループ入口なのでスケジューラーの位置にも差があります。GPU時間やCPUの純粋な処理時間とも区別します。起動時の関数名に `returned` / `finished` があるものは、初期化の成功を保証しません。欠落アセット・シェーダーフォールバック・音声失敗はログを確認して除外してください。

同一 Windows PC の QueryPerformanceCounter を親・子両方で使用します。内部の main や Unity の C# 初期化からだけ測ると、それ以前のエンジン起動を除外してしまうためです。[Microsoft のQPC仕様](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps)

## 自作側の実行

新規 `.cpp` を追加したので再構成が必要です。既存 `build` を上書きせず計測用の出力先を使用します。

シェーダー探索先のCMake定義がRendererへ届かない既存問題も修正しています。ビルド時にdxcが検出され、起動ログに `Loaded precompiled shader` が出ることを確認してください。実行時コンパイルへ落ちた試行と混ぜないでください。

```powershell
cmake -S . -B build-event -G "Visual Studio 17 2022" -A x64 -DHARDENED=OFF
cmake --build build-event --config Release --target game
python tools/benchmark/run.py tools/benchmark/cases.native.example.json --output output/benchmarks/native-01 --runs 5
python tools/benchmark/plot.py output/benchmarks/native-01/summary.json
```

`python` と `cmake` は使用する環境の実行ファイルへ読み替え可能です。描画付きのゲームウィンドウが各試行で開き、既定では準備後2秒待って10秒計測し自動終了します。計測中は P/M・操作・ウィンドウ移動を行わず、フォーカスと電源設定を保ってください。自作側は既定で VSync ON です。フレーム間隔がモニターの周期に制限される点を条件に残します。

引数を付けない通常起動では CSV 出力・自動終了は無効です。単独で試す場合:

```powershell
build-event/Release/GameEngine.exe --mode fps --metrics output/native-trace.csv --benchmark-seconds 10 --benchmark-warmup 2
```

出力先の親フォルダーは事前作成してください。`--metrics` 指定時は既定10秒で自動終了し、指定CSVを上書きします。ランチャー経由なら出力フォルダーの既存時はエラーにして過去データを保護します。正常終了時に書き出すため、起動中のCSV監視には使いません。

## Unity 側の接続

1. 比較用 Unity プロジェクトの `Assets/Benchmark/` に `unity/EventBenchmark.cs` をコピーする。教材のテンプレート固有コードは不要です。
2. 共通ミニゲームの初期化担当コードで、シーン、必要アセット、カメラ、入力が利用可能になった場所から、次を一度呼ぶ。

```csharp
GameEngine.Benchmark.EventBenchmark.MarkSceneReady();
```

3. Windows Standalone の非 Development Build を作り、バージョン、IL2CPP/Mono、描画API、品質、ストリッピング、VSync、解像度、スプラッシュ設定を記録する。Editor では計測コードが無効です。
4. manifest の `cases` に Unity の exe と作業フォルダーを追加する。`command` は文字列配列。`cwd` と `distribution` は manifest の場所からの相対パス、実行ファイルは `cwd` からの相対パスまたは絶対パスです。`distribution` は任意です。
5. 共通ゲームが完成し条件を揃えてから両ケースを一つの manifest で交互計測する。Unity の `--metrics` 等の引数はランチャーが付けます。`-batchmode` / `-nographics` は描画比較条件を変えるので指定しません。

自動の `sceneLoaded` で準備完了とみなさず、ゲーム側の準備完了宣言を必須にしています。未接続は成功値にせずタイムアウトになります。`DefaultExecutionOrder(-32000)` は計測入口を早める設定です。基盤のオブジェクトは `DontDestroyOnLoad` で維持します。現教材へこの一呼出しを付けても「同じゲームを実装した」ことにはなりません。

## 結果と実験条件

`manifest.resolved.json` に解決後のコマンド、exe の SHA-256、OS、QPC周波数、条件メモ、乱数seedを保存します。全アセットのハッシュやGPU情報は自動収集しないため、ビルドの配布一式とGPU/ドライバー/リフレッシュレートの記録を別途保管します。`runs.csv` / `runs.json` は試行単位、各生CSVはイベント/フレーム単位、`summary.json` はケース単位です。ログ・失敗理由を残し、失敗試行を成功として集計しません。

最初の1起動はケースごとの準備運転として保存し集計対象外にします（`--warmup-runs 1`）。それ以降もOS/GPUのキャッシュを制御していないため「コールド起動」とは称しません。再起動条件を揃えたコールド実験は別に設計します。単に1回目という理由でコールドに分類しません。各ラウンドのケース順序をseed付きでランダム化します。

初期確認は5試行、発表用はまず30試行を目安にし、中央値・p95・試行数を併記します。30試行のp95にも大きな不確かさが残るので、差が小さければ追加測定します。フレームp95の集計は「各試行内p95の中央値」であり、起動p95と混ぜません。グラフ生成だけは `matplotlib` が必要です。

グラフ用の依存は `python -m pip install matplotlib` で使用するPython環境に導入します。今回の検証では既存環境を変更せず `build-event/python-packages` へ配置しました。その環境を使う場合は `$env:PYTHONPATH = "$PWD/build-event/python-packages"` を指定してから `plot.py` を実行します。

両アダプターとも最大10万件の固定容量を先に確保し、計測中のディスクI/Oを避けます。計測自体のメモリとQPC呼出しの負担は残ります。あふれた場合や途中終了は無効試行です。計測付き/なしによる負担の校正、RAM/VRAM、入力→表示の外部計測、固定入力再生は今後の実装です。

```powershell
python -m unittest discover -s tools/benchmark -p "test_*.py" -v
```

このテストは未完走・異なる時計・dt上限処理による値の書換・NaN・バッファあふれを成功と誤認しないこと、および準備運転・失敗試行を集計へ混ぜないことを確認します。
