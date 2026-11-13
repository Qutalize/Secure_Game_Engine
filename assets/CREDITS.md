# アセットの出典とライセンス

> **原則**: このプロジェクトで使うモデル・テクスチャ・音源は
> **CC0 / パブリックドメイン / 自作（手続き生成を含む）** のみとする。
> 新しいアセットを追加したら、必ずこの表に 1 行足すこと。

## 現在同梱しているもの

| ファイル | 種別 | 出典 | ライセンス |
|---|---|---|---|
| `models/test_model.obj` / `.mtl` | 3Dモデル | 自作 | パブリックドメイン |
| `models/test_figure.glb` | 3Dモデル | 自作（`tools/make_test_glb.py` で生成） | パブリックドメイン |
| `models/soldier.glb` | 3Dモデル＋テクスチャ | 自作（`tools/make_soldier_glb.py` で生成） | パブリックドメイン |
| `models/test_splat.ply` | 3DGS 点群（GS-01 ローダー検証用） | 自作（`tools/make_test_ply.py` で生成） | パブリックドメイン |
| 地面 / 壁 / 木箱 / カード絵柄 | テクスチャ | 自作（`game/GroundTexture.cpp`・`game/CardArt.cpp` で手続き生成） | パブリックドメイン |
| BGM / 効果音 | 音源 | 自作（`engine/audio/AudioPlayer.cpp` で手続き生成） | パブリックドメイン |

---

## ローカル所有アセット（Unity Asset Store 由来・★コミット禁止★）

開発者が所有している Unity プロジェクト
`D:\プログラミング\Unity\unity-assets\Assets\polyperfect\Poly Universal Pack`
から変換して使う。

### ⚠️ ライセンス上の重要事項

Unity Asset Store の EULA では、購入者は素材を**自分の製品に組み込んで使う**ことは許されますが、
**素材そのものの再配布は禁止**されています。したがって:

| 行為 | 可否 |
|---|---|
| このプロジェクトで使う（ローカル） | **可** |
| Git リポジトリにコミットする | **不可**（＝素材の再配布） |
| 変換後の `.glb` をコミットする | **不可**（同上） |
| 完成したゲームの実行ファイルに組み込んで配布 | 概ね可（ただし生の .glb を抜き出せる形はグレー） |

→ `.gitignore` で `/assets/models/external/` を除外済み。**変換後の .glb は必ずこのフォルダに置くこと。**
→ 公開する成果物（論文・リポジトリ・スクリーンショット以外の配布物）には CC0 素材のみを使うこと。

### パックの中身（棚卸し結果 2026-08-22）

| カテゴリ | 数 | 本プロジェクトでの用途 | 評価 |
|---|---|---|---|
| **Guns** | 51 | AK-47 / AWP / M4A4 / Glock 18 / Desert Eagle / P90 / M249 / RPG / Kar98k など**実在の現代銃** | ★★★★★ そのまま使える |
| **Modular Parts** | 1191 | 壁・柱・梁・アーチ・屋根・階段。マップの建築 | ★★★★★ L-09 の主役 |
| **Nature** | 751 | 木・茂み・岩・地面 | ★★★★ 植生の密度 |
| **City** | 501 | 街の小物 | ★★★★ |
| **Survival** | 172 | バレル・木箱・バックパック・ダッフルバッグ | ★★★★ FPS のカバー物 |
| **Movie Set** | 91 | 機材ケース・ケーブル・バリケード | ★★★ |
| **Terrain** | 24 | 丘・湖・川の 20×20m タイル | ★★★★ 高低差 |
| **People** | 79 | **全部ファンタジー／季節もの**（Soldier は中世甲冑・Ranger はファンタジー） | ★ **現代兵士は無い** |
| Textures | 290（PNG 257） | 4096×4096 RGBA8 アトラス（`Universal_A_Alb.png` 等）。**本エンジンの PNG デコーダでそのまま読める** | ★★★★★ |

**結論**: 武器・小物・建築・地形はこのパックで揃う。**足りないのは現代兵士のキャラクターとモーションだけ。**

### 変換手順（FBX → glTF）

本エンジンは glTF のみ対応。FBX は Blender で変換する（Blender 5.1 導入済みを確認）。

