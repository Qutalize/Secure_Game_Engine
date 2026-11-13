# ゲーム種類・機能選択と最小ランタイム生成

2026-10-29更新。ゲーム種類と必要な機能をJSONまたは番号入力で指定し、自作エンジンの**制作サンプルをビルド・パッケージ化・起動**できます。外部パッケージは理由と公式資料を付けて提案します。

## 1. 自作エンジンで制作を始める

| `game` | 起動する制作サンプル | 作品にするための主な追加作業 |
|---|---|---|
| `fps` | 一人称移動・射撃・壁による遮蔽 | 武器、敵AI、ステージ |
| `tps` | 三人称追従カメラ・射撃 | カメラ遮蔽、照準補正、キャラクター |
| `aim_training` | 一人称の的当て・スコア | トラッキング課題、時間制限、成績保存 |
| `monster_capture` | 三人称探索・近くの対象を回収 | 捕獲確率、図鑑、育成、セーブ |
| `action_rpg` | 三人称探索・近接インタラクション | 戦闘、スキル、クエスト、広域マップ |
| `chase` | 動く対象に近づいてタッチ | 追跡AI、役割別能力、勝敗、オンライン化 |
| `social_deduction` | タスク地点に近づいて操作 | 秘密役職、会議、投票、受信者別配信 |
| `new_game` | 自由移動・箱のシーン | 任意のルール・シーン制作 |

箱のモデルと手続き音源を使う小さな制作の土台です。完成したゲームを生成する機能ではありません。原神型の探索アクションとポケモン型の捕獲育成、第五人格型の追跡とAmong Us型の正体隠匿をそれぞれ別の設計項目として扱います。既存タイトルの素材は含めません。

`range` / `card` / `hide` は従来ゲームの提案用識別子です。従来の `all/fps/range` ビルドを使います。個別選択ビルドの `fps` は新しい制作サンプルであり、旧FPSのステージや対策実験を自動変換するものではありません。共通比較射撃場とエイム練習サンプルも別です。

操作：WASD/左スティックで移動、Tabでマウス捕捉、マウス/右スティックで視点、E/捕捉中のクリック/R2で行動、Rでリセット、Esc/Optionsで終了。衝突を外すと壁・射撃命中・タッチ判定も外れます。スコアはウィンドウタイトルに表示します。剛体を選んだ構成はSpaceで箱へ力積を与えます。

## 2. 個別選択をビルドへ反映する

| 選択 | 組み込むもの／外すもの |
|---|---|
| `physics: []` / `["collision"]` | `Collision.cpp`と呼び出しを除外／AABB衝突・レイ判定を組み込む。汎用剛体ではない |
| `physics: ["rigid_body"]` | 衝突を依存追加しJolt 5.5.0を静的リンク。回転する箱の重力・接触・摩擦・反発・力積 |
| `rendering: none` / `raster` | Window・Input・Gamepad・Renderer・DX12リンク・全シェーダーを除外／描画を組み込む |
| `material: unlit / simple_lit / pbr` | 照明なし／簡易照明／PBR。PBR選択時だけPBR実装・シェーダー・テクスチャ転送を追加 |
| `shadows: none / shadow_map` | シャドウマップ作成・影パス・影のVSを除外／追加。OFFではPSの影サンプリングもコンパイル時に除去 |
| `rendering: hybrid` | ラスタ描画にRT影・反射を併用。選択時だけRayTracing.cpp・RT定数/資源とHLSLを追加 |
| `shadows: ray_traced` / `reflections: ray_traced` | 影と1回反射を独立選択。反射だけ＋シャドウマップも可能 |
| `ray_acceleration: software / hardware_required` | GPUソフトウェア交差判定／DXR 1.1 inline RayQuery。unspecifiedは前者として明示 |
| `model: none / obj / glb / fbx` | モデル処理を除外／指定形式をビルド時に配布用メッシュへ変換 |
| `model_path` / `model_scale` | 選択形式と拡張子が一致するプロジェクト相対パス／読み込み時に形状へ焼き込む一様倍率 |
| `ragdoll: none / passive` | ラグドール個体を追加。GLB/FBX＋骨アニメーション必須、2～32骨 |
| `deformation_ccd: none / conservative` | 変形三角形と動的剛体/プレイヤーの連続判定。deforming_mesh必須 |
| `model_physics: none / convex_hull / static_mesh / deforming_mesh` | モデル物理なし／動く凸包／固定三角形／骨変形へ追従する三角形。必要な物理APIだけをコンパイル |
| `model_ray_tracing: none / triangles` | モデル交差処理を除外／実メッシュのBVH・GPUバッファとDXR形状を追加 |
| `animation: none / skeletal` | 骨変形処理とデータを除外／GLB・FBXのCPUスキニングを追加 |
| `animation_clip` | 組み込むクリップ名。空文字は先頭、指定名が存在しない・重複する場合は拒否 |
| `distribution: unsigned / signed` | 署名検証なし／公開鍵埋込・配布物の署名・起動前検証。signedではbcryptと検証コードを追加 |
| `audio: none / basic` | `AudioPlayer.cpp`・XAudio2リンク・初期化を除外／BGM・効果音を追加 |
| `lights: none / directional` | 光源強度なし／平行光。影を使う場合はdirectionalが必須 |
| `indirect_light: none / ambient` | 環境光なし／一定の環境光 |
| `physics_hz` | 固定更新Hz。低Hzの移動には衝突のすり抜けを抑える内部小刻み処理を使う |
| `width / height` | ウィンドウとレンダーターゲットの解像度 |
| `target_fps` | 比較用の目標を保存。達成保証や描画制限値ではない。描画はVSync |

