"""Check error-page text against the actual bitmap font; optionally render samples.

Run from any directory: python fonts/check_error_screen.py [output-directory]
Uses the same six lines, positions, glyph offsets and icon as drawActionScreen.
This is a layout check, not a hardware/display-driver test.
"""

from pathlib import Path
import re
import sys

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "platformio/lib/esp32-weather-epd-assets/fonts/Dengb/Dengb_16pt8b.h"
SOURCE = ROOT / "platformio/src/client_utils.cpp"


def array(text, name):
    return re.search(r"\b" + name + r"\[\].*?=\s*\{(.*?)\};", text, re.S).group(1)


def load_font():
    text = FONT.read_text(encoding="utf-8")
    bitmap = bytes(int(x, 16) for x in re.findall(r"0x([\dA-Fa-f]+)", array(text, "Dengb_16pt8bBitmaps")))
    glyphs = [tuple(map(int, values)) for values in re.findall(
        r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}",
        array(text, "Dengb_16pt8bGlyphs"))]
    codes = list(range(32, 127)) + [int(x, 16) for x in re.findall(
        r"0x([\dA-Fa-f]+)", array(text, "Dengb_16pt8bChinese_code"))]
    assert len(codes) == len(glyphs), "Glyph/map count mismatch"
    return bitmap, dict(zip(codes, glyphs))


def bounds(text, glyphs):
    cursor = 0
    left, right = 10000, -10000
    for character in text:
        assert ord(character) in glyphs, f"Missing glyph: {character!r}"
        _, width, height, advance, xo, _ = glyphs[ord(character)]
        if width and height:
            left, right = min(left, cursor + xo), max(right, cursor + xo + width - 1)
        cursor += advance
    return left, right


def render(lines, size, bitmap, glyphs):
    width, height = size
    image = Image.new("1", size, 1)
    center = 168 + (width - 188) // 2
    baseline = (height - (len(lines) - 1) * 50) // 2 + 10
    for line in lines:
        left, right = bounds(line, glyphs)
        cursor = center - (right - left + 1) // 2
        assert cursor + left >= 168 and cursor + right < width - 20, (size, line, right - left + 1)
        for character in line:
            offset, gw, gh, advance, xo, yo = glyphs[ord(character)]
            for yy in range(gh):
                for xx in range(gw):
                    bit = yy * gw + xx
                    if bitmap[offset + bit // 8] & (0x80 >> (bit % 8)):
                        px, py = cursor + xo + xx, baseline + yo + yy
                        assert 0 <= px < width and 0 <= py < height, (size, line, px, py)
                        image.putpixel((px, py), 0)
            cursor += advance
        baseline += 50
    icon_path = ROOT / "platformio/lib/esp32-weather-epd-assets/icons/128x128/wi_cloud_down_128x128.h"
    icon = bytes(int(x, 16) for x in re.findall(r"0x([\dA-Fa-f]+)", array(icon_path.read_text(), "wi_cloud_down_128x128")))
    for yy in range(128):
        for xx in range(128):
            if not icon[yy * 16 + xx // 8] & (0x80 >> (xx % 8)):
                image.putpixel((20 + xx, (height - 128) // 2 + yy), 0)
    return image


def main():
    bitmap, glyphs = load_font()
    source = SOURCE.read_text(encoding="utf-8")
    translations = dict(re.findall(r'errorText\("([^"\n]*)",\s*"([^"\n]*)"\)', source))
    locale = (ROOT / "platformio/src/locales/locale_zh_CN.inc").read_text(encoding="utf-8")
    action = lambda key: re.search(r"\b" + key + r'\s*=\s*"([^"]*)"', locale).group(1)
    actions = [action("TXT_RESET_TO_RETRY"), action("TXT_RETRY_NEXT_UPDATE")]
    cases = [
        ("dns", "DNS lookup failed", "HTTP -1 / DNS / 0.5s"),
        ("connect", "CF TCP/TLS connection failed", "HTTP -1 / TLS -1 / 8.0s"),
        ("certificate", "TLS certificate rejected", "HTTP -1 / TLS -9984 / 8.0s"),
        ("response", "Waiting for CF timed out", "HTTP -11 / 10.0s"),
        ("upstream", "CF to OW timed out", "HTTP 502 / 7.2s"),
        ("json", "Weather data incomplete", "JSON -102 / 10.0s"),
        ("warming", "Weather cache preparing", "HTTP 503 / 5.0s"),
        ("warmup-timeout", "Weather cache preparation timed out", "HTTP 503 / 120.0s"),
        ("expired", "Weather cache expired", "HTTP 503 / 0.2s"),
        ("budget", "OW daily budget insufficient", "HTTP 503 / 0.2s"),
        ("wifi-reconnect", "WiFi reconnect failed", "WiFi 20/20 / -4 / 257.0s"),
    ]
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if output:
        output.mkdir(parents=True, exist_ok=True)
    for size in [(800, 480), (640, 384)]:
        for text in translations.values():
            left, right = bounds(text, glyphs)
            assert right - left + 1 <= size[0] - 188, (size, text)
        for name, reason, raw in cases:
            lines = ["One Call 4.0 API", translations[reason], raw,
                     "CF / " + translations["Current weather"] + " / 5/5", *actions]
            image = render(lines, size, bitmap, glyphs)
            if output:
                image.save(output / f"error-{name}-{size[0]}.png")
    print(f"PASS: {len(translations)} Chinese labels and {len(cases) * 2} bitmap layouts; no missing glyphs or clipping.")


if __name__ == "__main__":
    main()
