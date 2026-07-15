#!/usr/bin/env python3
"""Scan firmware text, detect missing glyphs, and update Chinese bitmap fonts."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
CONVERTER_DIR = SCRIPT_DIR / "fontconvert"
sys.path.insert(0, str(CONVERTER_DIR))

try:
    from fontconvert_unicode import (  # type: ignore[import-not-found]
        DEGREE_CODEPOINT,
        MAX_BITMAP_OFFSET,
        FontBuildResult,
        FontConversionError,
        build_font_header,
        read_characters,
    )
except ImportError as exc:  # pragma: no cover - wrapper prints dependency help
    raise SystemExit(f"Unable to load Unicode font converter: {exc}") from exc


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc"}


@dataclass(frozen=True)
class ExistingFont:
    path: Path
    codepoints: frozenset[int]
    bitmap_bytes: int
    file_bytes: int


@dataclass(frozen=True)
class ScanResult:
    codepoints: frozenset[int]
    scanned_files: tuple[Path, ...]
    active_locale: str | None
    unsupported_two_byte: frozenset[int]
    unsupported_non_bmp: frozenset[int]


def _configure_console() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            try:
                reconfigure(encoding="utf-8")
            except OSError:
                pass


def _load_config(path: Path) -> dict[str, object]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise FontConversionError(f"Font sync config not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise FontConversionError(f"Invalid JSON in {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise FontConversionError("Font sync config root must be a JSON object.")
    return data


def _active_locale() -> str | None:
    config_header = PROJECT_ROOT / "platformio" / "include" / "config.h"
    if not config_header.is_file():
        return None
    match = re.search(
        r"^\s*#\s*define\s+LOCALE\s+([A-Za-z0-9_]+)\s*$",
        config_header.read_text(encoding="utf-8"),
        flags=re.MULTILINE,
    )
    return match.group(1) if match else None


def _record_codepoint(
    value: int,
    supported: set[int],
    two_byte: set[int],
    non_bmp: set[int],
) -> None:
    if value == DEGREE_CODEPOINT:
        supported.add(value)
    elif value >= 0x10000:
        non_bmp.add(value)
    elif value >= 0x0800:
        supported.add(value)
    elif value >= 0x0080:
        two_byte.add(value)


def _extract_literal_codepoints(text: str) -> tuple[set[int], set[int], set[int]]:
    """Extract Unicode from C/C++ string literals while ignoring comments."""

    supported: set[int] = set()
    two_byte: set[int] = set()
    non_bmp: set[int] = set()
    index = 0
    length = len(text)

    def record(value: int) -> None:
        _record_codepoint(value, supported, two_byte, non_bmp)

    while index < length:
        if text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = length if newline < 0 else newline + 1
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = length if end < 0 else end + 2
            continue

        # C++ raw string: R"delimiter(content)delimiter"
        raw_marker = text.find('R"', index, min(length, index + 4))
        if raw_marker == index or (
            raw_marker > index and text[index:raw_marker] in {"u8", "u", "U", "L"}
        ):
            delimiter_start = raw_marker + 2
            open_paren = text.find("(", delimiter_start, delimiter_start + 18)
            if open_paren >= 0:
                delimiter = text[delimiter_start:open_paren]
                terminator = ")" + delimiter + '"'
                end = text.find(terminator, open_paren + 1)
                if end >= 0:
                    for character in text[open_paren + 1 : end]:
                        record(ord(character))
                    index = end + len(terminator)
                    continue

        character = text[index]
        if character == "'":
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                elif text[index] == "'":
                    index += 1
                    break
                else:
                    index += 1
            continue

        if character != '"':
            index += 1
            continue

        index += 1
        while index < length:
            character = text[index]
            if character == '"':
                index += 1
                break
            if character != "\\":
                record(ord(character))
                index += 1
                continue

            if index + 1 >= length:
                index += 1
                break
            escape = text[index + 1]
            if escape == "u" and index + 6 <= length:
                token = text[index + 2 : index + 6]
                if re.fullmatch(r"[0-9A-Fa-f]{4}", token):
                    record(int(token, 16))
                    index += 6
                    continue
            if escape == "U" and index + 10 <= length:
                token = text[index + 2 : index + 10]
                if re.fullmatch(r"[0-9A-Fa-f]{8}", token):
                    record(int(token, 16))
                    index += 10
                    continue
            # Other escapes represent ASCII/control bytes and do not add a
            # Unicode glyph to the project's explicit map.
            index += 2

    return supported, two_byte, non_bmp


def _source_files(config: dict[str, object], active_locale: str | None) -> list[Path]:
    roots = config.get("scan_roots", ["platformio/src", "platformio/include"])
    if not isinstance(roots, list) or not all(isinstance(value, str) for value in roots):
        raise FontConversionError("scan_roots must be a list of paths.")

    files: list[Path] = []
    for raw_root in roots:
        root = (PROJECT_ROOT / raw_root).resolve()
        if not root.exists():
            raise FontConversionError(f"Scan root not found: {root}")
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if path.parent.name == "locales" and path.name.startswith("locale_"):
                if active_locale and path.name != f"locale_{active_locale}.inc":
                    continue
            files.append(path)
    return sorted(set(files))


def scan_required_codepoints(config: dict[str, object]) -> ScanResult:
    active_locale = _active_locale()
    supported: set[int] = {DEGREE_CODEPOINT}
    two_byte: set[int] = set()
    non_bmp: set[int] = set()
    files = _source_files(config, active_locale)

    for path in files:
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as exc:
            raise FontConversionError(f"Source file is not UTF-8: {path}") from exc
        found, found_two_byte, found_non_bmp = _extract_literal_codepoints(text)
        supported.update(found)
        two_byte.update(found_two_byte)
        non_bmp.update(found_non_bmp)

    extras = config.get("extra_character_files", [])
    if not isinstance(extras, list) or not all(isinstance(value, str) for value in extras):
        raise FontConversionError("extra_character_files must be a list of paths.")
    for raw_path in extras:
        path = (PROJECT_ROOT / raw_path).resolve()
        if not path.is_file():
            raise FontConversionError(f"Extra character file not found: {path}")
        supported.update(read_characters(path))

    return ScanResult(
        codepoints=frozenset(supported),
        scanned_files=tuple(files),
        active_locale=active_locale,
        unsupported_two_byte=frozenset(two_byte),
        unsupported_non_bmp=frozenset(non_bmp),
    )


def _parse_existing_font(path: Path, symbol: str) -> ExistingFont:
    if not path.is_file():
        return ExistingFont(path, frozenset(), 0, 0)
    text = path.read_text(encoding="utf-8")
    map_match = re.search(
        rf"\b{re.escape(symbol)}Chinese_code\s*\[\s*\]\s*=\s*\{{(.*?)\}}\s*;",
        text,
        flags=re.DOTALL,
    )
    if not map_match:
        raise FontConversionError(f"Unicode map not found in existing font: {path}")
    codepoints = {
        int(match, 16)
        for match in re.findall(r"0x([0-9A-Fa-f]{2,6})", map_match.group(1))
    }

    bitmap_match = re.search(
        rf"\b{re.escape(symbol)}Bitmaps\s*\[\s*\].*?=\s*\{{(.*?)\}}\s*;",
        text,
        flags=re.DOTALL,
    )
    if not bitmap_match:
        raise FontConversionError(f"Bitmap array not found in existing font: {path}")
    bitmap_bytes = len(re.findall(r"0x[0-9A-Fa-f]{2}", bitmap_match.group(1)))
    return ExistingFont(path, frozenset(codepoints), bitmap_bytes, path.stat().st_size)


def _resolve_font_path(config: dict[str, object], override: Path | None) -> Path:
    candidates: list[Path] = []
    if override:
        candidates.append(override)
    env_font = os.environ.get("EPD_FONT_FILE")
    if env_font:
        candidates.append(Path(env_font))
    configured = config.get("font_candidates", [])
    if not isinstance(configured, list) or not all(isinstance(value, str) for value in configured):
        raise FontConversionError("font_candidates must be a list of paths.")
    candidates.extend(Path(value) for value in configured)

    checked: list[Path] = []
    for candidate in candidates:
        resolved = candidate if candidate.is_absolute() else PROJECT_ROOT / candidate
        resolved = resolved.resolve()
        checked.append(resolved)
        if resolved.is_file():
            return resolved
    formatted = "\n  ".join(str(path) for path in checked) or "(no candidates configured)"
    raise FontConversionError(
        "No usable source font found. Set EPD_FONT_FILE or --font. Checked:\n  "
        + formatted
    )


def _configured_sizes(config: dict[str, object], override: list[int] | None) -> list[int]:
    values: object = override if override else config.get("sizes", [16])
    if not isinstance(values, list) or not values or not all(isinstance(value, int) for value in values):
        raise FontConversionError("sizes must be a non-empty list of integers.")
    if any(value <= 0 for value in values):
        raise FontConversionError("Font sizes must be positive.")
    return sorted(set(values))


def _target_path(config: dict[str, object], family: str, size: int) -> Path:
    template = config.get(
        "output_template",
        "platformio/lib/esp32-weather-epd-assets/fonts/{family}/{family}_{size}pt8b.h",
    )
    if not isinstance(template, str):
        raise FontConversionError("output_template must be a string.")
    return (PROJECT_ROOT / template.format(family=family, size=size)).resolve()


def _format_codepoints(values: Iterable[int], limit: int = 24) -> str:
    ordered = sorted(set(values))
    if not ordered:
        return "none"
    chunks = [f"{chr(value)}(U+{value:04X})" for value in ordered[:limit]]
    if len(ordered) > limit:
        chunks.append(f"... (+{len(ordered) - limit})")
    return " ".join(chunks)


def _atomic_replace_many(generated: dict[Path, str]) -> None:
    originals: dict[Path, bytes | None] = {
        path: path.read_bytes() if path.exists() else None for path in generated
    }
    replaced: list[Path] = []
    try:
        for path, content in generated.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                newline="\n",
                dir=path.parent,
                prefix=path.name + ".",
                suffix=".tmp",
                delete=False,
            ) as handle:
                handle.write(content)
                temporary = Path(handle.name)
            os.replace(temporary, path)
            replaced.append(path)
    except Exception:
        for path in reversed(replaced):
            original = originals[path]
            if original is None:
                path.unlink(missing_ok=True)
                continue
            with tempfile.NamedTemporaryFile(
                mode="wb",
                dir=path.parent,
                prefix=path.name + ".rollback.",
                suffix=".tmp",
                delete=False,
            ) as handle:
                handle.write(original)
                temporary = Path(handle.name)
            os.replace(temporary, path)
        raise


def _platformio_command() -> list[str]:
    configured = os.environ.get("PLATFORMIO_CMD")
    if configured:
        return shlex.split(configured, posix=os.name != "nt")
    for executable in ("pio", "platformio"):
        resolved = shutil.which(executable)
        if resolved:
            return [resolved]
    if os.name == "nt":
        platformio_venv = Path.home() / ".platformio" / "penv" / "Scripts"
        for executable in ("platformio.exe", "pio.exe"):
            candidate = platformio_venv / executable
            if candidate.is_file():
                return [str(candidate)]
    try:
        if importlib.util.find_spec("platformio.__main__") is not None:
            return [sys.executable, "-m", "platformio"]
    except ModuleNotFoundError:
        pass
    raise FontConversionError(
        "PlatformIO CLI not found. Install PlatformIO or set PLATFORMIO_CMD."
    )


def _run_build(environment: str) -> None:
    command = [*_platformio_command(), "run", "-e", environment]
    print("\nBuilding firmware:", subprocess.list2cmdline(command))
    subprocess.run(command, cwd=PROJECT_ROOT / "platformio", check=True)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Detect missing Chinese glyphs and regenerate project bitmap fonts."
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=SCRIPT_DIR / "font_sync.json",
        help="Font sync JSON configuration",
    )
    parser.add_argument("--font", type=Path, help="Override source TTF/OTF/TTC")
    parser.add_argument("--font-index", type=int, default=0, help="Face index for TTC")
    parser.add_argument("--sizes", type=int, nargs="+", help="Override configured point sizes")
    parser.add_argument("--apply", action="store_true", help="Replace generated font headers")
    parser.add_argument("--build", action="store_true", help="Build firmware after a successful sync")
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Generate validated headers into a staging directory instead of replacing project files",
    )
    parser.add_argument(
        "--preserve-existing",
        action="store_true",
        help="Keep every existing mapped glyph (may exceed the 64 KiB bitmap limit)",
    )
    parser.add_argument("--report-json", type=Path, help="Write a machine-readable report")
    parser.add_argument("--environment", help="Override PlatformIO environment")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    _configure_console()
    args = _build_parser().parse_args(argv)
    try:
        config = _load_config(args.config.resolve())
        if args.output_dir and not args.apply:
            raise FontConversionError("--output-dir requires --apply.")
        if args.output_dir and args.build:
            raise FontConversionError("--build cannot be combined with --output-dir.")
        family = config.get("family", "Dengb")
        if not isinstance(family, str) or not family:
            raise FontConversionError("family must be a non-empty string.")
        dpi = config.get("dpi", 141)
        if not isinstance(dpi, int) or dpi <= 0:
            raise FontConversionError("dpi must be a positive integer.")
        sizes = _configured_sizes(config, args.sizes)
        scan = scan_required_codepoints(config)
        source_font = _resolve_font_path(config, args.font)

        print("Chinese bitmap font sync")
        print(f"  Family:        {family}")
        print(f"  Source font:   {source_font}")
        print(f"  Active locale: {scan.active_locale or 'unknown'}")
        print(f"  Scanned files: {len(scan.scanned_files)}")
        print(f"  Required map:  {len(scan.codepoints)} entries")

        if scan.unsupported_two_byte:
            print(
                "  Warning: two-byte UTF-8 characters are unsupported by the current "
                "renderer: " + _format_codepoints(scan.unsupported_two_byte)
            )
        if scan.unsupported_non_bmp:
            print(
                "  Warning: non-BMP characters exceed uint16_t: "
                + _format_codepoints(scan.unsupported_non_bmp)
            )

        existing_by_size: dict[int, ExistingFont] = {}
        required_by_size: dict[int, set[int]] = {}
        missing_any = False
        unsafe_existing_any = False
        report_fonts: list[dict[str, object]] = []

        for size in sizes:
            symbol = f"{family}_{size}pt8b"
            target = _target_path(config, family, size)
            existing = _parse_existing_font(target, symbol)
            required = set(scan.codepoints)
            if args.preserve_existing:
                required.update(existing.codepoints)
            missing = required - set(existing.codepoints)
            unused = set(existing.codepoints) - required
            missing_any |= bool(missing)
            bitmap_overflow = existing.bitmap_bytes > MAX_BITMAP_OFFSET
            unsafe_existing_any |= bitmap_overflow
            existing_by_size[size] = existing
            required_by_size[size] = required

            print(f"\n  {size}pt: {target.relative_to(PROJECT_ROOT)}")
            print(
                f"    existing={len(existing.codepoints)}, required={len(required)}, "
                f"missing={len(missing)}, removable={len(unused)}, "
                f"bitmap={existing.bitmap_bytes} bytes, "
                f"offset-safe={'no' if bitmap_overflow else 'yes'}"
            )
            if bitmap_overflow:
                print(
                    f"    unsafe: bitmap payload exceeds the {MAX_BITMAP_OFFSET}-byte "
                    "GFXglyph offset limit"
                )
            if missing:
                print("    missing: " + _format_codepoints(missing))

            report_fonts.append(
                {
                    "size": size,
                    "target": str(target),
                    "existing_count": len(existing.codepoints),
                    "required_count": len(required),
                    "missing": [f"U+{value:04X}" for value in sorted(missing)],
                    "removable_count": len(unused),
                    "existing_bitmap_bytes": existing.bitmap_bytes,
                    "existing_offset_safe": not bitmap_overflow,
                }
            )

        generated_headers: dict[Path, str] = {}
        build_results: dict[int, FontBuildResult] = {}
        if args.apply:
            # Generate and validate every requested size before replacing any
            # project file, so conversion failures leave the workspace intact.
            for size in sizes:
                symbol = f"{family}_{size}pt8b"
                result = build_font_header(
                    font_path=source_font,
                    font_index=args.font_index,
                    point_size=size,
                    dpi=dpi,
                    symbol=symbol,
                    codepoints=required_by_size[size],
                )
                target = (
                    args.output_dir.resolve() / existing_by_size[size].path.name
                    if args.output_dir
                    else existing_by_size[size].path
                )
                generated_headers[target] = result.header
                build_results[size] = result
                print(
                    f"\n  Generated {size}pt: glyphs={result.glyph_count}, "
                    f"bitmap={result.bitmap_bytes} bytes, "
                    f"approx={result.approximate_bytes} bytes"
                )

            _atomic_replace_many(generated_headers)
            action = "Staged" if args.output_dir else "Updated"
            print(f"\n{action} {len(generated_headers)} font header(s) atomically.")

            # Parse the actual files again; this catches output-format or write
            # regressions independently of the in-memory generator result.
            for size in sizes:
                symbol = f"{family}_{size}pt8b"
                actual_path = (
                    args.output_dir.resolve() / existing_by_size[size].path.name
                    if args.output_dir
                    else existing_by_size[size].path
                )
                actual = _parse_existing_font(actual_path, symbol)
                remaining = required_by_size[size] - set(actual.codepoints)
                if remaining:
                    raise FontConversionError(
                        f"Post-write validation failed for {size}pt: "
                        + _format_codepoints(remaining)
                    )
            missing_any = False
            unsafe_existing_any = False

        report = {
            "family": family,
            "source_font": str(source_font),
            "active_locale": scan.active_locale,
            "scanned_files": len(scan.scanned_files),
            "required": [f"U+{value:04X}" for value in sorted(scan.codepoints)],
            "unsupported_two_byte": [
                f"U+{value:04X}" for value in sorted(scan.unsupported_two_byte)
            ],
            "unsupported_non_bmp": [
                f"U+{value:06X}" for value in sorted(scan.unsupported_non_bmp)
            ],
            "fonts": report_fonts,
            "applied": args.apply,
            "generated": {
                str(size): result.report() for size, result in build_results.items()
            },
        }
        if args.report_json:
            report_path = args.report_json.resolve()
            report_path.parent.mkdir(parents=True, exist_ok=True)
            report_path.write_text(
                json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
                newline="\n",
            )
            print(f"Report: {report_path}")

        if (missing_any or unsafe_existing_any) and not args.apply:
            print(
                "\nFont update required. Run with --apply to repair missing glyphs "
                "or unsafe bitmap offsets."
            )
            return 2

        if args.build:
            environment = args.environment or config.get(
                "platformio_environment", "dfrobot_firebeetle2_esp32e"
            )
            if not isinstance(environment, str) or not environment:
                raise FontConversionError("platformio_environment must be a string.")
            _run_build(environment)
        print("\nFont sync completed successfully.")
        return 0
    except (FontConversionError, OSError, subprocess.CalledProcessError) as exc:
        print(f"update_fonts: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
