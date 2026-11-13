#!/usr/bin/env python3
"""FBX → glTF(.glb) 一括変換（Blender の Python API で実行する）。

なぜ必要か:
  本エンジンのローダーは glTF 2.0 のみ対応。FBX は Autodesk の独自バイナリ形式で、
  パーサを自作するのは PNG や glTF とは工数の桁が違ううえ、公式 SDK はライセンスが絡む。
  幸い Blender が FBX を読めて glTF を書けるので、変換は Blender に任せるのが正解。

使い方（Blender の実行ファイル経由で呼ぶ。"--" 以降がこのスクリプトの引数）:

  # まず中身を調べる（マテリアル名・メッシュ・ボーンの有無を出すだけ）
  blender --background --python tools/fbx_to_glb.py -- \
      --src "D:/path/to/Pack" --pattern "Meshes/Guns/SM_Ak_47.fbx" --inspect

  # 変換する
  blender --background --python tools/fbx_to_glb.py -- \
      --src "D:/path/to/Pack" --pattern "Meshes/Guns/*.fbx" \
      --out assets/models/external --texsize 1024

引数:
  --src      パックのルート（この下の Textures/ からテクスチャを探す）
  --pattern  --src からの相対 glob。複数指定可
  --out      .glb の出力先
  --texsize  埋め込むテクスチャの一辺（既定 1024）。元が 4096 だと 1 枚 67MB の
             VRAM を食うので縮小する。ローポリのアトラスなら 1024 で十分
  --scale    取り込み後にスケールを掛ける（既定 1.0）
  --inspect  変換せず中身を報告するだけ
  --limit    変換する最大件数（試し打ち用）
  --override-tex <stem> | <材質名の一部>=<stem>
             マテリアル名からの自動推測を使わず、テクスチャを明示指定する
             （stem は拡張子・パス無しのファイル名。例: Universal_A_Alb）。
             Unity のプリーハブは「メッシュ(FBX)は共通、色違いは差し替えマテリアルで表現」
             という構成が多く、FBX 自身のマテリアル名は "Universal Shader V10" のような
             汎用プレースホルダのままで、どのアトラス(A/B/C...)を使うかは Unity 側の
             .mat/.prefab にしか書かれていない。自動推測（マテリアル名→テクスチャ名）が
             効かないのはこのため。.prefab の m_Materials guid → .mat.meta → .mat の
             _MainTex guid → .png.meta の順にたどって実際のテクスチャを特定してから、
             ここで明示的に指定すること。

             2 形式あり、複数回指定できる:
               --override-tex Universal_A_Alb
                   全マテリアルに同じテクスチャ（1 マテリアルのアセット向け）
               --override-tex "Plaster=Wall_Plaster_A_Alb" --override-tex "Universal=Universal_A_Alb"
                   マテリアル名に "Plaster" を含むものへ Wall_Plaster_A_Alb を、
                   "Universal" を含むものへ Universal_A_Alb を割り当てる（部分一致・
                   大文字小文字無視）。壁のように 1 メッシュが複数マテリアルを持つ
                   アセットは、こちらを使わないと全部同じ絵柄になってしまう。
             ※ 先に書いた `名前=stem` から順に照合し、最初に一致したものを使う。
                `=` 無しの指定は「どれにも一致しなかった場合の既定」として働く。
"""

import glob
import os
import sys

try:
    import bpy
except ImportError:  # pragma: no cover
    print("このスクリプトは Blender の中で実行してください:")
    print('  blender --background --python tools/fbx_to_glb.py -- --help')
    sys.exit(1)


