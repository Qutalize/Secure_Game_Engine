#!/usr/bin/env python3
"""FBX バイナリの中身を Blender 無しで調べる（標準ライブラリのみ）。

なぜ作ったか:
  ダウンロードしたアセットが「使えるか」を判断するには
    ・スキン（ボーン）を持っているか  → アニメーションを当てられるか
    ・アニメーションが入っているか    → 何クリップ・何フレームか
    ・メッシュの規模・テクスチャ参照
  を知る必要がある。Blender を起動すれば分かるが、起動に時間がかかるうえ
  失敗の切り分けもしにくい。FBX バイナリのノード構造は素直なので、
  必要な情報だけ抜き出す軽量プローブを用意しておく。

FBX バイナリの構造（バージョン 7.x）:
  [27B ヘッダ] "Kaydara FBX Binary  \\x00" + 2B + バージョン(u32)
  以降はノードの入れ子。1 ノードは
    EndOffset / NumProperties / PropertyListLen  （7500 未満は u32、以上は u64）
    NameLen(u8) + Name
    プロパティ列（型 1 文字 + データ）
    子ノード列（全ゼロのレコードで終端）

使い方:
  python tools/fbx_probe.py <file.fbx> [file2.fbx ...]
  python tools/fbx_probe.py <file.fbx> --tree     ノード木を表示（デバッグ用）
"""

import struct
import sys
import zlib
from collections import Counter, defaultdict


class FbxReader:
    def __init__(self, data):
        self.d = data
        self.version = struct.unpack_from("<I", data, 23)[0]
        # 7500 以降はオフセットが 64bit になる
        self.wide = self.version >= 7500
        self.wsz = 8 if self.wide else 4
        self.wfmt = "<Q" if self.wide else "<I"

    def read_props(self, pos, count):
        """プロパティを読む。欲しいのは主に文字列と数値なので、配列は長さだけ拾う。"""
        out = []
        for _ in range(count):
            t = chr(self.d[pos]); pos += 1
            if t == "Y":
                out.append(struct.unpack_from("<h", self.d, pos)[0]); pos += 2
            elif t == "C":
                out.append(bool(self.d[pos])); pos += 1
            elif t == "I":
                out.append(struct.unpack_from("<i", self.d, pos)[0]); pos += 4
            elif t == "F":
                out.append(struct.unpack_from("<f", self.d, pos)[0]); pos += 4
            elif t == "D":
                out.append(struct.unpack_from("<d", self.d, pos)[0]); pos += 8
            elif t == "L":
                out.append(struct.unpack_from("<q", self.d, pos)[0]); pos += 8
            elif t in "fdlib":
                n, enc, clen = struct.unpack_from("<III", self.d, pos); pos += 12
                out.append(("array:" + t, n))   # 中身は使わないので長さだけ
                pos += clen
            elif t in "SR":
                n = struct.unpack_from("<I", self.d, pos)[0]; pos += 4
                raw = self.d[pos:pos + n]; pos += n
                if t == "S":
                    # FBX の名前は "Name\\x00\\x01Class" 形式のことがある
                    out.append(raw.split(b"\x00\x01")[0].decode("utf-8", "replace"))
                else:
                    out.append(("raw", n))
            else:
                raise ValueError("未知のプロパティ型 %r at %d" % (t, pos - 1))
        return out, pos

    def walk(self, pos, end, depth, visit):
        """ノードを再帰的に辿り、visit(name, props, depth) を呼ぶ。"""
        while pos < end:
            hdr = pos
            end_off, num_props, prop_len = struct.unpack_from(
                self.wfmt + self.wfmt[1:] * 0, self.d, pos)[0], 0, 0
            # 3 つまとめて読む
            end_off = struct.unpack_from(self.wfmt, self.d, pos)[0]
            num_props = struct.unpack_from(self.wfmt, self.d, pos + self.wsz)[0]
            prop_len = struct.unpack_from(self.wfmt, self.d, pos + self.wsz * 2)[0]
            name_len = self.d[pos + self.wsz * 3]
            p = pos + self.wsz * 3 + 1
            name = self.d[p:p + name_len].decode("utf-8", "replace")
            p += name_len

            if end_off == 0:      # 全ゼロレコード = このリストの終端
                return

            props, after_props = self.read_props(p, num_props)
            visit(name, props, depth)

            # プロパティの後ろに子ノードが続く（残りがあれば）
            if after_props < end_off - 13:
                self.walk(after_props, end_off, depth + 1, visit)

            pos = end_off
            if pos <= hdr:        # 壊れたファイルでの無限ループを防ぐ
                return


