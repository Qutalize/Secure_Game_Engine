"""Measure the red straw's apparent shift in the two original game-window captures."""
import argparse
import json
from pathlib import Path

from PIL import Image


def red_center(image, row):
    # Only the straw uses saturated red in this fixed scene.
    band = []
    for x in range(470, 675):
        red, green, blue = image.getpixel((x, row))
        if red > 130 and red > green * 1.55 and red > blue * 1.5:
            band.append(x)
    if not band:
        raise ValueError(f"No straw found at row {row}")
    return (band[0] + band[-1]) / 2


def verify(directory):
    with Image.open(directory / "straight.png") as first, Image.open(directory / "bent.png") as second:
        images = (first.convert("RGB"), second.convert("RGB"))
    if any(image.size != (960, 540) for image in images):
        raise ValueError("Use uncropped 960 x 540 game captures")
    above = [red_center(image, 75) for image in images]
    immersed = [red_center(image, 360) for image in images]
    shifts = [red_center(images[1], y) - red_center(images[0], y) for y in range(350, 371)]
    if abs(above[0] - above[1]) > 1 or min(shifts) <= 30:
        raise ValueError("Expected matching straw centers above the cup and a rightward shift in water")
    return {"red_center_x_above_cup_y75": above, "red_center_x_immersed_y360": immersed,
            "shift_x_at_y360_px": immersed[1] - immersed[0],
            "min_max_shift_across_y350_370_px": [min(shifts), max(shifts)]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path)
    print(json.dumps(verify(parser.parse_args().captures), indent=2))