PBRのためのテクスチャ転送は依存として表示します。旧Texturedパイプライン、従来のOBJ/glTFローダーとGPU骨描画、Pak/AES、UDP/TLS、チート検証ツールはこの制作サンプルに入りません。Rendererの共通データ型や基礎処理は残るため、機能OFFを「関連する全メンバー変数の消滅」とは呼びません。

未対応の関節・布・流体、全面パストレーシング、環境マップ/画面空間反射・高度なGI、変形メッシュの剛体接触更新、空間音響、オンライン権威サーバーは、理由を表示して**生成前に拒否**します。骨変形は今回のCPU経路、カードTLSは従来ゲームの経路です。unlitに照明・影を付ける等の矛盾も、設定を無視せず拒否します。

### ビルド・起動

Windows、VS2022 C++ツール、CMake、Windows SDKのDXC、Python 3.9以上が必要です。実行先にも対応するVisual C++ランタイムが必要です。描画版はDX12 feature level 12.0対応GPUを使います。Python側に追加pipパッケージは不要です。

```powershell
# Dドライブのプロジェクトルートから
Set-Location D:\Gameengine

# 対話で選び、対応している構成を生成して起動
python -B tools/advisor/build_runtime.py --interactive --run

# 既存の設定例から生成・起動
python -B tools/advisor/build_runtime.py --config tools/advisor/examples/selected-aim.json --run
python -B tools/advisor/build_runtime.py --config tools/advisor/examples/selected-tps.json --run

# 既存のPowerShell入口でも実行可能。Pythonを自動検出。見つからない場合は-Pythonで実行ファイルを指定
./tools/build.ps1 -Config tools/advisor/examples/selected-capture.json -Run

# 8フレームの自動起動確認、衝突/命中検査、計測CSV保存
python -B tools/advisor/build_runtime.py --config tools/advisor/examples/selected-free.json --name free-test --smoke-test
```

同じ出力名のパッケージは上書きしません。再生成は `--name aim-v2` のように別名を指定します。既存exeを再度遊ぶだけなら、そのexeを起動してください。外部パッケージのダウンロードやインストール、Unityプロジェクトの変更は行いません。

出力は `output/packages/selected-<名前>/SelectedGame.exe`。指定なしならゲーム種類＋設定ハッシュです。キャッシュは `output/build-selected/<設定ハッシュ>/`、最後のビルドログはその `last-build.log`。失敗時もログを残します。

パッケージには必要なDXILだけを明示リストでコピーし、ソース・モデル・不要なシェーダーを同梱しません。`request.json`が入力、`runtime-plan.json`が依存を解決した構成、`build-inputs.txt`がCMakeに渡ったソース・リンク一覧、`recommendations.json`が候補と理由、`inventory.json`がファイルサイズ・SHA256です。`--describe`でexe自身のコンパイル済み設定も読めます。

Releaseには既存の最適化・メタデータ削減・CFG/ASLR/NXを適用します。`distribution: signed`では、鍵の対応をビルド前に確認し、公開鍵だけをexeへ埋め込み、全ファイルの出力後にECDSA P-256で署名します。署名・ハッシュ・欠落・余分なファイルを**ゲーム初期化前**に検査し、`--describe`/`--self-test`でも迂回できません。検証済みファイルは終了まで書込/削除共有を許可しないハンドルで保持します。CSV等の出力はパッケージの外に置きます。`inventory.json`自体は署名ではなく、`distribution.manifest`と`distribution.sig`が署名の実体です。

これはAuthenticodeではなくアプリ内の配布整合性検証です。検証器自体を改変できる相手に対する完全な防御、逆コンパイル不能化を意味しません。署名なしを選ぶと検証コードとbcryptリンクを外します。秘密鍵は配布物へコピーしません。署名にはPython 3.11以上と`cryptography`が必要です。

### 剛体・RT・署名の構成例

