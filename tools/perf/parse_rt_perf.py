#!/usr/bin/env python3
"""Extract the latest unattended Vulkan RT PERF00 run from xpbd_baker.log."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from pathlib import Path
from typing import Any, Iterable


RUN_START = "Performance diagnostics enabled"
RUN_END = "Unattended frame limit reached"
PERF_MARKER = "VKDIAG rt_perf "

FLOAT_FIELDS = (
    "cpu_backend_ms",
    "cpu_scene_assembly_ms",
    "cpu_scene_hash_ms",
    "cpu_emitter_distribution_ms",
    "cpu_descriptor_update_ms",
    "gpu_total_ms",
    "gpu_as_build_ms",
    "gpu_path_trace_ms",
)
INTEGER_FIELDS = (
    "frame",
    "slot",
    "gpu_timestamp_valid",
    "rt_upload_bytes",
    "rt_emitter_distribution_rebuilds",
    "rt_descriptor_write_calls",
    "rt_descriptor_entries_written",
    "rt_descriptor_cache_hits",
    "rt_blas_full_builds",
    "rt_blas_refits",
    "rt_tlas_full_builds",
    "rt_tlas_updates",
    "rt_allocated_bytes",
)
TEXT_FIELDS = (
    "timestamp",
    "rt_aov_write_mask",
    "last_build_reason",
    "last_tlas_reason",
)
CSV_FIELDS = ("sample",) + INTEGER_FIELDS + FLOAT_FIELDS + TEXT_FIELDS

KEY_PATTERN = re.compile(r"(?P<key>[a-z_]+)=")
TIMESTAMP_PATTERN = re.compile(r"^\[(?P<timestamp>[^]]+)]")
ERROR_PATTERNS = (
    "VK_ERROR_DEVICE_LOST",
    "Device Lost",
    "Validation Error",
    "validation error",
)


def read_log_set(path: Path) -> tuple[list[str], list[str]]:
    """Read spdlog rotations from oldest to newest, followed by the live log."""
    rotations: list[Path] = []
    for index in range(1, 100):
        candidate = path.with_name(f"{path.stem}.{index}{path.suffix}")
        if candidate.exists():
            rotations.append(candidate)
    sources = list(reversed(rotations)) + [path]
    lines: list[str] = []
    for source in sources:
        lines.extend(
            source.read_text(encoding="utf-8", errors="replace").splitlines()
        )
    return lines, [str(source) for source in sources]


def latest_run(lines: list[str]) -> tuple[list[str], int, int]:
    start = 0
    for index, line in enumerate(lines):
        if RUN_START in line:
            start = index
    end = len(lines)
    for index in range(start, len(lines)):
        if RUN_END in lines[index]:
            end = index + 1
            break
    return lines[start:end], start + 1, end


def parse_key_values(payload: str) -> dict[str, str]:
    matches = list(KEY_PATTERN.finditer(payload))
    values: dict[str, str] = {}
    for index, match in enumerate(matches):
        value_start = match.end()
        value_end = matches[index + 1].start() if index + 1 < len(matches) else len(payload)
        values[match.group("key")] = payload[value_start:value_end].strip()
    return values


def parse_records(lines: Iterable[str]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for line in lines:
        marker = line.find(PERF_MARKER)
        if marker < 0:
            continue
        values = parse_key_values(line[marker + len(PERF_MARKER) :])
        timestamp_match = TIMESTAMP_PATTERN.match(line)
        record: dict[str, Any] = {
            "sample": len(records),
            "timestamp": timestamp_match.group("timestamp") if timestamp_match else "",
        }
        for field in INTEGER_FIELDS:
            text = values.get(field, "0")
            record[field] = int(text, 0)
        for field in FLOAT_FIELDS:
            record[field] = float(values.get(field, "nan"))
        for field in TEXT_FIELDS[1:]:
            record[field] = values.get(field, "")
        records.append(record)
    return records


def percentile(values: list[float], probability: float) -> float | None:
    finite = sorted(value for value in values if math.isfinite(value))
    if not finite:
        return None
    if len(finite) == 1:
        return finite[0]
    position = probability * (len(finite) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    fraction = position - lower
    return finite[lower] * (1.0 - fraction) + finite[upper] * fraction


def metric_summary(records: list[dict[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for field in FLOAT_FIELDS:
        values = [float(record[field]) for record in records]
        finite = [value for value in values if math.isfinite(value)]
        result[field] = {
            "p50": percentile(finite, 0.50),
            "p95": percentile(finite, 0.95),
            "p99": percentile(finite, 0.99),
            "max": max(finite) if finite else None,
        }
    return result


def build_summary(
    records: list[dict[str, Any]], steady_records: list[dict[str, Any]]
) -> dict[str, Any]:
    final = records[-1] if records else {}
    return {
        "sample_count": len(records),
        "steady_sample_count": len(steady_records),
        "frame_range": [records[0]["frame"], records[-1]["frame"]]
        if records
        else [],
        "valid_gpu_samples": sum(
            int(record["gpu_timestamp_valid"]) for record in records
        ),
        "all_samples": metric_summary(records),
        "steady_samples": metric_summary(steady_records),
        "totals": {
            "rt_upload_bytes": sum(record["rt_upload_bytes"] for record in records),
            "rt_upload_nonzero_frames": sum(
                record["rt_upload_bytes"] > 0 for record in records
            ),
            "emitter_distribution_rebuild_events": (
                records[0]["rt_emitter_distribution_rebuilds"]
                + sum(
                    max(
                        0,
                        records[index]["rt_emitter_distribution_rebuilds"]
                        - records[index - 1][
                            "rt_emitter_distribution_rebuilds"
                        ],
                    )
                    for index in range(1, len(records))
                )
                if records
                else 0
            ),
            "descriptor_write_calls": sum(
                record["rt_descriptor_write_calls"] for record in records
            ),
            "descriptor_entries_written": sum(
                record["rt_descriptor_entries_written"] for record in records
            ),
            "descriptor_cache_hits": sum(
                record["rt_descriptor_cache_hits"] for record in records
            ),
        },
        "final_cumulative_counters": {
            key: final.get(key, 0)
            for key in (
                "rt_blas_full_builds",
                "rt_blas_refits",
                "rt_tlas_full_builds",
                "rt_tlas_updates",
                "rt_allocated_bytes",
            )
        },
        "aov_write_masks": sorted(
            {record["rt_aov_write_mask"] for record in records}
        ),
    }


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(records)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument(
        "--warmup",
        type=int,
        default=10,
        help="number of leading samples excluded from steady-state percentiles",
    )
    args = parser.parse_args()

    lines, source_logs = read_log_set(args.log)
    run_lines, start_line, end_line = latest_run(lines)
    records = parse_records(run_lines)
    if not records:
        parser.error(f"no {PERF_MARKER.strip()} records found in {args.log}")
    warmup = max(0, min(args.warmup, len(records)))
    steady_records = records[warmup:]
    errors = [
        line
        for line in run_lines
        if any(pattern in line for pattern in ERROR_PATTERNS)
    ]
    document = {
        "schema": "xpbd.perf00.rt_perf.v1",
        "source_logs": source_logs,
        "latest_run_source_lines": [start_line, end_line],
        "warmup_samples_excluded": warmup,
        "summary": build_summary(records, steady_records),
        "error_lines": errors,
        "records": records,
    }

    write_csv(args.csv, records)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(document["summary"], ensure_ascii=False, indent=2))
    if errors:
        print(f"warning: {len(errors)} validation/device-loss line(s) detected")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
