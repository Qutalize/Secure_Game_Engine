#!/usr/bin/env python3
"""ステージL-01a: glTF ローダー検証用の .glb を生成する（依存なし・標準ライブラリのみ）。

なぜ自前生成するのか:
  ・外部からアセットを落とさずに、すぐ動かせるテスト対象が要る（著作権も不要）。
  ・ローダーが正しいかを判定するには「中身が既知」のモデルが一番よい。
  ・ノード階層 / TRS 変換 / 複数 primitive / UNSIGNED_SHORT インデックスといった
    ローダーが間違えやすい要素を意図的に盛り込んである。

生成されるもの: 非対称な低ポリの人型っぽい形（向きの間違いに気付けるよう左右非対称にしてある）
  ルートノード（Y 回転 20 度）
    ├ 胴（青） …………………… ノードの translation で持ち上げ
    ├ 頭（肌色） ……………………子ノード。親の回転も効く＝階層合成の検証
    ├ 右腕（緑）………………… scale を使う＝TRS の S の検証
    └ 左足（赤・前に出す）…… 左右非対称＝向きの検証

使い方:
  python tools/make_test_glb.py assets/models/test_figure.glb
"""

import json
import struct
import sys
import os

# ---------------------------------------------------------------------------
# 立方体メッシュ（面ごとに 4 頂点 = 法線が面で正しく分かれる）
# ---------------------------------------------------------------------------
def make_box(sx, sy, sz):
    """中心原点・サイズ (sx,sy,sz) の箱を返す。(positions, normals, uvs, indices)"""
    hx, hy, hz = sx / 2.0, sy / 2.0, sz / 2.0
    # (法線, その面の 4 頂点) を並べる
    faces = [
        ((0, 0, -1), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0, 0,  1), [(hx, -hy,  hz), (-hx, -hy,  hz), (-hx, hy,  hz), (hx, hy,  hz)]),
        ((-1, 0, 0), [(-hx, -hy,  hz), (-hx, -hy, -hz), (-hx, hy, -hz), (-hx, hy,  hz)]),
        (( 1, 0, 0), [(hx, -hy, -hz), (hx, -hy,  hz), (hx, hy,  hz), (hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy,  hz), (hx, -hy,  hz), (hx, -hy, -hz), (-hx, -hy, -hz)]),
        ((0,  1, 0), [(-hx,  hy, -hz), (hx,  hy, -hz), (hx,  hy,  hz), (-hx,  hy,  hz)]),
    ]
    positions, normals, uvs, indices = [], [], [], []
    for normal, quad in faces:
        base = len(positions)
        for k, v in enumerate(quad):
            positions.append(v)
            normals.append(normal)
            uvs.append([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)][k])
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


class GlbBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.buffer_views = []
        self.accessors = []

    def _align4(self):
        while len(self.bin) % 4 != 0:
            self.bin.append(0)

    def _add_view(self, data, target):
        self._align4()
        offset = len(self.bin)
        self.bin += data
        self.buffer_views.append(
            {"buffer": 0, "byteOffset": offset, "byteLength": len(data), "target": target}
        )
        return len(self.buffer_views) - 1

    def add_vec(self, values, comps):
        """float の VEC2/VEC3 アクセサを追加して index を返す。"""
        flat = [c for v in values for c in v]
        data = struct.pack("<%df" % len(flat), *flat)
        view = self._add_view(data, 34962)  # ARRAY_BUFFER
        acc = {
            "bufferView": view,
            "componentType": 5126,  # FLOAT
            "count": len(values),
            "type": {2: "VEC2", 3: "VEC3"}[comps],
        }
        # POSITION アクセサには min/max が必須（glTF 仕様）
        if comps == 3:
            acc["min"] = [min(v[i] for v in values) for i in range(3)]
            acc["max"] = [max(v[i] for v in values) for i in range(3)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def add_indices(self, indices):
        """UNSIGNED_SHORT のインデックスアクセサ（ローダーの型変換を検証させる）。"""
        data = struct.pack("<%dH" % len(indices), *indices)
        view = self._add_view(data, 34963)  # ELEMENT_ARRAY_BUFFER
        self.accessors.append(
            {"bufferView": view, "componentType": 5123, "count": len(indices), "type": "SCALAR"}
        )
        return len(self.accessors) - 1


def build(path):
    b = GlbBuilder()
    meshes, materials, nodes = [], [], []

    # (名前, サイズ, 位置, 回転(quat), スケール, 色)
    parts = [
        ("torso", (0.6, 0.9, 0.35), (0.0, 1.05, 0.0),   None,            None,              [0.20, 0.35, 0.75, 1.0]),
        ("head",  (0.42, 0.42, 0.42), (0.0, 0.70, 0.0), None,            None,              [0.95, 0.78, 0.62, 1.0]),
        ("armR",  (0.18, 0.6, 0.18), (0.42, 1.15, 0.0), None,            (1.0, 1.15, 1.0),  [0.25, 0.65, 0.30, 1.0]),
        ("legL",  (0.22, 0.7, 0.22), (-0.17, 0.35, 0.18), None,          None,              [0.75, 0.22, 0.22, 1.0]),
    ]

    for name, size, pos, rot, scale, color in parts:
        positions, normals, uvs, indices = make_box(*size)
        prim = {
            "attributes": {
                "POSITION": b.add_vec(positions, 3),
                "NORMAL": b.add_vec(normals, 3),
                "TEXCOORD_0": b.add_vec(uvs, 2),
            },
            "indices": b.add_indices(indices),
            "material": len(materials),
            "mode": 4,
        }
        materials.append(
            {
                "name": name + "_mat",
                "pbrMetallicRoughness": {
                    "baseColorFactor": color,
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.8,
                },
            }
        )
        meshes.append({"name": name, "primitives": [prim]})
        node = {"name": name, "mesh": len(meshes) - 1, "translation": list(pos)}
        if rot:
            node["rotation"] = list(rot)
        if scale:
            node["scale"] = list(scale)
        nodes.append(node)

    # head を torso の子にする＝親の変換が効くことを検証させる。
    # （head の translation は torso からの相対位置になる）
    nodes[0]["children"] = [1]

    # ルートノード: Y 軸まわりに 20 度回転（quaternion）。
    # ローダーが回転を無視していたら向きで気付ける。
    import math
    a = math.radians(20.0) * 0.5
    root = {
        "name": "root",
        "rotation": [0.0, math.sin(a), 0.0, math.cos(a)],
        "children": [0, 2, 3],
    }
    nodes.append(root)
    root_index = len(nodes) - 1

    b._align4()
    gltf = {
        "asset": {"version": "2.0", "generator": "make_test_glb.py (GameEngine L-01a)"},
        "scene": 0,
        "scenes": [{"nodes": [root_index]}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": b.accessors,
        "bufferViews": b.buffer_views,
        "buffers": [{"byteLength": len(b.bin)}],
    }

    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4 != 0:
        json_bytes += b" "          # JSON チャンクはスペースでパディング
    bin_bytes = bytes(b.bin)
    while len(bin_bytes) % 4 != 0:
        bin_bytes += b"\x00"        # BIN チャンクはゼロでパディング

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    out = bytearray()
    out += struct.pack("<III", 0x46546C67, 2, total)          # 'glTF', version 2, 全体長
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A)    # 'JSON'
    out += json_bytes
    out += struct.pack("<II", len(bin_bytes), 0x004E4942)     # 'BIN\0'
    out += bin_bytes

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)

    print("生成: %s (%d バイト)" % (path, len(out)))
    print("  ノード %d / メッシュ %d / マテリアル %d / アクセサ %d"
          % (len(nodes), len(meshes), len(materials), len(b.accessors)))
    print("  期待される読み込み結果: primitive 4 個 / 頂点 96 / インデックス 144（三角形 48）")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/models/test_figure.glb"
    build(out)