```bash
"C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" --background --python tools/fbx_to_glb.py -- --src "D:/プログラミング/Unity/unity-assets/Assets/polyperfect/Poly Universal Pack" --pattern "Meshes/Guns/SM_Ak_47.fbx" --inspect
```

`--inspect` を外し `--out assets/models/external --texsize 1024` を付けると変換されます。
**`--texsize` は必須級**: 元のアトラスは 4096×4096 で、1 枚あたり 67MB の VRAM を食います。
ローポリのアトラスなら 1024 に落としても見た目はほぼ変わりません。

**★テクスチャの自動推測は当たらないことが多い（要 `--override-tex`）★**: `tools/fbx_to_glb.py`
のマテリアル名からの自動推測（`M_Universal_A` → `Universal_A_Alb.png`）は、**FBX 自身が
持つマテリアル名がそもそも Unity 側の命名と一致していない**ため実際には機能しなかった。
`SM_Ak_47.fbx` を Blender にインポートすると、マテリアル名は Unity 側の `M_Universal_A`
ではなく汎用のプレースホルダ名 `Universal Shader V10` になる（Poly Universal Pack は
「メッシュ(FBX)は共通、色違いは Unity 側の差し替えマテリアルで表現」という構成のため、
どのアトラス(A〜F等)を使うかは FBX ではなく Unity の `.prefab`/`.mat` にしか書かれていない）。

実際に使うテクスチャを特定するには、Unity 側のファイルを手繰る必要がある:

```bash
# 1) prefab の m_Materials guid を調べる
grep -A1 "m_Materials:" "Prefabs/Guns/Ak_47.prefab"
# 2) その guid を持つ .mat.meta を探す（.mat 本体のファイル名が分かる）
grep -rl "<guid>" "Materials/"*.mat.meta
# 3) .mat の中の _MainTex guid を調べる
grep -A2 "_MainTex:" "Materials/M_Universal_A.mat"
# 4) その guid を持つ .png.meta を探す（実際のテクスチャファイル名が分かる）
grep -rl "<guid>" "Textures/Universal/"*.meta
```

`SM_Ak_47.fbx`（AK-47・`Ak_47.prefab` 経由）はこの手順で `Universal_A_Alb.png` と判明した。
変換コマンドに `--override-tex Universal_A_Alb` を追加して自動推測を上書きする:

```bash
"C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" --background --python tools/fbx_to_glb.py -- --src "D:/プログラミング/Unity/unity-assets/Assets/polyperfect/Poly Universal Pack" --pattern "Meshes/Guns/SM_Ak_47.fbx" --out assets/models/external --texsize 1024 --override-tex Universal_A_Alb
```

`gltf_test` で検証済み（2026-08-23）: primitive 4 / 頂点 4201 / 画像1（1024×1024・平均色
RGB(125,107,90)）/ サイズ 0.067 x 0.251 x 0.868m（実寸の AK-47 とほぼ一致）/ `[PASS]`。
**★複数マテリアルのアセットは `材質名=stem` 形式で個別指定する★**（ステージ「ワールド」で
建築モジュールを取り込む際に必要になり拡張した）。壁モジュールは 1 メッシュに 2 マテリアル
（トリム＝`M_Universal_A` / 壁面＝`M_Wall_Plaster_A`）を持つため、全マテリアルへ同じ
テクスチャを強制するとトリムの絵柄が壁面全体に貼られてしまう。`--override-tex` は複数回
指定でき、`材質名の一部=stem` で個別に割り当てられる（部分一致・大文字小文字無視、
指定順に照合）。`=` 無しの指定は「どれにも一致しなかった場合の既定」として働く:

```bash
"C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" --background --python tools/fbx_to_glb.py -- --src "D:/プログラミング/Unity/unity-assets/Assets/polyperfect/Poly Universal Pack" --pattern "Meshes/Modular Parts/Walls/**/SM_Wall_5x3m_Ext.fbx" --pattern "Meshes/Modular Parts/Walls/**/SM_Wall_Door_Regular_3x3m_Ext.fbx" --pattern "Meshes/Modular Parts/Walls/**/SM_Wall_Window_3x3m_Ext.fbx" --pattern "Meshes/Survival/**/SM_Barrell_Plain_A_Closed.fbx" --pattern "Meshes/City/**/SM_Container_City.fbx" --out assets/models/external --texsize 1024 --override-tex "Plaster=Wall_Plaster_A_Alb" --override-tex "Universal_A_Alb"
```

