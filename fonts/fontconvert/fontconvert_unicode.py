#!/usr/bin/env python3
"""Generate the project's Unicode-aware Adafruit GFX bitmap font format.

The upstream C fontconvert utility only supports contiguous 8-bit ranges. This
converter emits the layout expected by MyGxEPD2_BW::ChineseGFXfont:

* printable ASCII U+0020..U+007E;
* degree symbol U+00B0 at the first Unicode-map entry;
* a sorted, explicit list of BMP Unicode glyphs;
* a uint16_t Unicode lookup table and ChineseGFXfont descriptor.

Glyph rendering intentionally mirrors Adafruit's FreeType converter: monochrome
hinting, tightly cropped glyph bitmaps, continuous bit packing, and baseline
offsets compatible with GFXglyph.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

try:
    from fontTools.ttLib import TTFont
    from PIL import ImageFont
except ImportError as exc:  # pragma: no cover - exercised by wrapper diagnostics
    raise SystemExit(
        "Missing font dependencies. Run: "
        "python -m pip install -r fonts/requirements.txt"
    ) from exc


ASCII_FIRST = 0x20
ASCII_LAST = 0x7E
DEGREE_CODEPOINT = 0x00B0
MAX_BITMAP_OFFSET = 0xFFFF


class FontConversionError(RuntimeError):
    """Raised when a source font cannot safely be converted."""


@dataclass(frozen=True)
class GlyphRecord:
    codepoint: int
    bitmap_offset: int
    width: int
    height: int
    x_advance: int
    x_offset: int
    y_offset: int
    bitmap: bytes


@dataclass(frozen=True)
class FontBuildResult:
    symbol: str
    point_size: int
    pixel_size: int
    y_advance: int
    glyph_count: int
    unicode_count: int
    bitmap_bytes: int
    approximate_bytes: int
    codepoints: tuple[int, ...]
    header: str

    def report(self) -> dict[str, object]:
        return {
            "symbol": self.symbol,
            "point_size": self.point_size,
            "pixel_size": self.pixel_size,
            "y_advance": self.y_advance,
            "glyph_count": self.glyph_count,
            "unicode_count": self.unicode_count,
            "bitmap_bytes": self.bitmap_bytes,
            "approximate_bytes": self.approximate_bytes,
            "codepoints": [f"U+{value:04X}" for value in self.codepoints],
        }


def _sanitize_symbol(value: str) -> str:
    symbol = re.sub(r"\W+", "_", value, flags=re.UNICODE).strip("_")
    if not symbol:
        raise FontConversionError("The generated font symbol cannot be empty.")
    if symbol[0].isdigit():
        symbol = "_" + symbol
    return symbol


def _normalize_unicode_codepoints(values: Iterable[int]) -> tuple[int, ...]:
    normalized = {DEGREE_CODEPOINT}
    for value in values:
        if value == DEGREE_CODEPOINT:
            continue
        if ASCII_FIRST <= value <= ASCII_LAST:
            continue
        if value < 0 or value > 0xFFFF:
            raise FontConversionError(
                f"U+{value:06X} is outside the renderer's uint16_t Unicode range."
            )
        # MyGxEPD2_BW currently decodes three-byte UTF-8 sequences. U+0080 to
        # U+07FF use two bytes and cannot be rendered by that implementation.
        if value < 0x0800:
            raise FontConversionError(
                f"U+{value:04X} uses two-byte UTF-8, which MyGxEPD2_BW does "
                "not currently decode."
            )
        normalized.add(value)
    return (DEGREE_CODEPOINT, *sorted(normalized - {DEGREE_CODEPOINT}))


def read_characters(path: Path) -> set[int]:
    """Read literal characters and U+XXXX tokens from a UTF-8 text file."""

    # Lines beginning with # are documentation, not requested glyph content.
    text = path.read_text(encoding="utf-8")
    content_lines = [line for line in text.splitlines() if not line.lstrip().startswith("#")]
    content = "\n".join(content_lines)
    codepoints: set[int] = set()
    for match in re.finditer(r"(?i)U\+([0-9a-f]{4,6})", content):
        codepoints.add(int(match.group(1), 16))
    for character in content:
        value = ord(character)
        if value >= 0x0800 or value == DEGREE_CODEPOINT:
            codepoints.add(value)
    return codepoints


def _font_codepoints(font_path: Path, font_index: int) -> set[int]:
    try:
        font = TTFont(str(font_path), fontNumber=font_index, lazy=True)
    except Exception as exc:  # fontTools provides several format-specific errors
        raise FontConversionError(f"Unable to open source font {font_path}: {exc}") from exc
    try:
        cmap = font.getBestCmap() or {}
        return set(cmap)
    finally:
        font.close()


def _pack_bitmap(mask: object, bbox: tuple[int, int, int, int]) -> bytes:
    left, top, right, bottom = bbox
    width = right - left
    height = bottom - top
    mask_width = mask.size[0]  # type: ignore[attr-defined]
    output = bytearray()
    current = 0
    bit_index = 0

    for y in range(height):
        for x in range(width):
            if mask[(y + top) * mask_width + (x + left)]:  # type: ignore[index]
                current |= 0x80 >> bit_index
            bit_index += 1
            if bit_index == 8:
                output.append(current)
                current = 0
                bit_index = 0

    if bit_index:
        output.append(current)
    return bytes(output)


def _render_glyph(
    font: ImageFont.FreeTypeFont,
    codepoint: int,
    bitmap_offset: int,
) -> GlyphRecord:
    character = chr(codepoint)
    mask, origin = font.getmask2(character, mode="1", anchor="ls")
    bbox = mask.getbbox()
    x_advance = int(font.getlength(character))

    if bbox is None:
        width = 0
        height = 0
        x_offset = 0
        y_offset = 1
        bitmap = b""
    else:
        left, top, right, bottom = bbox
        width = right - left
        height = bottom - top
        x_offset = origin[0] + left
        # Adafruit fontconvert uses `1 - glyph->top`; Pillow's baseline origin
        # is `-glyph->top`, hence the matching +1 adjustment here.
        y_offset = origin[1] + top + 1
        bitmap = _pack_bitmap(mask, bbox)

    if not 0 <= bitmap_offset <= MAX_BITMAP_OFFSET:
        raise FontConversionError(
            f"Bitmap offset {bitmap_offset} for U+{codepoint:04X} exceeds the "
            f"uint16_t {MAX_BITMAP_OFFSET}-byte limit. Reduce the character set "
            "or font size."
        )
    for label, value in (
        ("width", width),
        ("height", height),
        ("xAdvance", x_advance),
    ):
        if not 0 <= value <= 0xFF:
            raise FontConversionError(
                f"{label}={value} for U+{codepoint:04X} exceeds GFXglyph uint8_t."
            )
    for label, value in (("xOffset", x_offset), ("yOffset", y_offset)):
        if not -128 <= value <= 127:
            raise FontConversionError(
                f"{label}={value} for U+{codepoint:04X} exceeds GFXglyph int8_t."
            )

    return GlyphRecord(
        codepoint=codepoint,
        bitmap_offset=bitmap_offset,
        width=width,
        height=height,
        x_advance=x_advance,
        x_offset=x_offset,
        y_offset=y_offset,
        bitmap=bitmap,
    )


def _glyph_comment(codepoint: int) -> str:
    character = chr(codepoint)
    if character == "\\":
        printable = "\\\\"
    elif character == "'":
        printable = "\\'"
    elif character.isprintable():
        printable = character
    else:
        printable = ""
    return f"U+{codepoint:04X}" + (f" '{printable}'" if printable else "")


def _format_bitmap(symbol: str, bitmap: bytes) -> list[str]:
    lines = [f"const uint8_t {symbol}Bitmaps[] PROGMEM = {{"]
    if bitmap:
        for index in range(0, len(bitmap), 12):
            chunk = bitmap[index : index + 12]
            suffix = "," if index + 12 < len(bitmap) else ""
            lines.append("  " + ", ".join(f"0x{value:02X}" for value in chunk) + suffix)
    lines.append("};")
    return lines


def _format_glyphs(symbol: str, glyphs: Sequence[GlyphRecord]) -> list[str]:
    lines = [f"const GFXglyph {symbol}Glyphs[] PROGMEM = {{"]
    for index, glyph in enumerate(glyphs):
        suffix = "," if index + 1 < len(glyphs) else ""
        lines.append(
            "  { "
            f"{glyph.bitmap_offset:5d}, {glyph.width:3d}, {glyph.height:3d}, "
            f"{glyph.x_advance:3d}, {glyph.x_offset:4d}, {glyph.y_offset:4d} "
            f"}}{suffix}  // {_glyph_comment(glyph.codepoint)}"
        )
    lines.append("};")
    return lines


def _format_unicode_map(symbol: str, codepoints: Sequence[int]) -> list[str]:
    lines = [f"uint16_t {symbol}Chinese_code[] = {{"]
    for index in range(0, len(codepoints), 8):
        chunk = codepoints[index : index + 8]
        suffix = "," if index + 8 < len(codepoints) else ""
        lines.append("  " + ", ".join(f"0x{value:04X}" for value in chunk) + suffix)
    lines.append("};")
    return lines


def build_font_header(
    *,
    font_path: Path,
    point_size: int,
    symbol: str,
    codepoints: Iterable[int],
    dpi: int = 141,
    font_index: int = 0,
) -> FontBuildResult:
    """Build a complete C++ header in memory and return metadata with it."""

    font_path = font_path.resolve()
    if not font_path.is_file():
        raise FontConversionError(f"Source font not found: {font_path}")
    if point_size <= 0:
        raise FontConversionError("Point size must be positive.")
    if dpi <= 0:
        raise FontConversionError("DPI must be positive.")

    symbol = _sanitize_symbol(symbol)
    unicode_codepoints = _normalize_unicode_codepoints(codepoints)
    glyph_codepoints = (
        *range(ASCII_FIRST, ASCII_LAST + 1),
        *unicode_codepoints,
    )

    available = _font_codepoints(font_path, font_index)
    missing = [value for value in glyph_codepoints if value not in available]
    if missing:
        formatted = ", ".join(f"U+{value:04X}" for value in missing[:20])
        remainder = len(missing) - 20
        if remainder > 0:
            formatted += f", ... (+{remainder})"
        raise FontConversionError(
            f"Source font {font_path.name} does not contain: {formatted}"
        )

    # FreeType rounds point-size * DPI / 72 to the integer pixels-per-em used
    # by Pillow. This reproduces the existing 16pt Dengb metrics at 141 DPI.
    pixel_size = max(1, int(math.floor(point_size * dpi / 72.0 + 0.5)))
    layout = getattr(getattr(ImageFont, "Layout", object()), "BASIC", None)
    kwargs = {"font": str(font_path), "size": pixel_size, "index": font_index}
    if layout is not None:
        kwargs["layout_engine"] = layout
    font = ImageFont.truetype(**kwargs)

    glyphs: list[GlyphRecord] = []
    bitmap = bytearray()
    for codepoint in glyph_codepoints:
        glyph = _render_glyph(font, codepoint, len(bitmap))
        glyphs.append(glyph)
        bitmap.extend(glyph.bitmap)

    if len(bitmap) > MAX_BITMAP_OFFSET:
        raise FontConversionError(
            f"Generated bitmap is {len(bitmap)} bytes; the project format is limited "
            f"to {MAX_BITMAP_OFFSET} bytes. Reduce the character set or font size."
        )

    y_advance = int(font.font.height)
    if not 0 <= y_advance <= 0xFF:
        raise FontConversionError(
            f"yAdvance={y_advance} exceeds ChineseGFXfont uint8_t."
        )

    first = ASCII_FIRST
    last = first + len(glyphs) - 1
    if last > 0xFFFF:
        raise FontConversionError(f"Glyph index upper bound {last} exceeds uint16_t.")

    approximate_bytes = len(bitmap) + len(glyphs) * 7 + len(unicode_codepoints) * 2 + 16
    output: list[str] = [
        "// Generated by fonts/fontconvert/fontconvert_unicode.py. DO NOT EDIT.",
        f"// Source: {font_path.name}; {point_size}pt at {dpi} DPI; "
        f"{len(unicode_codepoints)} Unicode map entries.",
        "",
        *_format_bitmap(symbol, bytes(bitmap)),
        "",
        *_format_glyphs(symbol, glyphs),
        "",
        *_format_unicode_map(symbol, unicode_codepoints),
        "",
        f"const ChineseGFXfont {symbol} PROGMEM = {{",
        f"  (uint8_t  *) {symbol}Bitmaps,",
        f"  (GFXglyph *) {symbol}Glyphs,",
        f"  (uint16_t *) {symbol}Chinese_code,",
        f"  0x{first:02X}, 0x{last:04X}, {y_advance}, {len(unicode_codepoints)}",
        "};",
        "",
        f"// Approx. {approximate_bytes} bytes; bitmap payload {len(bitmap)} bytes.",
        "",
    ]

    return FontBuildResult(
        symbol=symbol,
        point_size=point_size,
        pixel_size=pixel_size,
        y_advance=y_advance,
        glyph_count=len(glyphs),
        unicode_count=len(unicode_codepoints),
        bitmap_bytes=len(bitmap),
        approximate_bytes=approximate_bytes,
        codepoints=unicode_codepoints,
        header="\n".join(output),
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert a TTF/OTF font to the project's ChineseGFXfont header."
    )
    parser.add_argument("--font", required=True, type=Path, help="Source TTF/OTF/TTC file")
    parser.add_argument("--font-index", type=int, default=0, help="Face index for TTC files")
    parser.add_argument("--size", required=True, type=int, help="Point size")
    parser.add_argument("--dpi", type=int, default=141, help="Rasterization DPI")
    parser.add_argument("--symbol", required=True, help="C++ font symbol name")
    parser.add_argument(
        "--chars-file",
        required=True,
        type=Path,
        help="UTF-8 file containing required Unicode characters or U+XXXX tokens",
    )
    parser.add_argument("--output", required=True, type=Path, help="Generated .h path")
    parser.add_argument("--report-json", type=Path, help="Optional metadata report")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        result = build_font_header(
            font_path=args.font,
            font_index=args.font_index,
            point_size=args.size,
            dpi=args.dpi,
            symbol=args.symbol,
            codepoints=read_characters(args.chars_file),
        )
    except (FontConversionError, OSError) as exc:
        print(f"fontconvert_unicode: error: {exc}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result.header, encoding="utf-8", newline="\n")
    if args.report_json:
        args.report_json.parent.mkdir(parents=True, exist_ok=True)
        args.report_json.write_text(
            json.dumps(result.report(), ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
            newline="\n",
        )
    print(
        f"Generated {args.output}: {result.glyph_count} glyphs, "
        f"{result.bitmap_bytes} bitmap bytes, yAdvance={result.y_advance}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
