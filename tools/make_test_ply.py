#!/usr/bin/env python3
# 3DGS .ply ローダー（GS-01）検証用の小さな .ply を生成する。
# Python 標準ライブラリのみ。graphdeco-inria/gaussian-splatting の save_ply と同じ
# プロパティ構成（SH degree 0 = f_rest 無し）で binary_little_endian を書く。
#
#   python tools/make_test_ply.py assets/models/test_splat.ply
#   build/Debug/ply_test.exe assets/models/test_splat.ply --dump 4
#
# 期待値（このスクリプトが決め打ちしている値）:
#   ガウシアン数 : 8（立方体の頂点に配置）
#   SH degree    : 0
#   バウンディング: min(-1, -1, -1) max(1, 1, 1) / サイズ 2 x 2 x 2
#   不透明度 α   : 全点 sigmoid(2.0) = 0.8808（範囲 [0.881, 0.881]）
#   平均スケール : exp(log(0.1)) = 0.1000
#   平均色 RGB   : (0.50, 0.50, 0.50) ← 8 点で各色が打ち消し合う
#   NaN を含む点 : 0  /  判定 [PASS]
import struct
import sys
import math

C0 = 0.28209479177387814

def dc_for(target):  # target(0..1) → f_dc raw
    return (target - 0.5) / C0

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test_splat.ply"

    # 立方体の 8 頂点。色は頂点位置の符号から R/G/B を決める（合計は灰色になる）。
    pts = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            for sz in (-1, 1):
                r = 1.0 if sx > 0 else 0.0
                g = 1.0 if sy > 0 else 0.0
                b = 1.0 if sz > 0 else 0.0
                pts.append((sx, sy, sz, r, g, b))

    props = (
        ["x", "y", "z", "nx", "ny", "nz",
         "f_dc_0", "f_dc_1", "f_dc_2",
         "opacity",
         "scale_0", "scale_1", "scale_2",
         "rot_0", "rot_1", "rot_2", "rot_3"]
    )
    header = "ply\nformat binary_little_endian 1.0\n"
    header += f"element vertex {len(pts)}\n"
    for p in props:
        header += f"property float {p}\n"
    header += "end_header\n"

    opacity_raw = 2.0            # sigmoid(2.0) = 0.8808
    scale_raw = math.log(0.1)   # exp -> 0.1

    with open(out, "wb") as f:
        f.write(header.encode("ascii"))
        for (x, y, z, r, g, b) in pts:
            row = [
                float(x), float(y), float(z),
                0.0, 0.0, 0.0,
                dc_for(r), dc_for(g), dc_for(b),
                opacity_raw,
                scale_raw, scale_raw, scale_raw,
                1.0, 0.0, 0.0, 0.0,   # rot_0=w=1（単位クォータニオン）
            ]
            f.write(struct.pack("<" + "f" * len(row), *row))

    print(f"wrote {out}  ({len(pts)} gaussians, SH degree 0)")

if __name__ == "__main__":
    main()