※ `--pattern` は `--src` からの相対 glob。パックは `Meshes/Modular Parts/Walls/3m/Walls Plain/`
のように深い階層なので **`**/` を挟まないとマッチしない**（`Meshes/Modular Parts/Walls/SM_*.fbx`
では 0 件になる）。

**★FBX の「壊れたテクスチャ参照」に注意★**: 壁モジュールの FBX は「テクスチャを参照して
いる」情報だけを持ち、実ファイルは同梱されていない。Blender にインポートすると *size 0 の
空の画像データブロック*ができ、`--override-tex` を指定しても「既にテクスチャがある」と
誤判定して割り当てをスキップしてしまっていた（結果：glTF に画像が出力されず壁面が真っ白）。
`tools/fbx_to_glb.py` 側で「画素を持っているか」まで確認し、空なら貼り直すよう修正済み。
加えて `名前=stem` の明示指定は既存テクスチャより優先するようにした。

### 取り込み済みの建築モジュール（2026-08-23）

`gltf_test` で検証済み。実寸はファイル名と一致する。**壁は原点が角**（`min(0,0,0)`〜
`max(w,h,d)`）で、置物は XZ 中心・底面 y=0 という違いがあるので配置時に補正が要る
（`World.cpp` の `kModuleSpecs` の `cornerOrigin` で扱っている）。

| .glb | 実寸 (W×H×D m) | primitive / 画像 | 原点 | 用途 |
|---|---|---|---|---|
| `SM_Wall_5x3m_Ext` | 5.000 × 3.005 × 0.100 | 2 / 2 | 角 | 外周壁・建物・遮蔽壁 |
| `SM_Wall_Door_Regular_3x3m_Ext` | 3.000 × 3.005 × 0.100 | 2 / 2 | 角 | 建物の入口（当たり判定に開口部を空ける） |
| `SM_Wall_Window_3x3m_Ext` | 3.000 × 3.005 × 0.100 | 2 / 2 | 角 | 射線を通す窓 |
| `SM_Barrell_Plain_A_Closed` | 0.646 × 0.900 × 0.679 | 1 / 1 | XZ中心 | 小さいカバー |
| `SM_Container_City` | 1.533 × 1.929 × 1.326 | 2 / 1 | XZ中心 | 腰高のカバー |

### 取り込み済みの公園モジュール（かくれんぼ用・2026-08-23）

かくれんぼを「公園／幼稚園」テーマにするため、City の遊具と Nature の植栽を追加した。
**テクスチャがアセット群で分かれている**（小物＝`Universal_A_Alb` / 茂み＝`Foliage_B` /
樹木＝葉 `Foliage_A` ＋ 幹 `Universal_A_Alb`）ので、3 バッチに分けて変換する:

```bash
# ① 公園小物（すべて Universal アトラス）
"C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" --background --python tools/fbx_to_glb.py -- --src "D:/プログラミング/Unity/unity-assets/Assets/polyperfect/Poly Universal Pack" --pattern "Meshes/Modular Parts/Fences/**/SM_Fence_Brick_3m_A.fbx" --pattern "Meshes/City/**/SM_Slide_Plastic_City.fbx" --pattern "Meshes/City/**/SM_Swing_City.fbx" --pattern "Meshes/City/**/SM_Horse_Playground_City.fbx" --pattern "Meshes/City/**/SM_Bench_A_City.fbx" --pattern "Meshes/City/**/SM_Table_A_City.fbx" --out assets/models/external --texsize 1024 --override-tex "Universal_A_Alb"
```

```bash
# ② 茂み（Foliage_B）
"C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" --background --python tools/fbx_to_glb.py -- --src "D:/プログラミング/Unity/unity-assets/Assets/polyperfect/Poly Universal Pack" --pattern "Meshes/Nature/**/SM_Bush_Round_Big.fbx" --pattern "Meshes/Nature/**/SM_Bush_Round_Medium.fbx" --out assets/models/external --texsize 1024 --override-tex "Foliage_B"
```

```bash
# ③ 樹木（葉と幹でテクスチャが違うので材質名で振り分ける）
"C:/Program Files/Blender Foundation/Blender 5.1/blender.exe" --background --python tools/fbx_to_glb.py -- --src "D:/プログラミング/Unity/unity-assets/Assets/polyperfect/Poly Universal Pack" --pattern "Meshes/Nature/**/SM_Tree_Birch_Mature_A.fbx" --out assets/models/external --texsize 1024 --override-tex "Foliage=Foliage_A" --override-tex "Universal_A_Alb"
```