def probe(path, show_tree=False):
    data = open(path, "rb").read()
    if data[:20] != b"Kaydara FBX Binary  ":
        print("[NG] %s : FBX バイナリではありません（ASCII FBX か別形式）" % path)
        return
    r = FbxReader(data)

    counts = Counter()
    bones = []
    meshes = []
    textures = set()
    anim_stacks = []
    anim_layers = 0
    curve_nodes = 0
    geom_verts = defaultdict(int)
    materials = set()
    current_geom = [None]

    lines = []

    def visit(name, props, depth):
        counts[name] += 1
        if show_tree and depth < 3:
            head = [p for p in props if isinstance(p, str)][:2]
            lines.append("  " * depth + name + (" " + " / ".join(head) if head else ""))

        if name == "Model" and len(props) >= 3:
            obj_name = props[1] if isinstance(props[1], str) else ""
            obj_type = props[2] if isinstance(props[2], str) else ""
            if obj_type == "LimbNode":
                bones.append(obj_name)
            elif obj_type == "Mesh":
                meshes.append(obj_name)
        elif name == "Geometry":
            current_geom[0] = props[1] if len(props) > 1 and isinstance(props[1], str) else "?"
        elif name == "Vertices" and props and isinstance(props[0], tuple):
            # ("array:d", 要素数) — 頂点数は要素数 / 3
            if current_geom[0] is not None:
                geom_verts[current_geom[0]] += props[0][1] // 3
        elif name == "RelativeFilename" and props and isinstance(props[0], str):
            textures.add(props[0].replace("\\", "/").split("/")[-1])
        elif name == "Material" and len(props) > 1 and isinstance(props[1], str):
            materials.add(props[1])
        elif name == "AnimationStack" and len(props) > 1 and isinstance(props[1], str):
            anim_stacks.append(props[1])

    r.walk(27, len(data), 0, visit)
    anim_layers = counts.get("AnimationLayer", 0)
    curve_nodes = counts.get("AnimationCurveNode", 0)

    # Deformer は Skin（スキン本体）と Cluster（ボーンごとの重み）の 2 種類が入る
    deformers = counts.get("Deformer", 0)
    total_verts = sum(geom_verts.values())

    print("=" * 70)
    print("%s" % path)
    print("  FBX バージョン : %d (%.1f)" % (r.version, r.version / 1000.0))
    print("  メッシュ       : %d 個 / 頂点合計 %d" % (len(meshes), total_verts))
    print("  マテリアル     : %d 個%s" % (len(materials),
          ("  " + ", ".join(sorted(materials)[:5])) if materials else ""))
    print("  テクスチャ参照 : %d 件%s" % (len(textures),
          ("  " + ", ".join(sorted(textures)[:6])) if textures else "（なし）"))
    print("  ボーン(LimbNode): %d 本" % len(bones))
    print("  Deformer       : %d（Skin + Cluster。ボーン数に対して十分ならスキン済み）"
          % deformers)
    print("  AnimationStack : %d %s" % (len(anim_stacks),
          ("→ " + ", ".join(anim_stacks[:8])) if anim_stacks else ""))
    print("  AnimationLayer : %d / AnimationCurveNode : %d" % (anim_layers, curve_nodes))

    # --- 判定 ---
    print("  ---- 判定 ----")
    skinned = len(bones) > 0 and deformers > 0
    animated = len(anim_stacks) > 0 and curve_nodes > 0
    if len(meshes) > 0 and skinned:
        print("  ◎ スキン済みキャラクター（ボーン %d 本）。アニメーションを当てられます。" % len(bones))
    elif len(meshes) > 0 and not skinned:
        print("  △ 静止メッシュ（ボーン無し）。アニメーションは当てられません。")
    if animated:
        print("  ◎ アニメーション %d クリップを含みます。" % len(anim_stacks))
    elif len(meshes) == 0:
        print("  ！ メッシュがありません（アニメーション専用ファイルの可能性）。")
    if not textures:
        print("  ！ テクスチャ参照なし。別ファイルで同梱されているか確認してください。")

    if bones:
        print("  ボーン名の例   : %s" % ", ".join(bones[:6]))
        # Mixamo は "mixamorig:Hips" のような命名になる
        if any(b.lower().startswith("mixamorig") for b in bones):
            print("  → Mixamo のリグです（mixamorig 命名）。Mixamo のモーションと互換。")

    if show_tree:
        print("  ---- ノード木（深さ 3 まで）----")
        for ln in lines[:80]:
            print("  " + ln)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    tree = "--tree" in sys.argv
    if not args:
        print(__doc__)
        sys.exit(1)
    for path in args:
        try:
            probe(path, tree)
        except Exception as e:                      # noqa: BLE001
            print("[エラー] %s : %s" % (path, e))


if __name__ == "__main__":
    main()
