#!/usr/bin/env python3
"""Generate deterministic, project-owned Bedrock fixtures for the S00 gate."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import random
import struct
from pathlib import Path
from typing import Any


GENERATOR_VERSION = 4
FIXTURE_SEED = 20260731
FIXTURES = (
    {
        "file": "bedrock_s00_small_8cubes.geo.json",
        "identifier": "geometry.xpbd_s00_small_8cubes",
        "branches": 2,
        "depth": 2,
        "cubes_per_bone": 2,
        "role": "small_control",
    },
    {
        "file": "bedrock_s00_stress_1280cubes.geo.json",
        "identifier": "geometry.xpbd_s00_stress_1280cubes",
        "branches": 40,
        "depth": 16,
        "cubes_per_bone": 2,
        "role": "primary_stress",
    },
    {
        "file": "bedrock_s00_stress_2624cubes.geo.json",
        "identifier": "geometry.xpbd_s00_stress_2624cubes",
        "branches": 82,
        "depth": 16,
        "cubes_per_bone": 2,
        "role": "extended_stress",
    },
    {
        "file": "bedrock_s00_stress_10368cubes_144bones.geo.json",
        "identifier": "geometry.xpbd_s00_stress_10368cubes_144bones",
        "branches": 12,
        "depth": 12,
        "cubes_per_bone": 72,
        "role": "user_scale_cube_density_stress",
    },
)

ROTATION_ANIMATIONS = (
    {
        "file": "bedrock_s00_rotation_3deg_per_60hz.animation.json",
        "identifier": "animation.xpbd_s00_rotation_3deg_per_60hz",
        "target_bone": "branch_000_depth_00",
        "nominal_fps": 60,
        "degrees_per_frame": 3.0,
        "axis": "y",
        "animation_length_seconds": 2.0,
        "role": "rotating_fg_motion_control",
    },
)

MATERIAL_TEXTURES = (
    {
        "file": "bedrock_s00_alpha_emissive.png",
        "material_set": "bedrock_s00_alpha_emissive",
        "role": "alpha_blended_base_color_control",
        "semantic": "base_color_rgba8",
        "rgba": (224, 96, 32, 128),
        "paired_file": "bedrock_s00_alpha_emissive_s.png",
        "alpha_semantic": "fixed_alpha_128_blended",
    },
    {
        "file": "bedrock_s00_alpha_emissive_s.png",
        "material_set": "bedrock_s00_alpha_emissive",
        "role": "labpbr_full_emission_control",
        "semantic": "labpbr_specular_rgba8",
        "rgba": (0, 0, 0, 254),
        "paired_file": "bedrock_s00_alpha_emissive.png",
        "alpha_semantic": "labpbr_emission_strength_1",
    },
)

MATERIAL_WIDTH = 16
MATERIAL_HEIGHT = 16


def _number(value: float) -> int | float:
    rounded = round(value, 4)
    return int(rounded) if rounded.is_integer() else rounded


def build_fixture(spec: dict[str, Any]) -> dict[str, Any]:
    rng = random.Random(
        FIXTURE_SEED
        + int(spec["branches"]) * 101
        + int(spec["depth"]) * 17
        + int(spec["cubes_per_bone"])
    )
    bones: list[dict[str, Any]] = []
    branches = int(spec["branches"])
    depth = int(spec["depth"])
    cubes_per_bone = int(spec["cubes_per_bone"])
    columns = min(10, branches)

    for branch in range(branches):
        grid_x = branch % columns
        grid_z = branch // columns
        base_x = (grid_x - (columns - 1) * 0.5) * 5.0
        base_z = grid_z * 5.0
        parent: str | None = None

        for level in range(depth):
            name = f"branch_{branch:03d}_depth_{level:02d}"
            pivot = [base_x, level * 2.25, base_z]
            bone: dict[str, Any] = {
                "name": name,
                "pivot": [_number(v) for v in pivot],
            }
            if parent is not None:
                bone["parent"] = parent
            if level > 0:
                bone["rotation"] = [
                    _number(rng.uniform(-2.0, 2.0)),
                    _number(rng.uniform(-4.0, 4.0)),
                    _number(rng.uniform(-2.0, 2.0)),
                ]

            cubes: list[dict[str, Any]] = []
            for cube_index in range(cubes_per_bone):
                if cubes_per_bone <= 2:
                    offset_x = -0.85 if cube_index % 2 == 0 else 0.35
                    offset_z = -0.6
                else:
                    cube_columns = min(12, cubes_per_bone)
                    cube_rows = (cubes_per_bone + cube_columns - 1) // cube_columns
                    offset_x = (
                        cube_index % cube_columns - (cube_columns - 1) * 0.5
                    ) * 1.15
                    offset_z = (
                        cube_index // cube_columns - (cube_rows - 1) * 0.5
                    ) * 1.15
                origin = [
                    base_x + offset_x,
                    level * 2.25 - 0.6,
                    base_z + offset_z,
                ]
                cubes.append(
                    {
                        "origin": [_number(v) for v in origin],
                        "size": [1.1, 1.2, 1.2],
                        "uv": [0, 0],
                    }
                )
            bone["cubes"] = cubes
            bones.append(bone)
            parent = name

    return {
        "format_version": "1.12.0",
        "minecraft:geometry": [
            {
                "description": {
                    "identifier": spec["identifier"],
                    "texture_width": 16,
                    "texture_height": 16,
                },
                "bones": bones,
            }
        ],
    }


def encode_fixture(document: dict[str, Any]) -> bytes:
    return (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def _png_chunk(chunk_type: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(chunk_type + payload) & 0xFFFFFFFF
    return (
        struct.pack(">I", len(payload))
        + chunk_type
        + payload
        + struct.pack(">I", checksum)
    )


def _adler32(payload: bytes) -> int:
    modulo = 65521
    a = 1
    b = 0
    for value in payload:
        a = (a + value) % modulo
        b = (b + a) % modulo
    return (b << 16) | a


def encode_uniform_rgba_png(width: int, height: int, rgba: tuple[int, ...]) -> bytes:
    if width <= 0 or height <= 0 or len(rgba) != 4:
        raise ValueError("RGBA PNG dimensions and pixel must be valid")
    if any(channel < 0 or channel > 255 for channel in rgba):
        raise ValueError("RGBA channels must be in [0, 255]")

    scanline = b"\x00" + bytes(rgba) * width
    raw = scanline * height
    if len(raw) > 0xFFFF:
        raise ValueError("deterministic stored-DEFLATE encoder supports at most 65535 bytes")

    stored_block = (
        b"\x01"
        + struct.pack("<H", len(raw))
        + struct.pack("<H", (~len(raw)) & 0xFFFF)
        + raw
    )
    zlib_stream = b"\x78\x01" + stored_block + struct.pack(">I", _adler32(raw))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib_stream)
        + _png_chunk(b"IEND", b"")
    )


def build_rotation_animation(spec: dict[str, Any]) -> dict[str, Any]:
    fps = int(spec["nominal_fps"])
    degrees_per_frame = float(spec["degrees_per_frame"])
    length = float(spec["animation_length_seconds"])
    degrees_per_second = fps * degrees_per_frame
    return {
        "format_version": "1.8.0",
        "animations": {
            spec["identifier"]: {
                "loop": True,
                "animation_length": _number(length),
                "bones": {
                    spec["target_bone"]: {
                        "rotation": {
                            "0.0": [0, 0, 0],
                            "1.0": [0, _number(degrees_per_second), 0],
                            "2.0": [0, _number(degrees_per_second * 2.0), 0],
                        }
                    }
                },
            }
        },
    }


def fixture_record(spec: dict[str, Any], payload: bytes) -> dict[str, Any]:
    bone_count = int(spec["branches"]) * int(spec["depth"])
    cube_count = bone_count * int(spec["cubes_per_bone"])
    return {
        "file": spec["file"],
        "identifier": spec["identifier"],
        "role": spec["role"],
        "format_version": "1.12.0",
        "seed": FIXTURE_SEED,
        "branch_count": int(spec["branches"]),
        "hierarchy_depth": int(spec["depth"]),
        "bone_count": bone_count,
        "cubes_per_bone": int(spec["cubes_per_bone"]),
        "cube_count": cube_count,
        "byte_count": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest().upper(),
        "source": "project-owned deterministic generator",
        "license_spdx": "Apache-2.0",
    }


def animation_record(spec: dict[str, Any], payload: bytes) -> dict[str, Any]:
    return {
        "file": spec["file"],
        "identifier": spec["identifier"],
        "role": spec["role"],
        "format_version": "1.8.0",
        "seed": FIXTURE_SEED,
        "target_bone": spec["target_bone"],
        "nominal_fps": int(spec["nominal_fps"]),
        "degrees_per_frame": float(spec["degrees_per_frame"]),
        "axis": spec["axis"],
        "animation_length_seconds": float(spec["animation_length_seconds"]),
        "byte_count": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest().upper(),
        "source": "project-owned deterministic generator",
        "license_spdx": "Apache-2.0",
    }


def material_record(spec: dict[str, Any], payload: bytes) -> dict[str, Any]:
    return {
        "file": spec["file"],
        "material_set": spec["material_set"],
        "role": spec["role"],
        "semantic": spec["semantic"],
        "format": "PNG",
        "width": MATERIAL_WIDTH,
        "height": MATERIAL_HEIGHT,
        "pixel_format": "RGBA8",
        "uniform_rgba": list(spec["rgba"]),
        "paired_file": spec["paired_file"],
        "alpha_semantic": spec["alpha_semantic"],
        "byte_count": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest().upper(),
        "source": "project-owned deterministic generator",
        "license_spdx": "Apache-2.0",
    }


def build_manifest(
    fixture_records: list[dict[str, Any]],
    animation_records: list[dict[str, Any]],
    material_records: list[dict[str, Any]],
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "generator_version": GENERATOR_VERSION,
        "generator": "tools/scene_tests/generate_s00_bedrock_fixtures.py",
        "seed": FIXTURE_SEED,
        "source": "project-owned deterministic generator",
        "license_spdx": "Apache-2.0",
        "fixtures": fixture_records,
        "animations": animation_records,
        "materials": material_records,
    }


def compare_manifest(actual: dict[str, Any], expected_path: Path) -> None:
    expected = json.loads(expected_path.read_text(encoding="utf-8"))
    if expected != actual:
        raise SystemExit(
            f"fixture manifest mismatch: regenerate deliberately with "
            f"--write-manifest {expected_path} and review the diff"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    manifest_group = parser.add_mutually_exclusive_group()
    manifest_group.add_argument("--verify-manifest", type=Path)
    manifest_group.add_argument("--write-manifest", type=Path)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, Any]] = []
    for spec in FIXTURES:
        document = build_fixture(spec)
        payload = encode_fixture(document)
        output = args.output_dir / str(spec["file"])
        output.write_bytes(payload)
        records.append(fixture_record(spec, payload))

    animation_records: list[dict[str, Any]] = []
    for spec in ROTATION_ANIMATIONS:
        document = build_rotation_animation(spec)
        payload = encode_fixture(document)
        output = args.output_dir / str(spec["file"])
        output.write_bytes(payload)
        animation_records.append(animation_record(spec, payload))

    material_records: list[dict[str, Any]] = []
    for spec in MATERIAL_TEXTURES:
        payload = encode_uniform_rgba_png(
            MATERIAL_WIDTH, MATERIAL_HEIGHT, tuple(spec["rgba"])
        )
        output = args.output_dir / str(spec["file"])
        output.write_bytes(payload)
        material_records.append(material_record(spec, payload))

    manifest = build_manifest(records, animation_records, material_records)
    if args.verify_manifest:
        compare_manifest(manifest, args.verify_manifest)
    elif args.write_manifest:
        args.write_manifest.parent.mkdir(parents=True, exist_ok=True)
        args.write_manifest.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
            newline="\n",
        )

    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
