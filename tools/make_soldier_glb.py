#!/usr/bin/env python3
"""ステージL-01b: テクスチャ付きの兵士＋ライフルの .glb を生成する（標準ライブラリのみ）。

位置づけ（正直に）:
  これは「箱の積み木」から一段上げるための手続き生成モデルであって、
  Synty や AAA のアーティスト製アセットには届かない。到達点は
  「ローポリだが人型として成立し、テクスチャで質感がある」まで。
  本物のクオリティが要るなら CC0 の .glb を assets/models/ に置いて
  読み込ませるのが正解（assets/CREDITS.md 参照）。

それでも今より良くなる理由:
  ・角柱ではなく「断面 12 角形のテーパー付き円柱」で手足・胴を作る
    → 法線を軸から放射状に振るのでローポリでも丸く見える
  ・関節ごとに太さを変える（肩→肘→手首、腿→膝→足首）
  ・UV を張り、迷彩・装備ベルト・ブーツを焼いたテクスチャを貼る
  ・ライフルを別ノードで持たせる

使い方:
  python tools/make_soldier_glb.py assets/models/soldier.glb
"""

import json
import math
import os
import struct
import sys
import zlib

# ===========================================================================
# メッシュビルダー
# ===========================================================================
class MeshBuilder:
    def __init__(self):
        self.pos = []
        self.nrm = []
        self.uv = []
        self.idx = []

    def _v(self, p, n, t):
        self.pos.append(p)
        self.nrm.append(n)
        self.uv.append(t)
        return len(self.pos) - 1

    def quad(self, a, b, c, d, n, uvs):
        i0 = self._v(a, n, uvs[0])
        i1 = self._v(b, n, uvs[1])
        i2 = self._v(c, n, uvs[2])
        i3 = self._v(d, n, uvs[3])
        self.idx += [i0, i1, i2, i0, i2, i3]

    def taper(self, center, r0, r1, height, sides=12, uv_rect=(0, 0, 1, 1), axis="y"):
        """テーパー付き円柱（下半径 r0・上半径 r1）。側面法線を軸から放射状にして丸く見せる。

        r0/r1 は (rx, rz) のタプルでも良い（楕円断面 = 胴や足を平たくできる）。
        """
        cx, cy, cz = center
        r0 = r0 if isinstance(r0, tuple) else (r0, r0)
        r1 = r1 if isinstance(r1, tuple) else (r1, r1)
        u0, v0, u1, v1 = uv_rect

        ring0, ring1 = [], []
        for i in range(sides + 1):
            a = 2.0 * math.pi * i / sides
            ca, sa = math.cos(a), math.sin(a)
            u = u0 + (u1 - u0) * (i / sides)
            # 側面法線: 楕円なので半径で割って正規化する
            nx, nz = ca / max(r0[0], 1e-4), sa / max(r0[1], 1e-4)
            ln = math.hypot(nx, nz) or 1.0
            # 上下で半径が違うぶん、法線を少し傾ける（テーパー面の向きに合わせる）
            slope = (r0[0] - r1[0]) / max(height, 1e-4)
            n = (nx / ln, slope * 0.5, nz / ln)
            nl = math.sqrt(sum(c * c for c in n)) or 1.0
            n = tuple(c / nl for c in n)
            ring0.append(((cx + ca * r0[0], cy, cz + sa * r0[1]), n, (u, v1)))
            ring1.append(((cx + ca * r1[0], cy + height, cz + sa * r1[1]), n, (u, v0)))

        for i in range(sides):
            p0, n0, t0 = ring0[i]
            p1, n1, t1 = ring0[i + 1]
            p2, n2, t2 = ring1[i + 1]
            p3, n3, t3 = ring1[i]
            i0 = self._v(p0, n0, t0)
            i1 = self._v(p1, n1, t1)
            i2 = self._v(p2, n2, t2)
            i3 = self._v(p3, n3, t3)
            self.idx += [i0, i1, i2, i0, i2, i3]

        # 上下の蓋（三角ファン）
        for ring, y, ny, r in ((ring0, cy, -1.0, r0), (ring1, cy + height, 1.0, r1)):
            if max(r) < 1e-4:
                continue
            cidx = self._v((cx, y, cz), (0.0, ny, 0.0), ((u0 + u1) / 2, (v0 + v1) / 2))
            for i in range(sides):
                a = self._v(ring[i][0], (0.0, ny, 0.0), ring[i][2])
                b = self._v(ring[i + 1][0], (0.0, ny, 0.0), ring[i + 1][2])
                self.idx += ([cidx, a, b] if ny > 0 else [cidx, b, a])

    def box(self, center, half, uv_rect=(0, 0, 1, 1)):
        cx, cy, cz = center
        hx, hy, hz = half
        u0, v0, u1, v1 = uv_rect
        uvs = [(u0, v1), (u1, v1), (u1, v0), (u0, v0)]
        c = lambda sx, sy, sz: (cx + sx * hx, cy + sy * hy, cz + sz * hz)
        self.quad(c(-1, -1, -1), c(1, -1, -1), c(1, 1, -1), c(-1, 1, -1), (0, 0, -1), uvs)
        self.quad(c(1, -1, 1), c(-1, -1, 1), c(-1, 1, 1), c(1, 1, 1), (0, 0, 1), uvs)
        self.quad(c(-1, -1, 1), c(-1, -1, -1), c(-1, 1, -1), c(-1, 1, 1), (-1, 0, 0), uvs)
        self.quad(c(1, -1, -1), c(1, -1, 1), c(1, 1, 1), c(1, 1, -1), (1, 0, 0), uvs)
        self.quad(c(-1, 1, -1), c(1, 1, -1), c(1, 1, 1), c(-1, 1, 1), (0, 1, 0), uvs)
        self.quad(c(-1, -1, 1), c(1, -1, 1), c(1, -1, -1), c(-1, -1, -1), (0, -1, 0), uvs)

    def sphere(self, center, radius, rings=8, sides=12, uv_rect=(0, 0, 1, 1)):
        """低ポリ球（頭に使う）。法線は中心からの方向＝完全に滑らか。"""
        cx, cy, cz = center
        u0, v0, u1, v1 = uv_rect
        grid = []
        for r in range(rings + 1):
            phi = math.pi * r / rings
            row = []
            for s in range(sides + 1):
                th = 2.0 * math.pi * s / sides
                n = (math.sin(phi) * math.cos(th), math.cos(phi), math.sin(phi) * math.sin(th))
                p = (cx + n[0] * radius, cy + n[1] * radius, cz + n[2] * radius)
                t = (u0 + (u1 - u0) * s / sides, v0 + (v1 - v0) * r / rings)
                row.append((p, n, t))
            grid.append(row)
        for r in range(rings):
            for s in range(sides):
                a, b = grid[r][s], grid[r][s + 1]
                c, d = grid[r + 1][s + 1], grid[r + 1][s]
                i0 = self._v(*a); i1 = self._v(*b); i2 = self._v(*c); i3 = self._v(*d)
                self.idx += [i0, i1, i2, i0, i2, i3]


