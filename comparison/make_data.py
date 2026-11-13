"""射撃場と検証入力を生成する。第三者アセットは使用しない。"""
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "data"


def box(x, y, z, sx, sy, sz, color):
    return dict(x=x, y=y, z=z, sx=sx, sy=sy, sz=sz, r=color[0], g=color[1], b=color[2])


def segment(ticks=1, **values):
    return dict(ticks=ticks, moveX=0, moveZ=0, yawDelta=0, pitchDelta=0, fire=False, restart=False) | values


def save(name, data):
    (ROOT / name).write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    wall = (.28, .35, .43)
    spec = dict(version=1, tickRate=120, durationTicks=7200, cooldownTicks=24,
                speed=5, radius=.3, eyeHeight=1.6, extent=10, spawnX=0, spawnZ=-8,
                walls=[box(0, 1.25, 0, 3, 2.5, .5, wall), box(-5, 1.25, 1, 2, 2.5, .5, wall),
                       box(5, 1.25, 3, 2, 2.5, .5, wall),
                       box(-10.25, 1.5, 0, .5, 3, 20, wall), box(10.25, 1.5, 0, .5, 3, 20, wall),
                       box(0, 1.5, 10.25, 21, 3, .5, wall), box(0, 1.5, -10.25, 21, 3, .5, wall)],
                targets=[box(x, 1.6, z, .8, 1.2, .25, (1, .46, .12)) for z in (4, 8) for x in (-8, -4, 0, 4, 8)])
    save("range.json", spec)
    # 壁遮蔽、連射制限、斜め移動、壁衝突、上下視点、再開、時間切れを通す。
    validation = [segment(120, fire=True), segment(220, moveZ=1), segment(60, moveX=1, moveZ=1),
                  segment(1, yawDelta=90, pitchDelta=25), segment(40, moveX=-1, fire=True),
                  segment(1, restart=True), segment(1, yawDelta=-math.degrees(math.atan2(8, 12)), fire=True),
                  segment(24), segment(1, restart=True), segment(7200)]
    save("validation.json", dict(version=1, segments=validation))
    # クリア検証: 南側で横移動し、各列を正面から撃つ。中央/右の遮蔽物は迂回する。
    # 具体的な通過座標はstep=5/120の倍数を使う。
    victory = [segment(192, moveX=-1), segment(1, fire=True), segment(24), segment(1, fire=True),
               segment(96, moveX=1), segment(1, fire=True), segment(24), segment(1, fire=True),
               segment(144, moveX=1), segment(312, moveZ=1),  # x=2,z=5: 中央の壁を右から回る
               segment(48, moveX=-1), segment(1, yawDelta=180, fire=True), # 中央手前的を背面から
               segment(24), segment(1, yawDelta=-180, fire=True),        # 中央奥的
               segment(96, moveX=1), segment(1, yawDelta=180, fire=True),
               segment(24), segment(1, yawDelta=-180, fire=True),
               segment(96, moveX=1), segment(1, yawDelta=180, fire=True),
               segment(24), segment(1, yawDelta=-180, fire=True)]
    save("victory.json", dict(version=1, segments=victory))
    # 40秒の負荷比較用入力。外周を動き、同じ時刻に発砲する。両側へ同じ入力を渡す。
    tour = [segment(180, moveX=-1), segment(360, moveZ=1, fire=True), segment(1, yawDelta=90),
            segment(360, moveZ=1, fire=True), segment(1, yawDelta=90), segment(360, moveZ=1, fire=True),
            segment(1, yawDelta=90), segment(360, moveZ=1, fire=True), segment(1, restart=True)]
    used = sum(item["ticks"] for item in tour)
    tour.append(segment(4800-used))
    save("tour.json", dict(version=1, segments=tour))


if __name__ == "__main__":
    main()