```powershell
# プロジェクトルートで実行。毎回新しい--nameを指定する
python -B tools/advisor/build_runtime.py --config tools/advisor/examples/selected-rigid.json --name rigid-v2 --run
python -B tools/advisor/build_runtime.py --config tools/advisor/examples/selected-rt-software.json --name rt-v2 --run

# 同梱の実験用鍵を利用する例。本番の秘密鍵は別管理し、配布フォルダへ入れない
./tools/build.ps1 -Config tools/advisor/examples/selected-rigid-rt-signed.json `
  -Name combined-v2 -SigningKey output/release-identity-lab/release-private.key `
  -PublicKey output/release-identity-lab/release-public.bin -SmokeTest
```

署名に使うPythonを検出できない場合は`-Python <python.exe>`を指定します。JSONの`distribution`に加え、`-Signed`（Pythonでは`--signed`）でも明示的に署名構成へ変更できます。鍵未指定・鍵不一致はビルド前にエラー。秘密鍵を自動生成することはありません。署名の有無と、剛体・RT・音声は独立して選べます。

剛体は箱に加え、モデル用の凸包または固定三角形メッシュを選べます。最大128物体、単一スレッドです。ユーザー指定Hzの更新を最大1/120秒の内部ステップに分割します。プレイヤー移動はAABBのままで、動的な箱を押して歩くキャラクターコントローラーではありません。動的な凹形状・凸分解・拘束は未接続です。ライセンスと固定版情報を剛体選択時のみ配布へ含めます。

RTは最大32個の回転・拡縮したインスタンスを扱い、箱と登録した三角形モデルを混在できます。モデルなしのGPUソフトウェア経路は箱との交差だけを計算します。モデルを選ぶとCPUでBVHを構築し、HLSLでBVHと三角形を探索します。DXR経路は箱・登録モデルごとのBLASとフレーム別TLASを使い、同じモデルの複数配置でBLASを共有します。陰影は平行光の遮蔽、反射は1バウンス＋反射先の遮蔽です。GLB/FBXと骨変形メッシュにも対応します。骨変形時はBVHとDXR BLASをフレームごとに再構築します。全面パストレーシング、RT GI、粗い反射の畳み込みは未対応です。反射のみでも反射先の陰影用レイは使います。

`hardware_required`ではDXR Tier 1.1とShader Model 6.5を必須とし、非対応機でソフトウェア経路へ切り替えません。このPCのIntel Iris XeはTier 0でした。DXR経路のコンパイルと起動拒否は確認済み、対応GPUでの正常描画は未検証です。GPUソフトウェアRTは実行と数値検証を確認済みですが、指定した目標fpsを保証するものではありません。

生成済み：[剛体＋RT影/反射＋署名](../../output/packages/selected-rigid-rt-signed/SelectedGame.exe)、[影だけ](../../output/packages/selected-rt-shadow-only-independent/SelectedGame.exe)、[反射だけ](../../output/packages/selected-rt-reflection-only-independent/SelectedGame.exe)。JSON例は`examples/selected-rigid*.json`、`selected-rt-*.json`、`selected-signed-minimal.json`です。

新しいサンプルは旧比較射撃場とゲーム内容が違うため、その起動時間をUnityとの同一ゲーム比較には使えません。

### 箱以外のモデルを使う

三角形化したOBJをプロジェクト内へ置き、JSONから指定します。例：

```json
{
  "game": "tps",
  "model": "obj",
  "model_path": "assets/models/creator/tetrahedron.obj",
  "model_scale": 1,
  "model_physics": "convex_hull",
  "rendering": "hybrid",
  "shadows": "ray_traced",
  "reflections": "ray_traced",
  "ray_acceleration": "software",
  "model_ray_tracing": "triangles",
  "width": 960,
  "height": 540
}
```

`model_physics`を`none`にすれば物理を外し、`static_mesh`なら穴や凹みを保つ固定物になります。動く物体の`convex_hull`は凸包近似なので凹みを埋めます。三角形そのままの凹形状を動的剛体へ変換したという意味ではありません。凸包を生成できないモデルは理由を表示して止め、箱へ置き換えません。モデル物理を選ぶと`rigid_body`と既存の衝突処理を依存として追加します。

`model_ray_tracing: triangles`には`rendering: hybrid`とRT影または反射が必要です。`none`ではモデルをRTの交差・遮蔽・反射先から除外します。表示される面に対して他の物体の影・反射を計算する材質処理とは別の選択です。RTシーンに登録する変換行列は、剛体の位置・回転から作った描画用行列と共通です。形状登録は最初のフレーム前に固定します。骨変形を選ぶと三角形数を維持して頂点とRT構造を更新します。

OBJ経路で変換するのは頂点位置と三角形です。正・負の頂点番号に対応します。材質・テクスチャ・UV・指定法線・骨は取り込まず、面法線と共通色で描画します。MTLや画像の外部パスを読みません。四角形・多角形は事前に三角形化してください。固定物理メッシュの表裏は頂点の巻き順で決まり、RTは不透明な両面として交差します。透明度やアルファ抜きには未対応です。

