#!/usr/bin/env python3
"""Generate deterministic, project-owned Bedrock fixtures for the S00 gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import random
from pathlib import Path
from typing import Any


GENERATOR_VERSION = 2
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


def build_manifest(records: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "generator_version": GENERATOR_VERSION,
        "generator": "tools/scene_tests/generate_s00_bedrock_fixtures.py",
        "seed": FIXTURE_SEED,
        "source": "project-owned deterministic generator",
        "license_spdx": "Apache-2.0",
        "fixtures": records,
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

    manifest = build_manifest(records)
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
