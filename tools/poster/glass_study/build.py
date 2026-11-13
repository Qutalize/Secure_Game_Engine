"""Build an isolated optical study from the existing unsigned Windows runtime."""
import argparse
import ctypes as ct
import hashlib
import json
import math
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "output/packages/selected-rt-software-v1"

def blob_bytes(blob):
    table = ct.cast(blob, ct.POINTER(ct.POINTER(ct.c_void_p))).contents
    get_pointer = ct.WINFUNCTYPE(ct.c_void_p, ct.c_void_p)(table[3])
    get_size = ct.WINFUNCTYPE(ct.c_size_t, ct.c_void_p)(table[4])
    release = ct.WINFUNCTYPE(ct.c_ulong, ct.c_void_p)(table[2])
    try:
        return ct.string_at(get_pointer(blob), get_size(blob))
    finally:
        release(blob)

def compile_stage(source, entry, profile, source_name):
    compiler = ct.WinDLL("d3dcompiler_47").D3DCompile
    compiler.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_char_p, ct.c_void_p, ct.c_void_p,
                         ct.c_char_p, ct.c_char_p, ct.c_uint, ct.c_uint,
                         ct.POINTER(ct.c_void_p), ct.POINTER(ct.c_void_p)]
    compiler.restype = ct.c_int32
    code, info = ct.c_void_p(), ct.c_void_p()
    hr = compiler(source, len(source), source_name.encode(), None, None, entry, profile,
                  1 << 15, 0, ct.byref(code), ct.byref(info))
    message = blob_bytes(info).decode(errors="replace") if info else ""
    if hr < 0:
        raise RuntimeError(f"{entry.decode()} compilation failed: {message}")
    return blob_bytes(code)

def build(output, ior, absorption, bypass, scene="plate", straight_rays=False, swap_box_colors=False):
    if not math.isfinite(ior) or not 1 <= ior <= 2.5:
        raise ValueError("The glass index must be between 1 and 2.5")
    if output.exists():
        raise FileExistsError(f"Choose a new output directory: {output}")
    if scene not in ("plate", "straw", "metal") or (bypass and scene != "plate") or (straight_rays and scene != "straw"):
        raise ValueError("Choose --no-plate for plate, or --straight-rays for straw")
    if swap_box_colors and scene != "metal":
        raise ValueError("--swap-box-colors requires --scene metal")
    if scene == "metal" and (ior != 1.5 or not absorption):
        raise ValueError("--ior and --clear apply only to transparent materials")
    source_name = {"plate":"PlateStudy.hlsl", "straw":"StrawStudy.hlsl", "metal":"MetalSphereStudy.hlsl"}[scene]
    common = Path(__file__).with_name("OpticsCommon.hlsli").read_bytes()
    src = Path(__file__).with_name(source_name).read_bytes().replace(b'#include "OpticsCommon.hlsli"', common)
    defs = (f"#define PLATE_IOR {ior:.8f}\n#define PLATE_ABSORPTION {int(absorption)}\n"
            f"#define PLATE_BYPASS {int(bypass)}\n#define STRAW_BEND {int(not straight_rays)}\n").encode()
    if scene == "metal":
        defs += f"#define SWAP_BOX_COLORS {int(swap_box_colors)}\n".encode()
    stages = {name: compile_stage(defs+src, entry, profile, source_name) for name, entry, profile in (
        ("OverlayVS.dxil", b"VSMain", b"vs_5_1"), ("OverlayPS.dxil", b"PSMain", b"ps_5_1"))}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".glass-study-") as temp:
        package = Path(temp) / "package"
        shutil.copytree(BASE / "shaders", package / "shaders")
        shutil.copyfile(BASE / "SelectedGame.exe", package / "SelectedGame.exe")
        for name, data in stages.items():
            (package / "shaders" / name).write_bytes(data)
        settings = {"label":"Fixed-scene optical study on the custom D3D12 runtime",
                    "ior":ior, "absorption":absorption, "bypass":bypass,
                    "scene":scene, "size":[960,540],
                    "base_player":str((BASE / "SelectedGame.exe").relative_to(ROOT)),
                    "shader_sha256":hashlib.sha256(src).hexdigest()}
        if scene=="plate":
            settings.update(thickness_m=.55,tilt_deg=48)
        elif scene=="straw":
            settings.update(water_ior=1.333, straight_rays=straight_rays)
        else:
            for key in ("ior", "absorption", "bypass"):
                del settings[key]
            settings.update(swap_box_colors=swap_box_colors, direct_light_roughness=.16,
                            mirror_reflection_bounces=1)
        (package / "study.json").write_text(json.dumps(settings,indent=2),encoding="utf-8")
        package.rename(output)

if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    parser.add_argument("--ior",type=float,default=1.5)
    parser.add_argument("--clear",action="store_true",help="disable color absorption")
    parser.add_argument("--no-plate",action="store_true",help="render the bare reference board")
    parser.add_argument("--scene",choices=("plate","straw","metal"),default="plate")
    parser.add_argument("--straight-rays",action="store_true",help="straw control: transmit along the original direction")
    parser.add_argument("--swap-box-colors",action="store_true",help="metal control: exchange the two box pigments")
    args=parser.parse_args()
    build(args.output.resolve(),args.ior,not args.clear,args.no_plate,args.scene,args.straight_rays,args.swap_box_colors)