上限は元モデルデータ8 MiB、参照頂点65,536、三角形65,536、凸包の入力点4～4,096です。凸包は体積が必要で、Joltの形状生成制約にも従います。RT全モデルの合計も65,536三角形まで、登録形状31種類＋箱、配置は合計32個です。`model_scale`は0.001～1000、変換後座標は各軸±100,000以内です。大きなモデルの速度、各サイズでの衝突・光線バイアスの品質は別途測定してください。

選択されたモデルだけを`assets/model.mesh`へ変換します。元OBJ/GLB/FBXは配布へコピーしません。これは読込用のバイナリ形式であり、形状を秘密にする暗号化ではありません。頂点・面の上限、有限値、添字、退化面、ファイル長を変換時と実行時に検査します。署名選択時はこのメッシュも署名対象です。元データと変換結果のSHA256を記録し、モデル内容が変わればビルドキャッシュの識別子も変わります。

`TriangleMesh.cpp`はモデル選択時だけ、`TriangleAcceleration.cpp`とモデル用GPUバッファはモデルRT選択時だけ組み込みます。モデル表示のみならJoltやRTは不要です。Joltを選んだ構成ではライブラリ共通の型登録・ソルバが残り、内部の形状クラスすべてを個別に削除するものではありません。既存のOBJ/glTFローダーを丸ごと起動する経路ではありません。プレイヤーの移動・射撃判定は従来AABBで、モデル物理と連動するキャラクター制御は未実装です。

```powershell
python -B tools/advisor/build_runtime.py --config tools/advisor/examples/selected-model-render.json --name my-model-view --run
python -B tools/advisor/build_runtime.py --config tools/advisor/examples/selected-model-static.json --name my-model-stage --run
# selected-model-convex / selected-model-rt / selected-model-dxr / selected-model-combined も選択可能
```

生成済み：[モデル表示だけ](../../output/packages/selected-model-render/SelectedGame.exe)、[固定メッシュ](../../output/packages/selected-model-static-v1/SelectedGame.exe)、[モデル物理＋RT＋署名](../../output/packages/selected-model-combined-final/SelectedGame.exe)。最後の構成はSpaceでモデルへ力積、Rで再初期化できます。

モデル対応は41単体テスト、7構成の配布監査、署名関連17ケース、最終統合構成180フレーム×3回に成功しました。記録は`output/validation/selected/models/summary.json`。GPU読み戻し16ケース、ネイティブの不正メッシュ12ケース、署名付きモデルの改ざん・欠落拒否、機能除外・反復起動を保存します。ソフトウェアRTはこのPCで実行済み、DXRはコンパイルと非対応機の拒否までです。見た目・操作感の人による評価とDXR対応GPUでの成功確認は残ります。

```powershell
python -B tools/advisor/verify_models.py output/packages/selected-model-rt-v1
python -B tools/advisor/verify_advanced.py output/packages/selected-model-combined-final
```

### GLB/FBXと骨アニメーションを選ぶ

```json
{
  "game": "tps",
  "model": "glb",
  "model_path": "assets/models/creator/two-bone.glb",
  "model_scale": 1,
  "animation": "skeletal",
  "animation_clip": "Swing",
  "model_physics": "none",
  "physics": [],
  "rendering": "hybrid",
  "shadows": "ray_traced",
  "reflections": "ray_traced",
  "model_ray_tracing": "triangles"
}
```