| .glb | 実寸 (W×H×D m) | primitive / 画像 | 当たり判定 | 用途 |
|---|---|---|---|---|
| `SM_Fence_Brick_3m_A` | 3.300 × 3.450 × 0.600 | 1 / 1 | 実寸 | 園庭の外周 |
| `SM_Slide_Plastic_City` | 2.887 × 4.924 × 4.151 | 1 / 1 | 0.6倍 | 滑り台（視線を切る） |
| `SM_Swing_City` | 7.191 × 2.845 × 1.002 | 5 / 1 | **なし** | ブランコ（下をくぐれる） |
| `SM_Horse_Playground_City` | 0.381 × 1.188 × 1.146 | 1 / 1 | 実寸 | スプリング遊具 |
| `SM_Bench_A_City` | 2.005 × 0.716 × 0.419 | 1 / 1 | 実寸 | ベンチ |
| `SM_Table_A_City` | 2.246 × 1.066 × 2.246 | 1 / 1 | 実寸 | テーブル |
| `SM_Bush_Round_Big` | 3.437 × 2.720 × 3.070 | 1 / 1 | 0.6倍 | 大きい茂み（主要な隠れ場所） |
| `SM_Bush_Round_Medium` | 2.225 × 2.538 × 2.202 | 1 / 1 | 0.6倍 | 小さい茂み |
| `SM_Tree_Birch_Mature_A` | 3.848 × 11.575 × 3.876 | 2 / 2 | **0.2倍**（幹だけ） | 樹木 |

★**原点は .glb から自動で読み取る**★ 上表の「原点」欄を FPS 側の表のようには書いていない
のは、公園アセットの原点が中心でも角でもなく（植物は根が 1.3m 地面下から始まるものすら
ある）、手書きの表では管理しきれなかったため。`World::AddModule` がバウンディングから
「水平中心合わせ＋接地」を毎回計算する方式に変えた（詳細は ENGINE_GUIDE.md §20②）。

---

## 「もっとリアルなアセットにしたい」場合の手順

### 正直な現状認識

`soldier.glb` は手続き生成なので、**アーティストが作ったモデルには届きません**。
到達点は「ローポリだが人型として成立し、テクスチャで質感がある」までです。
2010年代 FPS 相当を狙うなら、**実際の CC0 アセットを持ってくるのが唯一の現実的な道**です。

幸い L-01 で glTF ローダーが入ったので、**`.glb` を置くだけで差し替わります**（コード変更不要）。

### 入手先（すべて CC0 または同等）

| サイト | 内容 | 形式 | 備考 |
|---|---|---|---|
| **Quaternius** (quaternius.com) | ローポリのキャラ・武器・環境 一式 | glTF / FBX / OBJ | CC0。**最も相性が良い**（軽量・スタイル統一・武器とキャラが揃う） |
| ┗ **Ultimate Modular Men/Characters Pack**（★開発者提示 2026-08-26） | 11キャラ×各4パーツ差替え＋**24アニメ内蔵** | glTF / FBX / OBJ / Blend | **CC0（"free to use in personal and commercial projects"）**。glTF＋スキン付きアニメ＝**本エンジンの L-04b スキニング経路にそのまま乗る**。★Swat.glb（Unity 由来＝コミット禁止）を置き換える最有力候補＝これなら **git にコミットできる**。URL: https://quaternius.com/packs/ultimatemodularcharacters.html |
| **Kenney** (kenney.nl) | ゲーム向け素材全般 | glTF / OBJ | CC0。武器・小物が豊富 |
| **Poly Haven** (polyhaven.com) | HDRI・PBR テクスチャ・モデル | glTF / EXR / PNG | CC0。L-07（IBL）でも使う |
| **Sketchfab**（CC0 フィルタ） | 高品質モデル | glTF | **ライセンス表示を必ず確認**。CC-BY は表記義務あり |
| **Mixamo** (mixamo.com) | キャラ＋モーションキャプチャ | FBX | 無償・要アカウント。**L-04（スキンドアニメ）で本領発揮** |