# ---------------------------------------------------------------------------
def parse_args(argv):
    # Blender は "--" より前を自分の引数として食うので、以降だけを見る。
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    opt = {
        "src": "", "out": "assets/models/external", "patterns": [],
        "texsize": 1024, "scale": 1.0, "inspect": False, "limit": 0,
        # [(材質名の一部 or "", テクスチャ stem), ...] 指定順に照合する。
        # 名前が "" のエントリは「どれにも一致しなかった場合の既定」。
        "override_tex": [],
    }
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--src":        opt["src"] = args[i + 1]; i += 2
        elif a == "--out":      opt["out"] = args[i + 1]; i += 2
        elif a == "--pattern":  opt["patterns"].append(args[i + 1]); i += 2
        elif a == "--texsize":  opt["texsize"] = int(args[i + 1]); i += 2
        elif a == "--scale":    opt["scale"] = float(args[i + 1]); i += 2
        elif a == "--limit":    opt["limit"] = int(args[i + 1]); i += 2
        elif a == "--inspect":  opt["inspect"] = True; i += 1
        elif a == "--override-tex":
            spec = args[i + 1]
            if "=" in spec:
                name, stem = spec.split("=", 1)
                opt["override_tex"].append((name.strip(), stem.strip()))
            else:
                opt["override_tex"].append(("", spec.strip()))
            i += 2
        elif a in ("--help", "-h"):
            print(__doc__); sys.exit(0)
        else:
            print("不明な引数: %s" % a); sys.exit(1)
    return opt


def ensure_importer():
    """FBX インポータが使える状態か確認する。

    Blender 4.x 以降は FBX 入出力がアドオン(io_scene_fbx)として同梱されており、
    通常は有効だが、環境によっては無効になっている。ここで明示的に有効化しておく。
    """
    if hasattr(bpy.ops.import_scene, "fbx"):
        return True
    try:
        import addon_utils
        addon_utils.enable("io_scene_fbx", default_set=True, persistent=True)
    except Exception as e:                          # noqa: BLE001
        print("FBX アドオンの有効化に失敗: %s" % e)
    if not hasattr(bpy.ops.import_scene, "fbx"):
        print("[致命的] この Blender では FBX インポータが使えません。")
        print("  Blender の 編集 > プリファレンス > アドオン で 'FBX' を検索して有効化してください。")
        return False
    return True


def clear_scene():
    """シーンを空にする。1 ファイルずつ独立して変換するため毎回呼ぶ。

    ※ read_factory_settings ではなく read_homefile を使う。
      前者はアドオンの有効状態まで初期化してしまうことがあり、
      2 ファイル目以降で FBX インポータが消える事故が起きうる。
    """
    bpy.ops.wm.read_homefile(use_empty=True)


def build_texture_index(src):
    """<src>/Textures/** の PNG を「小文字のファイル名（拡張子なし）→ パス」で索引する。"""
    index = {}
    tex_root = os.path.join(src, "Textures")
    for path in glob.glob(os.path.join(tex_root, "**", "*.png"), recursive=True):
        stem = os.path.splitext(os.path.basename(path))[0].lower()
        index.setdefault(stem, path)
    return index


def find_texture_for_material(mat_name, tex_index):
    """マテリアル名からアルベドテクスチャを推測する。

    Unity のパックは「マテリアルは FBX 内、テクスチャの紐付けは .mat（Unity 固有）」
    という構成が多く、FBX 単体には画像が入っていないことがある。そこで
    命名規則から探す:  M_Universal_A  →  Universal_A_Alb.png
    """
    base = mat_name
    if base.lower().startswith("m_"):
        base = base[2:]
    for cand in (base + "_alb", base + "_albedo", base + "_basecolor", base):
        hit = tex_index.get(cand.lower())
        if hit:
            return hit
    return None


def resolve_override_stem(mat_name, overrides):
    """--override-tex の指定リストから、このマテリアルに使う stem を選ぶ。

    指定順に「材質名の一部」を部分一致（大文字小文字無視）で照合し、最初に一致した
    ものを返す。名前が空のエントリは「どれにも一致しなかった場合の既定」として、
    リストを全部見終えてから使う。該当が無ければ None（＝名前からの自動推測へ）。
    """
    fallback = None
    lower = mat_name.lower()
    for name, stem in overrides:
        if not name:
            if fallback is None:
                fallback = stem
            continue
        if name.lower() in lower:
            return stem
    return fallback