`model`と`model_path`を`fbx`と`.fbx`のパスへ替えると、ASCII・バイナリFBXをビルド時に変換します。FBX変換専用の[ufbx](https://github.com/ufbx/ufbx) 0.23.1を同梱しており、初回のみMSVC/CMakeで変換ツールを生成します。ネット接続やBlenderはビルドに不要です。`third_party/ufbx/UPSTREAM.json`に固定コミット・SHA256、`LICENSE`にライセンスがあります。ufbx自体をゲーム実行物へリンクしません。

```powershell
# 自作GLBの骨変形。--nameは毎回異なる名前にする
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-glb-animated.json --name glb-animation-demo --smoke-test
# FBXの骨変形
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-fbx-animated.json --name fbx-animation-demo --smoke-test
# PBRとモデルRTを併用
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-glb-animated-rt.json --name animated-rays-demo --smoke-test
```

GLBではシーン内のメッシュ、複数プリミティブ、ノード変換、JOINTS_0/WEIGHTS_0、逆バインド行列を読み込みます。ボーンの移動・回転・拡縮を階層に沿って合成します。FBXはufbxでノード変換・ピボット・アニメーションスタックを評価し、Y-upのメートル単位へ変換します。GLBもY-up・メートルを前提にし、共通の`model_scale`を最後に適用します。材質、画像、UV、指定法線は取り込まず、面法線と共通色で表示します。元データが示す外部バッファ・画像・キャッシュを開きません。GLBは外部バッファを含む入力を拒否します。

骨変形を選んだ場合だけ、選択クリップのボーン行列を約60Hz（間隔は1/60秒以下）で評価し、ウェイト・バインド位置と一緒に`assets/model.anim`へ保存します。再生時は隣接サンプルの行列補間と4ウェイトのCPUスキニングです。GLBのLINEAR/STEP/CUBICSPLINEは変換時に評価しますが、再生はサンプリング近似で、元の曲線や不連続変化と完全に一致する保証はありません。クリップのブレンド、IK、モーフ、デュアルクォータニオンには対応していません。受動ラグドールは後述の独立選択で対応します。1頂点4本を超えるボーンは自動で切り捨てず拒否します。

上限は512ノード、256パレット要素（複数スキンや通常ノードの変換も含む）、60秒、3,601サンプル、変換後アニメーション64MiBです。元モデルは8MiB、頂点・三角形はそれぞれ65,536までです。NaN/Inf、範囲外の骨番号、合計が1でない配布ウェイト、非アフィン行列、長さ不一致、メッシュと初期姿勢の不一致を実行時にも拒否します。再生中も有限値と非退化三角形を検査するため、面が完全につぶれるアニメーションは停止してエラーになります。

静止GLB/FBXでは面積ゼロの三角形をビルド時に除去し、`discarded_degenerate_triangles`へ個数を記録します。元モデルは変更しません。骨変形モデルでは後の時刻に面が現れる可能性があるため、この自動除去を行わず拒否します。

ラスタ描画とRTは同じCPU変形結果を使い、面法線も再計算します。GPU処理中の資源を書き換えないよう、頂点、RT三角形/BVH、DXRの頂点/BLAS/スクラッチは3フレーム分の資源とフェンスで管理します。RTは毎フレーム構造を再構築する実験実装で、大量のキャラクターや大きなモデルでの性能は未評価です。専用GPUスキニング経路ではありません。構成レポートの`model_animation`が今回の選択フラグで、`skinning`は従来GPU経路のためfalseです。

`animation: none`は静止姿勢だけを`model.mesh`へ焼き込み、`SkeletalAnimation.cpp`・`model.anim`・変形用フレーム資源を除外します。`model: none`ではモデル自体を除外します。GLB/FBXの静止モデルは既存の凸包・固定メッシュ物理と組み合わせられます。骨変形には`model_physics: none / deforming_mesh`を選べます。`convex_hull / static_mesh`との同時選択は拒否します。シーンのほかの箱の剛体は併用できます。

署名を選ぶと`model.mesh`と`model.anim`も起動前検証の対象です。選択クリップの内容もビルド識別子とインベントリへ含めます。元のGLB/FBX、変換ツール、Python、未選択クリップはゲームパッケージへコピーしません。配布形式のバイナリ化・署名は逆解析不能化ではありません。

自作モデル：`assets/models/creator/two-bone.glb`、`two-bone.fbx`、`two-bone-binary.fbx`（CC0）。再生成は`python tools/advisor/model_fixtures.py`。各形式の静止・骨変形・RT・署名の設定例は`examples/selected-glb-*.json`と`examples/selected-fbx-*.json`です。

### 骨変形に衝突を追従させる

`animation: skeletal`に加えて`model_physics: deforming_mesh`を選びます。GLB/FBX両対応で、物理・衝突を依存として追加します。選択しない構成では`DeformingMeshCollider.cpp`とそのAPI・更新処理をビルドから除きます。骨表示だけならJoltも不要です。

```json
{
  "game": "fps",
  "model": "glb",
  "model_path": "assets/models/creator/two-bone.glb",
  "animation": "skeletal",
  "animation_clip": "Swing",
  "model_physics": "deforming_mesh"
}
```

```powershell
# 骨変形・衝突のみの最小版（描画、音声、RTなし）
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-glb-deforming-headless.json --name my-deforming-headless --smoke-test
# 描画・ソフトウェアRTも組み込む版
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-glb-deforming-rt.json --name my-deforming-rt --smoke-test
# 署名付きFBX版。既存の署名鍵を指定する
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-fbx-deforming-rt-signed.json --name my-deforming-signed --signing-key <秘密鍵> --public-key <公開鍵> --smoke-test
```

物理の各サブステップ（最大1/120秒）でスキニングし、変形済み三角形をJoltの静的形状へ交換します。接触キャッシュ・空間探索を更新し、新旧範囲にいる停止中の剛体を再開します。同一姿勢は更新せず、物体の停止を妨げません。描画・RTも物理に渡した同じ頂点を使います。

剛体の接触は巻き順の表面側、射撃・プレイヤーの箱スイープは両面です。形状の穴を保持し、速いプレイヤー移動の停止、面に沿う移動、入力なしの食い込み解消を行います。既存の壁AABB判定と併用し、ゲームの射撃は変形モデルで遮蔽されます。Rで時刻・形状をリセットします。

**上限・制限**：この選択では4,096三角形まで。毎物理更新で形状を再構築する実験実装です。アニメーションが制御する障害物であり、骨への力の反作用・ラグドール・軟体・移動床の速度継承・段差昇降は未対応。既定の離散判定では高速な変形や瞬間移動ですり抜ける場合があります。後述の`deformation_ccd`で保守的な連続判定を選べます。プレイヤー押出しは最大8回、スイープと滑りは最大4回で、複雑な食い込みの完全解消を保証しません。大量のキャラクターでの性能は未評価です。

```powershell
# パッケージへは含めない実三角形の衝突検証ツール
cmake --build output/build-selected/<設定ハッシュ> --config Release --target deforming_test
./output/build-selected/<設定ハッシュ>/Release/deforming_test.exe <配布先>/assets/model.mesh <配布先>/assets/model.anim
```

自作2ボーンモデルを使い、射撃・スイープ・穴・剛体の支持と落下・停止と再開・ループ/リセット・無効形状の拒否を検証します。ツールの姿勢試験はこの自作モデル専用です。結果は[検証記録](../../output/validation/selected/deforming/summary.json)に保存しています。68テスト、GLB/FBXそれぞれ88件の衝突判定（計176）、GPU RT 96判定、署名19件、4構成の配布監査、180フレーム×3に成功しました。今回のビルドでコンパイラ・リンカーの警告/エラーはありません。人による画面・操作感や別PCでの確認とは区別します。

生成済み：[署名付きFBX＋骨変形衝突＋PBR/RT](../../output/packages/selected-fbx-deforming-rt-signed-v1/SelectedGame.exe)、[描画なし最小構成](../../output/packages/selected-glb-deforming-headless-final/SelectedGame.exe)。ログ・CSVは署名付きフォルダの外へ保存します。検証一式は`tools/advisor/verify_deforming.py --help`で再実行の引数を確認できます。

実装の形状交換と接触キャッシュ処理は同梱Jolt 5.5.0の`BodyInterface.cpp`、クエリは`TransformedShape.h`に照合しています。

### ラグドールと高速変形の連続衝突を個別に選ぶ

```json
{
  "game": "fps",
  "model": "glb",
  "model_path": "assets/models/creator/two-bone.glb",
  "animation": "skeletal",
  "animation_clip": "Swing",
  "model_physics": "deforming_mesh",
  "ragdoll": "passive",
  "deformation_ccd": "conservative"
}
```

`ragdoll`と`deformation_ccd`は独立して選べ、既定は両方`none`です。ラグドールのみなら`model_physics: none`でも動きます。連続衝突には`deforming_mesh`が必要です。4通りの設定を実際にビルド・起動します。未選択時は専用ソースとAPIを除外し、ラグドールOFFでは`model.rig`も配布しません。他の剛体機能で必要な共通Joltは残ります。

```powershell
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-ragdoll-headless.json --name my-ragdoll --smoke-test
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-ccd-headless.json --name my-ccd --smoke-test
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-ragdoll-ccd-headless.json --name my-dynamics --smoke-test
python tools/advisor/build_runtime.py --config tools/advisor/examples/selected-ragdoll-ccd-fbx-signed.json --name my-dynamics-signed --signing-key <秘密鍵> --public-key <公開鍵> --smoke-test
```

ラグドールは、初期のスキニング結果から骨ごとの箱剛体を生成し、親子を振り45度・ねじり±30度の関節でつなぎます。物理結果でモデルの頂点を変形させ、描画とRTにも渡します。元モデルの左側へ別個体を追加するサンプルです。**Spaceで末端骨へ衝撃、Rでリセット**します。射撃やプレイヤー移動によるラグドールへの反応は今回追加していません。

2～32骨、箱の各辺0.1～20mまで。関節軸や質量は自動の初期設定で、人体の部位ごとに調整した値ではありません。骨パレットが重複する複数スキン個体は整理が必要です。自己衝突、能動モーター、起き上がり、アニメーションからのブレンド、皮膚の厳密な三角形衝突は未対応です。

連続衝突は、更新間の三角形の直線移動と動的剛体・プレイヤーの外接球を対象に、接触時刻を保守的に計算して**変形を接触直前で止めます**。単なる中間姿勢の間引き検査ではありません。三角形の穴を保ちますが、外接球の近似で早く止まる場合があります。停止面の速度による押し出し・衝撃伝達は未対応です。

対象はベイク後の頂点経路です。アニメーションのサンプル境界で経路を分けます。倍精度の時刻を使い、微小な区間を物理ソルバーへ渡さない方式です。1物理ステップで8区間を超える極端に短いクリップ等は明示エラーになります。元の曲線全体や、別の接触/拘束で軌道が変わる動的剛体まで含む世界全体の厳密な連続解法ではありません。APIでのモデル全体の回転変更は拒否し、頂点経路へ焼き込む必要があります。静的な床・壁やラグドールの皮膚表面同士への連続三角形判定は対象外です。ラグドールの骨剛体はJoltのLinearCastを使います。

`model.rig`は骨階層・関節位置・箱の範囲を保存し、選択時だけ配布・署名します。循環、範囲外番号、NaN/Inf、不正な寸法・長さを拒否します。元モデルや変換ツールを配布せず、既存の`model.mesh`・`model.anim`形式も維持します。

[署名付きFBX＋ラグドール＋連続衝突＋RT](../../output/packages/selected-dynamics-fbx-rt-signed-final/SelectedGame.exe) ／ [描画なし最小版](../../output/packages/selected-dynamics-headless-final/SelectedGame.exe)。検証ツールは配布物には含めません。

```powershell
cmake --build output/build-selected/<設定ハッシュ> --config Release --target dynamics_test
./output/build-selected/<設定ハッシュ>/Release/dynamics_test.exe <配布先>/assets/model.mesh <配布先>/assets/model.anim <配布先>/assets/model.rig
python tools/advisor/verify_dynamics.py --help
```

試験は自作2ボーン素材に対するものです。結果と適用範囲は[検証記録](../../output/validation/selected/dynamics/summary.json)に保存しました。76テスト、物理426判定、従来衝突88、不正rig12、署名21、GPU96（2モデル同時更新）、5構成監査、描画なし1,200フレーム、描画180フレーム×3回に成功しました。最終構成のコンパイラ/リンカー警告・エラーはありません。初期実装で約2秒以降に時刻が停滞した問題を修正し、描画なし1,200フレームを確認しました。5秒の計測打ち切りと、時間制限なしの180フレーム完走は分けて記録します。CPU更新間隔は表示遅延・入力遅延や物理処理だけの所要時間を意味しません。

## 3. 制作環境・追加パッケージの提案

```powershell
python -B tools/advisor/advisor.py --interactive
python -B tools/advisor/advisor.py --config tools/advisor/examples/selected-capture.json
python -B tools/advisor/advisor.py --list-options
python -B tools/advisor/advisor.py --config tools/advisor/examples/web-rigid.json --json --out output/advisor-result-new.json
```

`native_build.status=ready` が個別ビルド可能、`blocked` は理由を確認して機能実装または条件変更が必要です。`candidates`は自作エンジンの既存部品と外部エンジンの候補表で、個別ビルドの準備状況とは別です。`priority`は希望として保持し、未計測の工数・性能から総合順位を作りません。

| 条件 | 追加候補 | 注意点 |
|---|---|---|
| C++で剛体・関節が必要 | [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | 5.5.0の箱・モデル物理・骨変形衝突・受動ラグドールを選択時に統合。汎用の関節編集は未接続 |
| FBXを自作エンジンへ取り込む | [ufbx](https://github.com/ufbx/ufbx) | 0.23.1をビルド用として統合。ゲームへはリンクしない |
| Webで剛体・関節が必要 | [Rapier](https://rapier.rs/docs/user_guides/javascript/getting_started_js/) | WASMロード・初回転送・接続を検証 |
| FPS/TPS等でカメラを作る | [Unity Cinemachine](https://docs.unity3d.com/Packages/com.unity.cinemachine@3.1/manual/index.html) | Unity版との整合・入力の接続を確認 |
| モンスター・敵・追跡者の移動 | [Recast/Detour](https://github.com/recastnavigation/recastnavigation)、[Unity AI Navigation](https://docs.unity3d.com/Packages/com.unity.ai.navigation@2.0/manual/index.html) | 経路探索とAIの意思決定を分ける |
| 権威サーバー型の複数人通信 | [Netcode for GameObjects](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.4/manual/index.html) | ルール判定・秘密情報の受信者制限は別途実装 |

公式資料の確認日：2026-10-29。パッケージの版、ライセンス、実行先、増える容量・初期化コストは採用前に固定して検証します。Joltとufbxは統合済みの範囲を表示し、その他の候補は導入済みを意味しません。布・流体など条件が不確かな機能へ無理な推薦はしません。

Three.js／Unity／Godotとの配布先・物理・描画方式の照合も継続しています。Webデプロイ、外部エンジン用の完成プロジェクト生成、GUI、実測による順位付け、GPUに応じた構成の自動最適選択は未実装です。DXR必須構成の起動時機能検査は実装済みです。

## 検証

```powershell
python -B -m unittest discover -s tools/advisor -p 'test_*.py' -v
# ビルド・起動・DLL依存・バイナリ/配布物の除去検証。suffixは毎回新しい名前
python -B tools/advisor/verify_runtime.py --suffix my-check
```

前段階のPython 28テストには、従来の機能選択、未対応選択の拒否、8ジャンルに加え、剛体×RT×署名の8通り、影/反射の独立フラグ、鍵未指定・鍵不一致の事前拒否を含みます。

```powershell
# 既に--smoke-testで生成したパッケージの監査。署名版は一時コピーで改ざん検査
python -B tools/advisor/verify_advanced.py output/packages/selected-rigid-rt-signed
# RTビルドのキャッシュでGPU読み戻しテストをビルド・実行（テスト実行ファイルは非配布）
cmake --build output/build-selected/<設定ハッシュ> --config Release --target ray_test
./output/build-selected/<設定ハッシュ>/Release/ray_test.exe
```

`verify_runtime.py`はキーの必要な署名構成と専用GPUを要するDXR構成を別検証として明示します。これらを通常の一括スモークへ紛れ込ませません。剛体・箱RT段階の追加検証は`output/validation/selected/advanced/summary.json`と同フォルダのログを参照してください。GPU読み戻しは交差・非交差・内部始点・回転拡縮・距離上限・影・反射の8結果を照合。統合署名パッケージは10種類の改ざん/欠落等を拒否し、180フレーム×3回の起動に成功しました。署名なしや描画なしでは関連ソース・DLL依存・目印文字列が除かれることも監査しています。

別PC、DXR対応GPUでの成功、画面・音・操作感の人による評価は残っています。
前段階の保存済み結果：24単体テスト、12構成のビルド/起動/除外検証に成功。旧all/FPSも正常終了、旧rangeの実Playerは基準と1,114ステップ完全一致。最終構成検証は `output/validation/selected/demo/results.json`。初回の依存監査はXAudio2の動的DLLロードをPEの静的importだけで調べて誤検出したため、動的ロード情報と実初期化ログを照合する方法へ修正しました。初回結果も履歴として残しています。

生成済みの例：[軽量エイム練習](../../output/packages/selected-aim-demo/SelectedGame.exe)、[PBR・影・音声ありTPS](../../output/packages/selected-tps-demo/SelectedGame.exe)、[捕獲の操作サンプル](../../output/packages/selected-capture-demo/SelectedGame.exe)。同じPCでの自動確認であり、目視・聴感・操作感評価は未完了です。

実装の参照元：Joltの固定版は[公式v5.5.0](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.5.0)。DXRは[Microsoft仕様](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)、[機能検査API](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-checkfeaturesupport)、[インスタンス変換形式](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_raytracing_instance_desc)を参照。固定ソースの取得記録は`third_party/jolt/UPSTREAM.json`。

モデル物理の参照：[Jolt ConvexHullShapeSettings](https://jrouwe.github.io/JoltPhysics/class_convex_hull_shape_settings.html)、[MeshShapeSettings](https://jrouwe.github.io/JoltPhysics/class_mesh_shape_settings.html)。実装は同梱5.5.0のAPIと照合しています。

GLB/FBX・骨変形の結果は[検証記録](../../output/validation/selected/animation/summary.json)に集約しています。61テスト、既知姿勢14ケース、不正骨ファイル20ケース、署名19ケース、GPU読み戻し96判定（16×6フレーム）、8構成の監査、署名構成180フレーム×3に成功しました。既存3Dモデル3点の変換とネイティブ読込も確認しました。静止版exeは110,592 bytes、骨付き版は120,832 bytesで、これは自作6頂点モデルと同一描画条件での値です。一般的なゲームの増加量・性能保証ではありません。

```powershell
# 骨付き構成のキャッシュに検証用ツールを生成（パッケージには含まれない）
cmake --build output/build-selected/<設定ハッシュ> --config Release --target model_test
# 自作2ボーンモデルの既知姿勢・不正ファイルを検証（署名なし版を指定）
python tools/advisor/verify_animation.py output/packages/selected-glb-animated-v1 --probe output/build-selected/<設定ハッシュ>/Release/model_test.exe
# 骨データの改ざん・欠落も含む署名検証
python tools/advisor/verify_advanced.py output/packages/selected-fbx-animated-rt-signed-v1
```

生成済み例：[GLB骨変形](../../output/packages/selected-glb-animated-v1/SelectedGame.exe)、[署名付きバイナリFBX＋骨変形＋PBR/RT](../../output/packages/selected-fbx-animated-rt-signed-v1/SelectedGame.exe)。`R`で時刻もリセットします。配布フォルダの中身を変えると署名検証が失敗するため、ログやCSVは外へ保存します。

モデル変換の参照：[Khronos glTF 2.0仕様](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)、[ufbx Animation](https://ufbx.github.io/elements/animation/)、[ufbx Reference](https://ufbx.github.io/reference)。