> ★Ultimate Modular Men Pack の使い方メモ（2026-08-26）★
> - Quaternius は glTF を直接配っているので、**FBX→Blender 変換すら不要**（`.glb` をそのまま
>   `gltf_test` に通して確認 → `game/World.cpp` の `kCandidates[]` 先頭に置くだけ）。
> - **CC0 なので `assets/models/external/` の .gitignore 除外は不要**。CC0 資産は `assets/models/` に
>   置いてコミットしてよい（出典を本ファイルに明記すること）。現行 Swat.glb（Unity Asset Store 由来＝
>   再配布禁止）と違い、これは公開リポジトリに入れられる。
> - 24 アニメ内蔵なので、敵/hider の Walk/Run/Idle 切替（既に実装済みの経路）がそのまま使える見込み。
>   ボーン名・クリップ名は取り込み後に `gltf_test -v` で確認し、`World::LoadGltfCharacter` の
>   `Play("Idle_Gun")→Play("Idle")→PlayIndex(0)` フォールバック鎖に合わせる（無ければ鎖に名前を足す）。
> - 「4パーツ差替えで11キャラ」= 敵と hider(子供役)で別の見た目を割り当てられる。ただし現行実装は
>   1 モデルを敵/hider で共有しているので、複数モデルを使うなら World 側に読み分けを足す（将来拡張）。

### 差し替え手順

1. `.glb`（GLB 形式・単一ファイル）をダウンロードする。
   `.gltf` + `.bin` + テクスチャの分割形式でも読めるが、`.glb` のほうが確実。
2. `assets/models/soldier.glb` として置く（既存を置き換えるか、名前を変えて
   `game/World.cpp` の `kCandidates[]` の先頭に足す）。
3. **先にローダー単体で確認する**:
   ```
   build\Debug\gltf_test.exe assets/models/soldier.glb -v
   ```
   - `[PASS]` が出れば読めている
   - サイズが極端なら（例: 高さ 100m）モデルの単位が違う。`World` 側でスケールを掛ける
   - `image[n]: デコード失敗` が出たらテクスチャが **JPEG**。PNG に変換して入れ直す
4. ゲームを起動して確認する。
5. **この CREDITS.md に出典とライセンスを 1 行追加する。**

### よくある引っかかり

| 症状 | 原因 | 対処 |
|---|---|---|
| `image[n]: デコード失敗` | テクスチャが JPEG | PNG に変換して `.glb` を作り直す（Blender で再エクスポートが早い） |
| モデルが巨大／極小 | 単位が cm や inch | Blender でスケールを適用してから書き出す |
| 真っ白／真っ黒 | PBR の metallic/roughness テクスチャしか無い | L-02（PBR）待ち。現状は baseColor のみ対応 |
| 横倒しになる | Z-up で作られている | Blender の glTF エクスポートは Y-up 変換をするので、正規手順で書き出せば直る |
| 読めない（スパースアクセサ） | 圧縮エクスポート設定 | Draco 圧縮を切って書き出す |
| ポーズが T ポーズのまま | スキン／アニメ未対応 | **L-04 で対応予定** |

### 現時点の未対応（アセット選びの制約）

- **JPEG テクスチャ** … PNG のみ対応（L-01b）
- **Draco 圧縮** … 未対応。エクスポート時にオフにする
- **スキン／アニメーション** … 未対応（L-04）。今は静止ポーズで表示される
- **法線マップ / ORM テクスチャ** … 未対応（L-02）。baseColor のみ反映される

## 制作ランタイムのモデル検証形状（2026-10-29）

`models/creator/tetrahedron.obj` と `split-platform.obj` は本プロジェクト用に生成した単純な幾何形状です。外部モデルは使用していません。CC0として使用できます。配布時は選択したモデルだけを `assets/model.mesh` へ変換します。

## 選択ビルド用の骨モデル（2026-10-29）

`models/creator/two-bone.glb`、`two-bone.fbx`、`two-bone-binary.fbx`は、このプロジェクトで作成した2ボーンの帯状モデルとSwing/Lift試験アニメーションです。CC0として提供します。外部ゲームの素材は含めません。生成ソースは`tools/advisor/model_fixtures.py`です。

FBXのビルド時変換にはufbx 0.23.1を使用します。ソース・ライセンス・固定版の記録は`third_party/ufbx/`に保存しています。ゲームパッケージにはufbxをリンクしません。