# ===========================================================================
# テクスチャ（手続き生成 → PNG）
# ===========================================================================
def rnd(seed):
    """再現性のある簡易乱数（同じシードなら毎回同じテクスチャになる）。"""
    s = seed & 0xFFFFFFFF
    while True:
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        yield (s & 0xFFFF) / 65535.0


def make_soldier_texture(w=256, h=256):
    """UV レイアウト（縦4分割）に合わせた 1 枚のテクスチャを描く。
         v 0.00-0.25 : 胴（迷彩＋装備ベルト）
         v 0.25-0.50 : 腕・脚（迷彩・やや暗め）
         v 0.50-0.75 : 顔・肌
         v 0.75-1.00 : 装備（ブーツ・銃・ヘルメット＝暗いグレー）
    """
    g = rnd(0xC0FFEE)
    px = bytearray(w * h * 4)

    def put(x, y, r, gg, b, a=255):
        o = (y * w + x) * 4
        px[o] = r; px[o + 1] = gg; px[o + 2] = b; px[o + 3] = a

    # 迷彩のブロブを打つためのパレット
    camo = [(74, 84, 60), (56, 64, 46), (92, 98, 74), (44, 48, 38)]
    for y in range(h):
        for x in range(w):
            band = y * 4 // h
            if band == 0 or band == 1:
                base = camo[0]
                dark = 1.0 if band == 0 else 0.82  # 手足は少し暗く
                put(x, y, int(base[0] * dark), int(base[1] * dark), int(base[2] * dark))
            elif band == 2:
                put(x, y, 214, 170, 138)  # 肌
            else:
                put(x, y, 46, 46, 50)     # 装備

    # 迷彩ブロブ（楕円をランダムに重ねる）
    for _ in range(220):
        cx = int(next(g) * w)
        cy = int(next(g) * h * 0.5)          # 迷彩は上半分（胴・手足）のみ
        rx = 6 + int(next(g) * 22)
        ry = 4 + int(next(g) * 16)
        col = camo[1 + int(next(g) * 2.99)]
        for y in range(max(0, cy - ry), min(int(h * 0.5), cy + ry)):
            for x in range(max(0, cx - rx), min(w, cx + rx)):
                dx = (x - cx) / rx
                dy = (y - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    dark = 1.0 if y < h * 0.25 else 0.82
                    put(x, y, int(col[0] * dark), int(col[1] * dark), int(col[2] * dark))

    # 胴の装備ベルト（横の暗い帯）＋ポーチ
    for y in range(int(h * 0.15), int(h * 0.19)):
        for x in range(w):
            put(x, y, 38, 34, 30)
    for i in range(4):
        x0 = int(w * (0.08 + i * 0.24))
        for y in range(int(h * 0.13), int(h * 0.21)):
            for x in range(x0, min(w, x0 + int(w * 0.10))):
                put(x, y, 52, 46, 38)

    # 顔（目のライン）
    for y in range(int(h * 0.57), int(h * 0.60)):
        for x in range(int(w * 0.20), int(w * 0.80)):
            put(x, y, 60, 45, 40)

    # 装備帯にハイライトの筋（金属感）
    for y in range(int(h * 0.75), h):
        for x in range(w):
            n = next(g)
            if n > 0.93:
                o = (y * w + x) * 4
                px[o] = min(255, px[o] + 40)
                px[o + 1] = min(255, px[o + 1] + 40)
                px[o + 2] = min(255, px[o + 2] + 45)

    # 全体に微細ノイズ（のっぺり感を消す）
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 4
            d = int((next(g) - 0.5) * 18)
            px[o] = max(0, min(255, px[o] + d))
            px[o + 1] = max(0, min(255, px[o + 1] + d))
            px[o + 2] = max(0, min(255, px[o + 2] + d))

    return bytes(px), w, h


def encode_png(rgba, w, h):
    """RGBA8 を PNG にエンコードする（zlib は標準ライブラリ）。
    ★これが C++ 側 PngLoader の検証対象になる★
      Python の zlib は動的ハフマンを使うので、自作 inflate の動的ハフマン経路が
      実データで検証されることになる。
    """
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # フィルタ種別 0 (None)
        raw += rgba[y * w * 4:(y + 1) * w * 4]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8bit RGBA, 非インターレース
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


# ===========================================================================
# GLB 出力
# ===========================================================================
class GlbBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.views = []
        self.accessors = []

    def _align4(self):
        while len(self.bin) % 4:
            self.bin.append(0)

    def view(self, data, target=None):
        self._align4()
        off = len(self.bin)
        self.bin += data
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        self.views.append(v)
        return len(self.views) - 1

    def vec(self, values, comps):
        flat = [c for v in values for c in v]
        vi = self.view(struct.pack("<%df" % len(flat), *flat), 34962)
        acc = {"bufferView": vi, "componentType": 5126, "count": len(values),
               "type": {2: "VEC2", 3: "VEC3"}[comps]}
        if comps == 3:
            acc["min"] = [min(v[i] for v in values) for i in range(3)]
            acc["max"] = [max(v[i] for v in values) for i in range(3)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def indices(self, idx):
        # 頂点数が 65535 を超えたら UNSIGNED_INT にする（ローダーの両方の経路を通す）
        if max(idx) < 65536:
            data, ct = struct.pack("<%dH" % len(idx), *idx), 5123
        else:
            data, ct = struct.pack("<%dI" % len(idx), *idx), 5125
        vi = self.view(data, 34963)
        self.accessors.append({"bufferView": vi, "componentType": ct,
                               "count": len(idx), "type": "SCALAR"})
        return len(self.accessors) - 1


# UV レイアウトの帯（テクスチャ側と対応させる）
UV_TORSO = (0.02, 0.02, 0.98, 0.24)
UV_LIMB  = (0.02, 0.27, 0.98, 0.48)
UV_SKIN  = (0.02, 0.52, 0.98, 0.73)
UV_GEAR  = (0.02, 0.77, 0.98, 0.98)


def build_soldier():
    """兵士のメッシュを組み立てる。原点は足元、+Z が正面。"""
    m = MeshBuilder()

    # --- 脚（腿→膝→足首でテーパー）---
    for side in (-1, 1):
        x = 0.115 * side
        m.taper((x, 0.06, 0.0), (0.105, 0.105), (0.075, 0.080), 0.42, uv_rect=UV_LIMB)  # 腿
        m.taper((x, 0.48, 0.0), (0.078, 0.082), (0.055, 0.060), 0.40, uv_rect=UV_LIMB)  # 脛
        m.box((x, 0.04, 0.03), (0.075, 0.045, 0.115), uv_rect=UV_GEAR)                   # ブーツ

    # --- 胴（腰→胸で太くなり、肩で少し絞る。楕円断面で厚みを抑える）---
    m.taper((0, 0.86, 0), (0.155, 0.105), (0.185, 0.125), 0.30, uv_rect=UV_TORSO)  # 腰→胸
    m.taper((0, 1.16, 0), (0.185, 0.125), (0.170, 0.115), 0.16, uv_rect=UV_TORSO)  # 胸→肩
    # 防弾ベスト（胴より少し大きい箱を重ねる）
    m.box((0, 1.10, 0.0), (0.175, 0.19, 0.125), uv_rect=UV_TORSO)

    # --- 首・頭・ヘルメット ---
    m.taper((0, 1.30, 0), 0.055, 0.055, 0.06, sides=8, uv_rect=UV_SKIN)
    m.sphere((0, 1.44, 0.0), 0.105, rings=8, sides=12, uv_rect=UV_SKIN)
    # ヘルメット（球の上半分を少し大きく重ねる）
    m.sphere((0, 1.46, 0.0), 0.122, rings=6, sides=12, uv_rect=UV_GEAR)

    # --- 腕（肩→肘→手首）---
    for side in (-1, 1):
        x = 0.215 * side
        m.taper((x, 1.05, 0.0), (0.062, 0.062), (0.050, 0.050), 0.24, uv_rect=UV_LIMB)  # 上腕
        m.taper((x, 0.81, 0.0), (0.050, 0.050), (0.042, 0.042), 0.24, uv_rect=UV_LIMB)  # 前腕
        m.box((x, 0.76, 0.0), (0.048, 0.05, 0.045), uv_rect=UV_SKIN)                     # 手
    return m


def build_rifle():
    """ライフル。原点はグリップ付近、+Z が銃口方向。"""
    m = MeshBuilder()
    m.box((0, 0.0, 0.16), (0.028, 0.055, 0.26), uv_rect=UV_GEAR)     # レシーバー（本体）
    m.taper((0, 0.0, 0.42), 0.016, 0.014, 0.001, sides=8, uv_rect=UV_GEAR)
    # 銃身（Z 方向に伸ばしたいので、円柱を作ってから頂点を回す）
    barrel = MeshBuilder()
    barrel.taper((0, 0, 0), 0.017, 0.015, 0.34, sides=10, uv_rect=UV_GEAR)
    for p, n, t in zip(barrel.pos, barrel.nrm, barrel.uv):
        # Y 軸方向の円柱を Z 方向へ倒す: (x, y, z) → (x, z, y)
        m._v((p[0], p[2] + 0.012, p[1] + 0.40), (n[0], n[2], n[1]), t)
    base = len(m.pos) - len(barrel.pos)
    m.idx += [base + i for i in barrel.idx]

    m.box((0, -0.13, 0.02), (0.026, 0.085, 0.05), uv_rect=UV_GEAR)   # グリップ
    m.box((0, -0.02, -0.16), (0.024, 0.048, 0.14), uv_rect=UV_GEAR)  # ストック
    m.box((0, -0.09, 0.16), (0.020, 0.075, 0.035), uv_rect=UV_GEAR)  # マガジン
    m.box((0, 0.072, 0.30), (0.010, 0.022, 0.02), uv_rect=UV_GEAR)   # フロントサイト
    m.box((0, 0.068, -0.02), (0.010, 0.018, 0.03), uv_rect=UV_GEAR)  # リアサイト
    return m


def build(path):
    b = GlbBuilder()

    tex_rgba, tw, th = make_soldier_texture()
    png = encode_png(tex_rgba, tw, th)
    png_view = b.view(png)

    parts = [("soldier", build_soldier()), ("rifle", build_rifle())]
    meshes, nodes = [], []
    for name, m in parts:
        prim = {
            "attributes": {
                "POSITION": b.vec(m.pos, 3),
                "NORMAL": b.vec(m.nrm, 3),
                "TEXCOORD_0": b.vec(m.uv, 2),
            },
            "indices": b.indices(m.idx),
            "material": 0,
            "mode": 4,
        }
        meshes.append({"name": name, "primitives": [prim]})

    # ライフルの構え位置（右手のあたり）
    RIFLE_T = (0.205, 0.90, 0.20)

    # ★接地とスケールを合わせる★
    # 手続き生成そのままだと足元が原点からずれ（ブーツが地面に埋まる/浮く）、
    # 身長もばらつく。ノード変換後の実寸を測ってから、ルートノードで
    #   ①身長 1.80m になるよう一様スケール
    #   ②min.y がちょうど 0 になるよう平行移動
    # を掛ける。こうしておくとゲーム側は「置きたい座標へ平行移動」するだけで済む。
    all_pts = [p for p in parts[0][1].pos]
    all_pts += [(p[0] + RIFLE_T[0], p[1] + RIFLE_T[1], p[2] + RIFLE_T[2])
                for p in parts[1][1].pos]
    ymin = min(p[1] for p in all_pts)
    ymax = max(p[1] for p in all_pts)
    scale = 1.80 / (ymax - ymin)

    nodes.append({"name": "soldier", "mesh": 0})
    nodes.append({"name": "rifle", "mesh": 1, "translation": list(RIFLE_T)})
    nodes.append({"name": "root", "children": [0, 1],
                  "scale": [scale, scale, scale],
                  "translation": [0.0, -ymin * scale, 0.0]})

    b._align4()
    gltf = {
        "asset": {"version": "2.0", "generator": "make_soldier_glb.py (GameEngine L-01b)"},
        "scene": 0,
        "scenes": [{"nodes": [2]}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": [{
            "name": "soldier_mat",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "baseColorFactor": [1, 1, 1, 1],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.85,
            },
            "doubleSided": True,
        }],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}],
        "images": [{"bufferView": png_view, "mimeType": "image/png", "name": "soldier_tex"}],
        "accessors": b.accessors,
        "bufferViews": b.views,
        "buffers": [{"byteLength": len(b.bin)}],
    }

    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    bn = bytes(b.bin)
    while len(bn) % 4:
        bn += b"\x00"

    total = 12 + 8 + len(js) + 8 + len(bn)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(js), 0x4E4F534A) + js
    out += struct.pack("<II", len(bn), 0x004E4942) + bn

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)

    nv = sum(len(m.pos) for _, m in parts)
    ni = sum(len(m.idx) for _, m in parts)
    # ルートノードの変換を適用した最終寸法（＝C++ ローダーが出すはずの値）
    fin = [(p[0] * scale, (p[1] - ymin) * scale, p[2] * scale) for p in all_pts]
    xs = [p[0] for p in fin]; ys = [p[1] for p in fin]; zs = [p[2] for p in fin]
    print("生成: %s (%d バイト)" % (path, len(out)))
    print("  primitive 2 / 頂点 %d / インデックス %d (三角形 %d)" % (nv, ni, ni // 3))
    print("  テクスチャ %dx%d PNG (%d バイト・zlib 圧縮)" % (tw, th, len(png)))
    print("  期待される読み込み結果:")
    print("    bounds min (%.3f, %.3f, %.3f)" % (min(xs), min(ys), min(zs)))
    print("    bounds max (%.3f, %.3f, %.3f)" % (max(xs), max(ys), max(zs)))
    print("    size %.3f x %.3f x %.3f" % (max(xs)-min(xs), max(ys)-min(ys), max(zs)-min(zs)))


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "assets/models/soldier.glb")
