# ガラスと水の光学試作

自作DirectX 12ランタイムのフルスクリーン描画パスで、傾けたガラス板越しに縞模様を見る固定シーンを描きます。標準のゲームシーンやGLB／FBXの材質設定への組み込みとは分けた、ポスター②の実験です。

## 動かす

プロジェクトルートで、保存済みの未署名パッケージ output/packages/selected-rt-software-v1 を入力にします。Windowsの標準シェーダーコンパイラで、試作用出力のシェーダーだけを用意します。出力先には新しいディレクトリを指定してください。

    python tools/poster/glass_study/build.py output/poster-demos/my-glass
    cd output/poster-demos/my-glass
    .\SelectedGame.exe

検証済みの標準設定は output/poster-demos/glass-ior150 にあります。対照として --ior 1 --clear（空気と同じ屈折率、吸収なし）、--ior 1.5 --clear（無色ガラス）、--no-plate --clear（板なし）も選べます。

## 測定条件

- 厚さ0.55 m、中央で入射角48°、空気の屈折率1.0、ガラスの屈折率1.5。
- 各境界でスネルの法則に沿って透過方向を計算し、非偏光のフレネル反射率で前面反射と直達透過を合成。内部を進む距離にBeer–Lambertの吸収を適用します。
- η=1.5では中央の屈折角が約29.7°。960×540の中央水平走査線でオレンジ帯の左端がx=410からx=428へ移動し、独立した計算と一致しました。
- これは平行な二面を持ち、外周を不透明な枠が囲む一枚の板のモデルです。内部多重反射や集光模様（コースティクス）、任意形状の透明メッシュは今回の計算対象外です。

実機のクライアント領域をWindows Graphics Captureで保存した画像と試験結果は poster_assets/glass_study/ にあります。--clear と --no-plate を組み合わせた対照を含め、下記で再検査できます。

    python tools/poster/glass_study/verify.py poster_assets/glass_study

## 金属球に箱が映る部屋

写真①は自作DirectX 12ランタイムの固定シーンです。赤い箱、青い箱、床と壁を視点から描き、球面上ではその法線で反射方向を求め、同じ部屋を1回追跡します。映り込みには鏡面方向を使い、ランプの光沢にはGGXの直接光を使います。実行例は `output/poster-demos/metal-sphere/SelectedGame.exe` です。次のコマンドで、新しい出力先に通常色・交換色をそれぞれビルドできます。

    python tools/poster/glass_study/build.py output/poster-demos/my-metal --scene metal
    python tools/poster/glass_study/build.py output/poster-demos/my-metal-control --scene metal --swap-box-colors

`poster_assets/metal_study/` の `normal.png` と `swapped.png` は、同じカメラと球、同じ位置の箱を使った960×540のゲーム実画面です。違いは二つの箱の色だけです。原画の球面上で、左側の (380, 266) は赤から青、右側の (581, 280) は青から赤に変わりました。測定値・ハッシュ・撮影条件は `results.json` にあります。

## ストローで屈折を見せる

ポスター②の写真③では、水を入れた円筒形のコップに一本のまっすぐな赤いストローを斜めに差しています。右の屈折像では、水面の近くでストローがずれて見えます。空気（屈折率1.0）、ガラス（1.5）、水（1.333）の境界でスネルの法則を使って光を曲げ、反射と透過の割合をフレネル反射率で求めます。透過は最大12境界まで追跡し、反射色はスタジオ環境で近似しています。

検証済みの実行ファイルは `output/poster-demos/straw-water-final/SelectedGame.exe` です。再ビルドする場合は、それぞれ未使用の出力先を指定してください。

    python tools/poster/glass_study/build.py output/poster-demos/my-straw --scene straw
    python tools/poster/glass_study/build.py output/poster-demos/my-straw-control --scene straw --straight-rays

左の対照では、コップ・水・ストロー・カメラを同じ位置に置き、界面を通る光線の向きだけを直進のままにしています。右の短い破線は、まっすぐなストローの中心軸をカメラに投影した位置です。Windows Graphics Captureで撮影した960×540の原画は `poster_assets/straw_study/` にあります。左上を原点に数えた y=75 では赤い帯の中心が左右とも x=622.5。水中の y=360 では x=523.5 から 563.5 へ、右に40 pxずれます。印刷用は両方を同じ範囲・倍率で切り出しています。

    python tools/poster/glass_study/verify_straw.py poster_assets/straw_study

## 資料

- [様々なレンダリング方式（指定資料）](https://blender3d.biz/knowledge_rendering_renderingmethods.html)
- [PBRT 第4版 9.3　Specular Reflection and Transmission](https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission)
- [Cook & Torrance　A Reflectance Model for Computer Graphics](https://graphics.pixar.com/library/ReflectanceModel/paper.pdf)
- [Walter et al.　Microfacet Models for Refraction through Rough Surfaces](https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.pdf)