def ensure_material_texture(mat, tex_index, texsize, cache, overrides=()):
    """マテリアルに画像テクスチャが無ければ、名前から探して割り当てる。

    overrides（--override-tex の指定リスト）に該当があれば、名前推測を使わず
    そのテクスチャを強制的に使う（Unity 側の .mat/.prefab を手繰って特定した
    アトラスを明示指定する場合に使う）。

    戻り値: (割り当てたか, テクスチャ名 or 理由)
    """
    if not mat.use_nodes:
        mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links

    override_stem = resolve_override_stem(mat.name, overrides)

    # 既に「中身のある」画像テクスチャが繋がっているなら触らない。
    # ★ただし空の画像データブロックに注意★ FBX は「テクスチャを参照している」という
    #   情報だけを持ち、実ファイルが同梱されていない（＝Unity 側にしかない）ことがある。
    #   その場合 Blender には size 0 の壊れた画像データブロックができ、`n.image is not None`
    #   は True になってしまう。これを「既存テクスチャあり」と誤判定すると、割り当てを
    #   スキップした挙句 glTF には画像が出力されず、真っ白な面になる（壁で実際に踏んだ）。
    #   なので画素を持っているかまで確認する。
    # ★また、明示的な `名前=stem` 指定がある場合はそれを優先する★ 「上書き指定」なのだから
    #   既存より強いのが自然で、そうでないと壊れた参照を持つアセットを救えない。
    for n in nodes:
        if n.type != "TEX_IMAGE" or n.image is None:
            continue
        has_pixels = (n.image.size[0] > 0 and n.image.size[1] > 0)
        if has_pixels and not override_stem:
            return (False, "既存: " + n.image.name)
        # 壊れた参照 or 上書き指定あり → このノードを外して下で貼り直す。
        nodes.remove(n)
        break

    if override_stem:
        path = tex_index.get(override_stem.lower())
        if path is None:
            return (False, "--override-tex 指定のテクスチャが見つからない: %s" % override_stem)
    else:
        path = find_texture_for_material(mat.name, tex_index)
    if path is None:
        return (False, "該当テクスチャ無し")

    img = cache.get(path)
    if img is None:
        img = bpy.data.images.load(path, check_existing=True)
        # ★縮小★ 4096 のアトラスをそのまま埋め込むと 1 枚で 67MB の VRAM を食う。
        # ローポリのアトラスは色面が大きいので 1024 でも見た目はほぼ変わらない。
        if texsize > 0 and (img.size[0] > texsize or img.size[1] > texsize):
            img.scale(texsize, texsize)
        cache[path] = img

    bsdf = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        bsdf = nodes.new("ShaderNodeBsdfPrincipled")
        out = next((n for n in nodes if n.type == "OUTPUT_MATERIAL"), None)
        if out is None:
            out = nodes.new("ShaderNodeOutputMaterial")
        links.new(bsdf.outputs[0], out.inputs["Surface"])

    tex = nodes.new("ShaderNodeTexImage")
    tex.image = img
    links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    return (True, os.path.basename(path))


def inspect_current_scene(label):
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    arms   = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    tris = 0
    for o in meshes:
        m = o.data
        tris += sum(max(len(p.vertices) - 2, 0) for p in m.polygons)
    mats = sorted({ms.material.name for o in meshes for ms in o.material_slots
                   if ms.material is not None})
    bones = sum(len(a.data.bones) for a in arms)

    print("  [%s]" % label)
    print("    メッシュ %d / 三角形 %d / マテリアル %d / アーマチュア %d（ボーン %d）"
          % (len(meshes), tris, len(mats), len(arms), bones))
    if mats:
        print("    マテリアル: " + ", ".join(mats[:8]) + (" ..." if len(mats) > 8 else ""))
    # 寸法（接地・スケールの確認用）
    if meshes:
        lo = [1e30] * 3
        hi = [-1e30] * 3
        for o in meshes:
            for c in o.bound_box:
                w = o.matrix_world @ __import__("mathutils").Vector(c)
                for i in range(3):
                    lo[i] = min(lo[i], w[i]); hi[i] = max(hi[i], w[i])
        print("    寸法 %.3f x %.3f x %.3f （Blender は Z-up。glTF 書き出しで Y-up に変換される）"
              % (hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]))


