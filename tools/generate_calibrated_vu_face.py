#!/usr/bin/env python3
"""Generate and verify the calibrated dual VU meter face.

The UI draws each needle from the top pivot with this same amplitude-linear
mapping.  Every printed tick is therefore a radius of the needle pivot rather
than an independently placed decorative mark.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "assets/ui/vu-meter-photoreal-base.png"
OUTPUT = ROOT / "assets/ui/vu-meter-photoreal-calibrated.png"
OUTPUT_1200 = ROOT / "assets/ui/vu-meter-photoreal-calibrated-1200x450.png"
OUTPUT_2X = ROOT / "assets/ui/vu-meter-face@2x.png"
METADATA = ROOT / "assets/ui/vu-meter-face.json"

DESIGN_WIDTH = 1200.0
DESIGN_HEIGHT = 450.0
DB_MIN = -22.0
DB_ZERO = 0.0
DB_MAX = 5.0
ANGLE_MIN = -50.037483
ANGLE_ZERO = 25.664888
ANGLE_MAX = 50.037483
ELLIPSE_RADIUS_X = 320.0
ELLIPSE_RADIUS_Y = 150.0
MAJOR_TICK_LENGTH = 24.0
MINOR_TICK_LENGTH = 12.0
SUPERSAMPLE = 4
PIVOTS = {"L": (300.5, 100.0), "R": (898.0, 100.0)}
FACE_RECTS = {
    "L": (76.0, 141.0, 449.0, 167.0),
    "R": (674.0, 141.0, 448.0, 167.0),
}
PERCENT_20_DB = 20.0 * math.log10(0.20)
TICKS = (-20.0, PERCENT_20_DB, -10.0, -9.0, -8.0, -7.0, -6.0, -5.0, -4.0, -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0)
LABELS = (-20, -10, -7, -5, -3, 0, 3, 5)
LABEL_TEXT = {-20: "−20", -10: "10", -7: "7", -5: "5", -3: "3", 0: "0", 3: "3", 5: "5+"}
PERCENT_LABELS = (0, 20, 40, 60, 80, 100)
PERCENT_ANGLE_MIN = ANGLE_MIN
PERCENT_ANGLE_MAX = ANGLE_ZERO
ARC_BLACK_WIDTH = 2.15
ARC_RED_WIDTH = 3.60
ARC_BLACK_RED_GAP_DEGREES = 1.25
TICK_ROOT_OVERLAP = 0.6


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def db_to_angle(db: float) -> float:
    bounded = min(DB_MAX, max(DB_MIN, db))
    amplitude = 10.0 ** (bounded / 20.0)
    if bounded <= DB_ZERO:
        minimum_amplitude = 10.0 ** (DB_MIN / 20.0)
        position = (amplitude - minimum_amplitude) / (1.0 - minimum_amplitude)
        return ANGLE_MIN + position * (ANGLE_ZERO - ANGLE_MIN)
    maximum_amplitude = 10.0 ** (DB_MAX / 20.0)
    position = (amplitude - 1.0) / (maximum_amplitude - 1.0)
    return ANGLE_ZERO + position * (ANGLE_MAX - ANGLE_ZERO)


def percent_to_db(percent: float) -> float | None:
    if percent <= 0.0:
        return None
    return 20.0 * math.log10(percent / 100.0)


def percent_to_angle(percent: float) -> float:
    db = percent_to_db(percent)
    return ANGLE_MIN if db is None else db_to_angle(db)


def point(pivot: tuple[float, float], angle_degrees: float, radius: float) -> tuple[float, float]:
    radians = math.radians(angle_degrees)
    return (
        pivot[0] + math.sin(radians) * radius,
        pivot[1] + math.cos(radians) * radius,
    )


def ellipse_radius(angle_degrees: float) -> float:
    """Polar radius where a needle ray intersects the calibrated ellipse."""
    radians = math.radians(angle_degrees)
    sine = math.sin(radians)
    cosine = math.cos(radians)
    return 1.0 / math.sqrt(
        (sine * sine) / (ELLIPSE_RADIUS_X * ELLIPSE_RADIUS_X)
        + (cosine * cosine) / (ELLIPSE_RADIUS_Y * ELLIPSE_RADIUS_Y)
    )


def needle_length(angle_degrees: float) -> float:
    return ellipse_radius(angle_degrees) + MAJOR_TICK_LENGTH


def tick_spec(db: float) -> tuple[str, float, float]:
    if math.isclose(db, -20.0, abs_tol=1e-6) or math.isclose(db, 5.0, abs_tol=1e-6):
        return ("stop", MAJOR_TICK_LENGTH, 3.8)
    if any(math.isclose(db, float(label), abs_tol=1e-6) for label in LABELS):
        return ("major", MAJOR_TICK_LENGTH, 2.8)
    return ("minor", MINOR_TICK_LENGTH, 1.1)


def scaled(value: float, factor: float) -> int:
    return max(1, round(value * factor))


def font(path: str, size: float, factor: float) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, scaled(size, factor))


def centered_text(
    draw: ImageDraw.ImageDraw,
    xy: tuple[float, float],
    text: str,
    text_font: ImageFont.FreeTypeFont,
    fill: tuple[int, int, int, int],
    factor: float,
) -> None:
    x, y = xy
    draw.text(
        (x * factor, y * factor),
        text,
        font=text_font,
        fill=fill,
        anchor="mm",
        stroke_width=0,
    )


def draw_percentage_label(
    draw: ImageDraw.ImageDraw,
    xy: tuple[float, float],
    percent: int,
    text_font: ImageFont.FreeTypeFont,
    fill: tuple[int, int, int, int],
    factor: float,
) -> None:
    """Center the complete 100% label on its own radial mark."""
    centered_text(
        draw,
        xy,
        f"{percent}%" if percent == 100 else str(percent),
        text_font,
        fill,
        factor,
    )


def draw_arc_band(
    draw: ImageDraw.ImageDraw,
    centerline: list[tuple[float, float]],
    widths: list[float],
    fill: tuple[int, int, int, int],
    factor: float,
) -> None:
    """Draw one continuous antialiased band, with optional width tapering."""
    if len(centerline) != len(widths) or len(centerline) < 2:
        raise ValueError("arc band needs matching centerline and width arrays")
    side_a: list[tuple[float, float]] = []
    side_b: list[tuple[float, float]] = []
    for index, ((x, y), width) in enumerate(zip(centerline, widths)):
        before = centerline[max(0, index - 1)]
        after = centerline[min(len(centerline) - 1, index + 1)]
        tangent_x = after[0] - before[0]
        tangent_y = after[1] - before[1]
        magnitude = math.hypot(tangent_x, tangent_y)
        normal_x = -tangent_y / magnitude
        normal_y = tangent_x / magnitude
        half = width * 0.5
        side_a.append(((x + normal_x * half) * factor, (y + normal_y * half) * factor))
        side_b.append(((x - normal_x * half) * factor, (y - normal_y * half) * factor))
    draw.polygon(side_a + list(reversed(side_b)), fill=fill)


def draw_meter(
    overlay: Image.Image,
    channel: str,
    factor: float,
    regular: ImageFont.FreeTypeFont,
    small: ImageFont.FreeTypeFont,
    title: ImageFont.FreeTypeFont,
) -> None:
    draw = ImageDraw.Draw(overlay)
    pivot = PIVOTS[channel]
    ink = (31, 29, 24, 224)
    red = (145, 42, 36, 226)

    # The black section stops shortly before 0 VU and the red section begins at
    # 0 VU, reproducing the small deliberate break visible on the reference.
    black_end = ANGLE_ZERO - ARC_BLACK_RED_GAP_DEGREES
    black_angles = [ANGLE_MIN + i * (black_end - ANGLE_MIN) / 224.0 for i in range(225)]
    red_angles = [ANGLE_ZERO + i * (ANGLE_MAX - ANGLE_ZERO) / 96.0 for i in range(97)]
    arc_points_black = [point(pivot, angle, ellipse_radius(angle)) for angle in black_angles]
    arc_points_red = [point(pivot, angle, ellipse_radius(angle)) for angle in red_angles]
    black_widths = [ARC_BLACK_WIDTH] * len(arc_points_black)
    red_widths = [ARC_RED_WIDTH] * len(arc_points_red)
    # Ticks are printed first and terminate only 0.6 design pixels inside the
    # centerline. Drawing the continuous arc afterward masks every tick cap, so
    # no radial stroke protrudes above the scale line or leaves a visible seam.
    for db in TICKS:
        angle = db_to_angle(float(db))
        tick_class, tick_length, tick_width = tick_spec(db)
        inner_radius = ellipse_radius(angle) - TICK_ROOT_OVERLAP
        outer_radius = ellipse_radius(angle) + tick_length
        p1 = point(pivot, angle, inner_radius)
        p2 = point(pivot, angle, outer_radius)
        tick_color = red if db >= 0 else ink
        draw.line(
            [(p1[0] * factor, p1[1] * factor), (p2[0] * factor, p2[1] * factor)],
            fill=tick_color,
            width=scaled(tick_width, factor),
        )

    draw_arc_band(draw, arc_points_black, black_widths, ink, factor)
    draw_arc_band(draw, arc_points_red, red_widths, red, factor)

    # dB label centers stay on a second concentric radius, so their spacing is
    # governed by the same dB-to-angle mapping instead of visual guesswork.
    for db in LABELS:
        angle = db_to_angle(float(db))
        label_radius = ellipse_radius(angle) + 34.0
        label_point = point(pivot, angle, label_radius)
        label = LABEL_TEXT[db]
        centered_text(
            draw,
            label_point,
            label,
            regular,
            red if db >= 0 else ink,
            factor,
        )

    # Percentage values are field-amplitude ratios. For p > 0, convert with
    # 20*log10(p/100), then feed the result through the exact needle mapping.
    for percent in PERCENT_LABELS:
        angle = percent_to_angle(float(percent))
        radius = ellipse_radius(angle)
        mark_inner = point(pivot, angle, radius - 18.5)
        mark_outer = point(pivot, angle, radius - 13.0)
        draw.line(
            [(mark_inner[0] * factor, mark_inner[1] * factor), (mark_outer[0] * factor, mark_outer[1] * factor)],
            fill=ink,
            width=scaled(1.2, factor),
        )
        radial_label_point = point(pivot, angle, radius - 29.0)
        mark_center = point(pivot, angle, radius - 15.75)
        percent_point = (mark_center[0], radial_label_point[1])
        draw_percentage_label(draw, percent_point, percent, small, ink, factor)

    centered_text(draw, (pivot[0], 189.0), "VU", title, ink, factor)
    face_x, face_y, face_w, face_h = FACE_RECTS[channel]
    centered_text(
        draw,
        (face_x + 28.0, face_y + 12.0),
        "CD.404",
        small,
        (45, 42, 34, 190),
        factor,
    )
    centered_text(
        draw,
        (face_x + face_w - 28.0, face_y + 12.0),
        "LEFT" if channel == "L" else "RIGHT",
        small,
        (45, 42, 34, 190),
        factor,
    )


def make_metadata(source_size: tuple[int, int]) -> dict:
    tick_records = []
    for db in TICKS:
        angle = db_to_angle(float(db))
        tick_class, tick_length, tick_width = tick_spec(db)
        tick_records.append(
            {
                "db": db,
                "angleDegrees": round(angle, 6),
                "class": tick_class,
                "innerRadius": round(ellipse_radius(angle) - TICK_ROOT_OVERLAP, 6),
                "outerRadius": round(ellipse_radius(angle) + tick_length, 6),
                "length": tick_length,
                "lineWidth": tick_width,
                "color": "red" if db >= 0 else "black",
            }
        )
    meters = []
    for channel in ("L", "R"):
        x, y = PIVOTS[channel]
        fx, fy, fw, fh = FACE_RECTS[channel]
        meters.append(
            {
                "channel": channel,
                "pivot": {"x": x, "y": y},
                "faceRect": {"x": fx, "y": fy, "width": fw, "height": fh},
                "needle": {
                    "angleConvention": "degrees clockwise from downward vertical",
                    "angleMin": ANGLE_MIN,
                    "angleZero": ANGLE_ZERO,
                    "angleMax": ANGLE_MAX,
                    "lengthMode": "angle-dependent outer tick radius",
                    "lengthFormula": "ellipsePolarRadius(angle, rx=320, ry=150) + 24",
                    "dbMin": DB_MIN,
                    "dbMax": DB_MAX,
                    "mapping": "piecewise amplitude-linear through aspect-corrected Sony 3 anchors: -22 dB/-50.037483 deg, 0 dB/+25.664888 deg, +5 dB/+50.037483 deg",
                },
            }
        )
    return {
        "asset": OUTPUT.name,
        "asset1200": OUTPUT_1200.name,
        "asset2x": OUTPUT_2X.name,
        "visualStyle": "photographic-calibrated-top-pivot-dual",
        "scaleGeometry": "elliptical-arc-with-radial-ticks-from-program-needle-pivot",
        "scaleArc": {
            "shape": "ellipse",
            "radiusX": ELLIPSE_RADIUS_X,
            "radiusY": ELLIPSE_RADIUS_Y,
            "polarIntersection": True,
            "supersample": SUPERSAMPLE,
            "layoutTransform": "reference card 660x328 nonuniformly fitted to target card 448.5x167",
            "blackLineWidth": ARC_BLACK_WIDTH,
            "redLineWidth": ARC_RED_WIDTH,
            "continuousUnderArc": False,
            "sharedNodeAngle": ANGLE_ZERO,
            "blackRedGapDegrees": ARC_BLACK_RED_GAP_DEGREES,
            "junctionMode": "separated-black-red-arcs-with-arc-over-tick-caps",
            "junctionCoveredByZeroTick": True,
        },
        "tickHierarchy": {
            "stop": {"length": 24.0, "lineWidth": 3.8},
            "major": {"length": 24.0, "lineWidth": 2.8},
            "minor": {"length": 12.0, "lineWidth": 1.1},
            "percentage": {"length": 5.5, "lineWidth": 1.2},
        },
        "percentageLayout": {
            "angleMin": PERCENT_ANGLE_MIN,
            "angleMax": PERCENT_ANGLE_MAX,
            "labelRadiusInset": 29.0,
            "fontSize": 8.2,
            "compositeCenterAligned": True,
            "labelAlignment": "vertical-over-percentage-mark",
            "unitPlacement": "inline-compact-with-100",
            "mapping": "field amplitude p converts to dB by 20*log10(p/100), then uses the needle map",
            "dbValues": {str(percent): percent_to_db(float(percent)) for percent in PERCENT_LABELS},
            "angleDegrees": {str(percent): round(percent_to_angle(float(percent)), 6) for percent in PERCENT_LABELS},
        },
        "referenceCalibration": {
            "sourceAnchors": {
                "minLevelDb": -22.0,
                "minAngleDegrees": 41.8,
                "zeroLevelDb": 0.0,
                "zeroAngleDegrees": -19.8,
                "maxLevelDb": 5.0,
                "maxAngleDegrees": -41.8
            },
            "programConventionAnchors": {
                "minLevelDb": DB_MIN,
                "minAngleDegrees": ANGLE_MIN,
                "zeroLevelDb": DB_ZERO,
                "zeroAngleDegrees": ANGLE_ZERO,
                "maxLevelDb": DB_MAX,
                "maxAngleDegrees": ANGLE_MAX
            }
        },
        "coordinateSpace": {"width": 1200, "height": 450},
        "sourceSize": {"width": source_size[0], "height": source_size[1]},
        "meterCount": 2,
        "dbRange": {"min": DB_MIN, "max": DB_MAX},
        "printedDbRange": {"min": -20.0, "max": DB_MAX},
        "tickValuesDb": list(TICKS),
        "labelValuesDb": list(LABELS),
        "percentLabels": list(PERCENT_LABELS),
        "ticks": tick_records,
        "meters": meters,
        "backlight": {"mode": "programmatic", "bakedIntoAsset": False},
        "clipIndicator": {
            "mode": "programmatic-active",
            "center": {"x": 600, "y": 231},
        },
    }


def generate() -> None:
    base = Image.open(BASE).convert("RGBA")
    base_factor = base.width / DESIGN_WIDTH
    if abs(base.height / DESIGN_HEIGHT - base_factor) > 0.01:
        raise SystemExit("base aspect does not match the 1200x450 design space")
    factor = base_factor * SUPERSAMPLE
    overlay = Image.new(
        "RGBA", (base.width * SUPERSAMPLE, base.height * SUPERSAMPLE), (0, 0, 0, 0)
    )
    regular = font(r"C:\Windows\Fonts\arial.ttf", 15.0, factor)
    small = font(r"C:\Windows\Fonts\arial.ttf", 8.2, factor)
    title = font(r"C:\Windows\Fonts\arial.ttf", 25.0, factor)
    for channel in ("L", "R"):
        draw_meter(overlay, channel, factor, regular, small, title)

    # A fractional photographic softness keeps the print in the same optical
    # plane as the photographed meter card without changing tick geometry.
    softened = overlay.filter(ImageFilter.GaussianBlur(radius=0.08 * factor))
    print_layer = Image.blend(softened, overlay, 0.78).resize(
        base.size, Image.Resampling.LANCZOS
    )
    output = Image.alpha_composite(base, print_layer).convert("RGB")
    output.save(OUTPUT, "PNG", optimize=True)
    output.resize((1200, 450), Image.Resampling.LANCZOS).save(
        OUTPUT_1200, "PNG", optimize=True
    )
    output.resize((2400, 900), Image.Resampling.LANCZOS).save(
        OUTPUT_2X, "PNG", optimize=True
    )
    METADATA.write_text(
        json.dumps(make_metadata(base.size), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(
        f"GENERATED output={OUTPUT.relative_to(ROOT).as_posix()} "
        f"size={output.width}x{output.height} sha256={sha256(OUTPUT)} "
        f"ticks={len(TICKS)}x2 pivots=300.5,100|898,100"
    )


def verify(profile: str, image_path: Path, metadata_path: Path) -> None:
    image = Image.open(image_path)
    base = Image.open(BASE)
    if image.size != base.size:
        raise SystemExit(f"FAIL profile={profile} reason=size actual={image.size} expected={base.size}")
    if profile == "baseline":
        if sha256(image_path) != sha256(BASE):
            raise SystemExit("FAIL profile=baseline reason=content-does-not-match-source")
        try:
            display_path = image_path.relative_to(ROOT).as_posix()
        except ValueError:
            display_path = image_path.as_posix()
        print(
            f"PASS profile=baseline file={display_path} "
            f"size={image.width}x{image.height} sha256={sha256(image_path)} "
            "state=blank-meter-cards calibrated_ticks=0"
        )
        return

    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    failures: list[str] = []
    if metadata.get("visualStyle") != "photographic-calibrated-top-pivot-dual":
        failures.append("visualStyle")
    if metadata.get("scaleGeometry") != "elliptical-arc-with-radial-ticks-from-program-needle-pivot":
        failures.append("scaleGeometry")
    scale_arc = metadata.get("scaleArc", {})
    if (
        scale_arc.get("shape") != "ellipse"
        or scale_arc.get("radiusX") != ELLIPSE_RADIUS_X
        or scale_arc.get("radiusY") != ELLIPSE_RADIUS_Y
        or scale_arc.get("supersample") != SUPERSAMPLE
        or scale_arc.get("blackLineWidth") != ARC_BLACK_WIDTH
        or scale_arc.get("redLineWidth") != ARC_RED_WIDTH
        or scale_arc.get("sharedNodeAngle") != ANGLE_ZERO
        or scale_arc.get("blackRedGapDegrees") != ARC_BLACK_RED_GAP_DEGREES
        or scale_arc.get("junctionMode") != "separated-black-red-arcs-with-arc-over-tick-caps"
    ):
        failures.append("ellipse-antialias")
    percentage_layout = metadata.get("percentageLayout", {})
    if (
        percentage_layout.get("compositeCenterAligned") is not True
        or percentage_layout.get("labelAlignment") != "vertical-over-percentage-mark"
        or percentage_layout.get("unitPlacement") != "inline-compact-with-100"
        or percentage_layout.get("mapping") != "field amplitude p converts to dB by 20*log10(p/100), then uses the needle map"
    ):
        failures.append("percentage-alignment")
    for meter in metadata.get("meters", []):
        pivot = (meter["pivot"]["x"], meter["pivot"]["y"])
        if tuple(pivot) != PIVOTS[meter["channel"]]:
            failures.append(f"pivot-{meter['channel']}")
        needle = meter["needle"]
        if (
            needle["angleMin"] != ANGLE_MIN
            or needle["angleZero"] != ANGLE_ZERO
            or needle["angleMax"] != ANGLE_MAX
            or needle["lengthMode"] != "angle-dependent outer tick radius"
        ):
            failures.append(f"needle-calibration-{meter['channel']}")
    maximum_cross_error = 0.0
    tick_values = [float(tick["db"]) for tick in metadata.get("ticks", [])]
    between_minus_20_and_minus_10 = [value for value in tick_values if -20.0 < value < -10.0]
    if (
        len(tick_values) != 18
        or any(math.isclose(value, -22.0, abs_tol=1e-6) for value in tick_values)
        or len(between_minus_20_and_minus_10) != 1
        or not math.isclose(between_minus_20_and_minus_10[0], PERCENT_20_DB, abs_tol=1e-6)
    ):
        failures.append("printed-tick-sequence")
    for tick in metadata.get("ticks", []):
        angle = db_to_angle(float(tick["db"]))
        expected_class, expected_length, expected_width = tick_spec(float(tick["db"]))
        if (
            tick.get("class") != expected_class
            or tick.get("length") != expected_length
            or tick.get("lineWidth") != expected_width
        ):
            failures.append(f"tick-style-{tick['db']}")
        maximum_cross_error = max(maximum_cross_error, abs(angle - tick["angleDegrees"]))
        for pivot in PIVOTS.values():
            inner = point(pivot, tick["angleDegrees"], tick["innerRadius"])
            outer = point(pivot, tick["angleDegrees"], tick["outerRadius"])
            cross = abs(
                (inner[0] - pivot[0]) * (outer[1] - pivot[1])
                - (inner[1] - pivot[1]) * (outer[0] - pivot[0])
            )
            maximum_cross_error = max(maximum_cross_error, cross)
    if maximum_cross_error > 0.0001:
        failures.append("tick-collinearity")
    if sha256(image_path) == sha256(BASE):
        failures.append("image-unchanged")
    if failures:
        raise SystemExit(f"FAIL profile=modified reason={','.join(failures)}")
    print(
        f"PASS profile=modified file={image_path.relative_to(ROOT).as_posix()} "
        f"size={image.width}x{image.height} sha256={sha256(image_path)} "
        f"pivots=300.5,100|898,100 ticks={len(metadata['ticks'])}x2 interval=-20..-10:1@{PERCENT_20_DB:.6f} "
        f"db_range=-22..+5 anchors=-50.037483|25.664888|50.037483 "
        f"max_alignment_error={maximum_cross_error:.6f} "
        f"ellipse=320x150 tick_widths=3.8|2.8|1.1 tick_lengths=24|12 "
        f"percent_db=20:{PERCENT_20_DB:.6f}|40:{percent_to_db(40):.6f}|60:{percent_to_db(60):.6f}|80:{percent_to_db(80):.6f}|100:0.000000 "
        f"percent_alignment=amplitude-derived+vertical+100percent-composite-centered "
        f"arc_widths={ARC_BLACK_WIDTH:.2f}|{ARC_RED_WIDTH:.2f} gap={ARC_BLACK_RED_GAP_DEGREES:.2f}deg seam=arc-over-tick-caps+separated-color-segments "
        f"supersample={SUPERSAMPLE}x layout=reference-affine mapping=piecewise-amplitude-linear"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("generate")
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--profile", choices=("baseline", "modified"), required=True)
    verify_parser.add_argument("--image", type=Path, required=True)
    verify_parser.add_argument("--metadata", type=Path, default=METADATA)
    args = parser.parse_args()
    if args.command == "generate":
        generate()
    else:
        verify(args.profile, args.image.resolve(), args.metadata.resolve())


if __name__ == "__main__":
    main()