def export_glb(out_path):
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    # ★Draco 圧縮は必ずオフ★ 本エンジンのローダーは未対応。
    # export_format='GLB' でテクスチャも 1 ファイルに埋め込まれる。
    # Blender のバージョンによって glTF エクスポータの引数名が増減するため、
    # 「全部指定 → 1 つずつ削る」の順で通るまで試す。
    # 最低限 filepath と GLB 指定さえ通れば変換自体は成立する。
    attempts = [
        dict(filepath=out_path, export_format="GLB",
             export_draco_mesh_compression_enable=False,
             export_apply=True, export_yup=True),
        dict(filepath=out_path, export_format="GLB",
             export_draco_mesh_compression_enable=False, export_apply=True),
        dict(filepath=out_path, export_format="GLB",
             export_draco_mesh_compression_enable=False),
        dict(filepath=out_path, export_format="GLB"),
    ]
    last = None
    for kwargs in attempts:
        try:
            bpy.ops.export_scene.gltf(**kwargs)
            return
        except TypeError as e:
            last = e
            continue
    raise RuntimeError("glTF エクスポートの引数が合いません: %s" % last)


def main():
    opt = parse_args(sys.argv)
    if not opt["src"] or not opt["patterns"]:
        print("--src と --pattern は必須です。--help を参照。")
        sys.exit(1)

    src = os.path.abspath(opt["src"])
    files = []
    for pat in opt["patterns"]:
        files += sorted(glob.glob(os.path.join(src, pat), recursive=True))
    files = [f for f in files if f.lower().endswith(".fbx")]
    if opt["limit"] > 0:
        files = files[:opt["limit"]]

    if not files:
        print("該当する FBX がありません: %s" % opt["patterns"])
        sys.exit(1)

    if not ensure_importer():
        sys.exit(2)

    print("Blender %s" % bpy.app.version_string)
    print("対象 %d ファイル" % len(files))
    tex_index = build_texture_index(src)
    print("テクスチャ索引 %d 件（%s/Textures）" % (len(tex_index), src))

    ok = 0
    fail = []
    for n, path in enumerate(files, 1):
        name = os.path.splitext(os.path.basename(path))[0]
        print("[%d/%d] %s" % (n, len(files), name))
        try:
            clear_scene()
            bpy.ops.import_scene.fbx(filepath=path)

            if opt["scale"] != 1.0:
                for o in bpy.data.objects:
                    if o.parent is None:
                        o.scale = [s * opt["scale"] for s in o.scale]

            # マテリアルにテクスチャを割り当てる（FBX 単体には入っていないことが多い）
            cache = {}
            for mat in bpy.data.materials:
                assigned, why = ensure_material_texture(
                    mat, tex_index, opt["texsize"], cache, opt["override_tex"])
                if assigned:
                    print("    %s ← %s" % (mat.name, why))
                elif why == "該当テクスチャ無し":
                    print("    %s : テクスチャ未割当（単色で出力されます）" % mat.name)

            if opt["inspect"]:
                inspect_current_scene(name)
                continue

            out_path = os.path.join(os.path.abspath(opt["out"]), name + ".glb")
            export_glb(out_path)
            size = os.path.getsize(out_path)
            print("    → %s (%.1f KB)" % (out_path, size / 1024.0))
            ok += 1
        except Exception as e:                      # noqa: BLE001
            print("    [失敗] %s" % e)
            fail.append((name, str(e)))

    print("")
    if opt["inspect"]:
        print("調査のみ（出力なし）")
    else:
        print("完了: 成功 %d / 失敗 %d" % (ok, len(fail)))
    for name, err in fail[:10]:
        print("  失敗: %s — %s" % (name, err))


if __name__ == "__main__":
    main()
