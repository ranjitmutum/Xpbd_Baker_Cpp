#!/usr/bin/env python3
"""Cross-platform rebuild and verification for checked-in Vulkan SPIR-V.

The checker deliberately compiles into a temporary directory.  A successful
check proves four independent contracts:

* GLSL plus its recursively included sources reproduces the checked-in .spv.
* The uint32 words in .spv.inc reproduce the checked-in .spv byte-for-byte.
* Every source/artifact hash agrees with tools/spirv_manifest.json.
* glslang's bundled SPIRV-Tools validator accepts every generated module;
  a standalone spirv-val performs a second validation pass when available.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Any, Iterable


MANIFEST_SCHEMA = "xpbd-spirv-manifest/1"
SHADER_DIRECTORY = Path("src/gfx/spirv")
COMPILED_SUFFIXES = {
    ".vert",
    ".frag",
    ".comp",
    ".rgen",
    ".rmiss",
    ".rchit",
    ".rahit",
}
RAY_TRACING_SUFFIXES = {".rgen", ".rmiss", ".rchit", ".rahit"}
INCLUDE_PATTERN = re.compile(r'^\s*#include\s+"([^"]+)"', re.MULTILINE)
INCLUDE_WORD_PATTERN = re.compile(r"\b0x([0-9a-fA-F]{8})u\b")
SPIRV_MAGIC = b"\x03\x02\x23\x07"


class SpirvCheckError(RuntimeError):
    """Expected verification failure with a concise user-facing message."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def text_sha256(path: Path) -> str:
    """Hash text deterministically across Git CRLF/LF checkout policies."""
    canonical = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return hashlib.sha256(canonical).hexdigest()


def repository_path(root: Path, relative: str | Path) -> Path:
    path = (root / relative).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise SpirvCheckError(
            f"manifest path escapes the repository: {relative}"
        ) from error
    return path


def relative_name(root: Path, path: Path) -> str:
    return path.resolve().relative_to(root).as_posix()


def target_environment(source: Path) -> str:
    name = source.name
    if (
        source.suffix in RAY_TRACING_SUFFIXES
        or name == "path_trace.comp"
        or re.search(r"_rt\.(vert|frag)$", name)
    ):
        return "vulkan1.2"
    return "vulkan1.0"


def resolve_include(root: Path, including_file: Path, include: str) -> Path:
    candidates = (root / include, including_file.parent / include)
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_file():
            try:
                relative = resolved.relative_to(root)
            except ValueError as error:
                raise SpirvCheckError(
                    f"{relative_name(root, including_file)} includes a file outside "
                    f"the repository: {include!r}"
                ) from error
            repository_path(root, relative)
            return resolved
    raise SpirvCheckError(
        f"{relative_name(root, including_file)} includes missing file {include!r}"
    )


def recursive_includes(root: Path, sources: Iterable[Path]) -> set[Path]:
    dependencies: set[Path] = set()
    visited: set[Path] = set()

    def visit(path: Path) -> None:
        resolved = path.resolve()
        if resolved in visited:
            return
        visited.add(resolved)
        text = resolved.read_text(encoding="utf-8")
        for include in INCLUDE_PATTERN.findall(text):
            dependency = resolve_include(root, resolved, include)
            dependencies.add(dependency)
            visit(dependency)

    for source in sources:
        visit(source)
    return dependencies


def discover_layout(root: Path) -> tuple[list[dict[str, str]], list[Path]]:
    shader_directory = root / SHADER_DIRECTORY
    if not shader_directory.is_dir():
        raise SpirvCheckError(f"shader directory is missing: {shader_directory}")
    sources = sorted(
        (
            path.resolve()
            for path in shader_directory.iterdir()
            if path.is_file() and path.suffix in COMPILED_SUFFIXES
        ),
        key=lambda path: relative_name(root, path),
    )
    if not sources:
        raise SpirvCheckError(f"no shader sources found below {shader_directory}")

    dependencies = recursive_includes(root, sources)
    # Include-only GLSL files in the shader directory are part of the source
    # contract even before a new compiled shader starts including them.
    dependencies.update(path.resolve() for path in shader_directory.glob("*.glsl"))
    dependencies.difference_update(sources)

    entries: list[dict[str, str]] = []
    for source in sources:
        source_name = relative_name(root, source)
        entries.append(
            {
                "source": source_name,
                "target_env": target_environment(source),
                "spv": f"{source_name}.spv",
                "inc": f"{source_name}.spv.inc",
            }
        )
    return entries, sorted(dependencies, key=lambda path: relative_name(root, path))


def manifest_data(root: Path) -> dict[str, Any]:
    entries, dependencies = discover_layout(root)
    hashed_entries: list[dict[str, str]] = []
    for entry in entries:
        source = repository_path(root, entry["source"])
        spv = repository_path(root, entry["spv"])
        inc = repository_path(root, entry["inc"])
        for path in (source, spv, inc):
            if not path.is_file():
                raise SpirvCheckError(
                    f"required shader artifact is missing: {relative_name(root, path)}"
                )
        hashed_entries.append(
            {
                **entry,
                "source_sha256": text_sha256(source),
                "spv_sha256": sha256(spv),
                "inc_sha256": text_sha256(inc),
            }
        )
    return {
        "schema": MANIFEST_SCHEMA,
        "compiler_contract": {
            "arguments": [
                "-V",
                "--spirv-val",
                "--target-env",
                "<entry.target_env>",
                "-I<repo-root>",
            ],
            "binary_comparison": "byte-for-byte",
            "include_encoding": "little-endian uint32",
            "text_hash_line_endings": "LF-normalized",
        },
        "entries": hashed_entries,
        "dependencies": [
            {
                "path": relative_name(root, dependency),
                "sha256": text_sha256(dependency),
            }
            for dependency in dependencies
        ],
    }


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise SpirvCheckError(
            f"SPIR-V hash manifest is missing: {path}\n"
            "Run tools/spirv_tool.py refresh-manifest after rebuilding shaders."
        )
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SpirvCheckError(f"failed to read SPIR-V manifest {path}: {error}") from error
    schema = data.get("schema") if isinstance(data, dict) else None
    if not isinstance(data, dict) or schema != MANIFEST_SCHEMA:
        raise SpirvCheckError(
            f"unsupported SPIR-V manifest schema in {path}: {schema!r}"
        )
    return data


def validate_manifest(root: Path, actual: dict[str, Any]) -> list[str]:
    try:
        expected = manifest_data(root)
    except SpirvCheckError as error:
        return [str(error)]
    errors: list[str] = []
    if actual.get("compiler_contract") != expected["compiler_contract"]:
        errors.append("manifest compiler_contract does not match the checker")
    actual_entries = actual.get("entries")
    actual_dependencies = actual.get("dependencies")
    if not isinstance(actual_entries, list):
        return ["manifest field 'entries' must be an array"]
    if not isinstance(actual_dependencies, list):
        return ["manifest field 'dependencies' must be an array"]

    def keyed(items: list[Any], key: str) -> dict[str, dict[str, Any]]:
        result: dict[str, dict[str, Any]] = {}
        for item in items:
            if not isinstance(item, dict) or not isinstance(item.get(key), str):
                errors.append(f"manifest item is missing string field {key!r}")
                continue
            name = item[key]
            if name in result:
                errors.append(f"manifest contains duplicate {key} {name!r}")
            result[name] = item
        return result

    expected_entries = keyed(expected["entries"], "source")
    indexed_entries = keyed(actual_entries, "source")
    if expected_entries.keys() != indexed_entries.keys():
        missing = sorted(expected_entries.keys() - indexed_entries.keys())
        extra = sorted(indexed_entries.keys() - expected_entries.keys())
        if missing:
            errors.append("manifest is missing shaders: " + ", ".join(missing))
        if extra:
            errors.append("manifest has unknown shaders: " + ", ".join(extra))
    for source in sorted(expected_entries.keys() & indexed_entries.keys()):
        expected_entry = expected_entries[source]
        actual_entry = indexed_entries[source]
        for field in (
            "target_env",
            "spv",
            "inc",
            "source_sha256",
            "spv_sha256",
            "inc_sha256",
        ):
            if actual_entry.get(field) != expected_entry[field]:
                errors.append(
                    f"manifest mismatch {source} {field}: "
                    f"expected {expected_entry[field]!r}, got {actual_entry.get(field)!r}"
                )

    expected_dependencies = keyed(expected["dependencies"], "path")
    indexed_dependencies = keyed(actual_dependencies, "path")
    if expected_dependencies.keys() != indexed_dependencies.keys():
        missing = sorted(expected_dependencies.keys() - indexed_dependencies.keys())
        extra = sorted(indexed_dependencies.keys() - expected_dependencies.keys())
        if missing:
            errors.append("manifest is missing includes: " + ", ".join(missing))
        if extra:
            errors.append("manifest has unknown includes: " + ", ".join(extra))
    for path in sorted(expected_dependencies.keys() & indexed_dependencies.keys()):
        expected_hash = expected_dependencies[path]["sha256"]
        actual_hash = indexed_dependencies[path].get("sha256")
        if actual_hash != expected_hash:
            errors.append(
                f"manifest mismatch {path} sha256: "
                f"expected {expected_hash}, got {actual_hash}"
            )
    return errors


def spirv_from_include(path: Path) -> bytes:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise SpirvCheckError(f"failed to read {path}: {error}") from error
    words = INCLUDE_WORD_PATTERN.findall(text)
    if not words:
        raise SpirvCheckError(f"SPIR-V include contains no uint32 words: {path}")
    return b"".join(struct.pack("<I", int(word, 16)) for word in words)


def validate_spirv_bytes(path: Path, data: bytes) -> None:
    if len(data) < 20 or len(data) % 4 != 0:
        raise SpirvCheckError(
            f"SPIR-V binary has invalid byte length {len(data)}: {path}"
        )
    if data[:4] != SPIRV_MAGIC:
        raise SpirvCheckError(f"SPIR-V binary has invalid magic: {path}")


def validate_artifact_pairs(root: Path, entries: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    for entry in entries:
        spv = repository_path(root, entry["spv"])
        inc = repository_path(root, entry["inc"])
        try:
            binary = spv.read_bytes()
            validate_spirv_bytes(spv, binary)
            embedded = spirv_from_include(inc)
            if embedded != binary:
                errors.append(
                    f"embedded SPIR-V is stale: {entry['inc']} does not match "
                    f"{entry['spv']}"
                )
        except (OSError, SpirvCheckError) as error:
            errors.append(str(error))
    return errors


def run_process(command: list[str], root: Path, description: str) -> None:
    process = subprocess.run(
        command,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if process.returncode != 0:
        output = process.stdout.strip()
        raise SpirvCheckError(
            f"{description} failed with exit code {process.returncode}"
            + (f":\n{output}" if output else "")
        )


def compile_shaders(
    root: Path,
    entries: list[dict[str, str]],
    glslang: Path,
    spirv_val: Path | None,
    output_directory: Path,
) -> dict[str, Path]:
    outputs: dict[str, Path] = {}
    for index, entry in enumerate(entries):
        source = repository_path(root, entry["source"])
        output = output_directory / f"{index:03d}-{source.name}.spv"
        command = [
            str(glslang),
            "-V",
            "--spirv-val",
            "--target-env",
            entry["target_env"],
            f"-I{root}",
            "-o",
            str(output),
            str(source),
        ]
        run_process(command, root, f"glslangValidator for {entry['source']}")
        generated = output.read_bytes()
        validate_spirv_bytes(output, generated)
        if spirv_val is not None:
            run_process(
                [
                    str(spirv_val),
                    "--target-env",
                    entry["target_env"],
                    str(output),
                ],
                root,
                f"spirv-val for {entry['source']}",
            )
        outputs[entry["source"]] = output
    return outputs


def resolve_tool(
    explicit: str | None,
    environment_variable: str,
    bundled_candidates: Iterable[Path],
    executable_names: Iterable[str],
    required: bool,
) -> Path | None:
    executable_names = tuple(executable_names)
    configured = explicit or os.environ.get(environment_variable)
    if configured:
        path = Path(configured).expanduser().resolve()
        if path.is_file():
            return path
        origin = "command line" if explicit else environment_variable
        raise SpirvCheckError(
            f"{origin} selected a missing tool: {configured}"
        )

    candidates: list[str | Path] = []
    candidates.extend(bundled_candidates)
    for name in executable_names:
        discovered = shutil.which(name)
        if discovered:
            candidates.append(discovered)
    for candidate in candidates:
        path = Path(candidate).expanduser().resolve()
        if path.is_file():
            return path
    if required:
        names = ", ".join(executable_names)
        raise SpirvCheckError(
            f"required tool was not found ({names}); install it, set "
            f"{environment_variable}, or pass an explicit command-line path"
        )
    return None


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=path.name, dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def include_text(source_name: str, binary: bytes, existing: Path | None) -> str:
    validate_spirv_bytes(Path(source_name), binary)
    header = (
        f"// Generated from {Path(source_name).name} by tools/spirv_tool.py. "
        "Do not edit by hand."
    )
    if existing is not None and existing.is_file():
        first_line = existing.read_text(encoding="utf-8").splitlines()[0]
        if first_line.startswith(f"// Generated from {Path(source_name).name} "):
            # Preserve the historical generator label to avoid header-only
            # churn when migrating from build_spirv.ps1.
            header = first_line
    words = struct.unpack(f"<{len(binary) // 4}I", binary)
    lines = [header]
    for offset in range(0, len(words), 6):
        lines.append(
            "  " + ", ".join(f"0x{word:08x}u" for word in words[offset : offset + 6]) + ","
        )
    return "\n".join(lines) + "\n"


def write_manifest(path: Path, data: dict[str, Any]) -> None:
    encoded = (json.dumps(data, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    atomic_write(path, encoded)


def check_repository(
    root: Path, manifest_path: Path, glslang: Path, spirv_val: Path | None
) -> None:
    actual_manifest = load_manifest(manifest_path)
    errors = validate_manifest(root, actual_manifest)
    entries, _ = discover_layout(root)
    errors.extend(validate_artifact_pairs(root, entries))
    with tempfile.TemporaryDirectory(prefix="xpbd-spirv-check-") as directory:
        try:
            outputs = compile_shaders(
                root, entries, glslang, spirv_val, Path(directory)
            )
            for entry in entries:
                checked_in = repository_path(root, entry["spv"])
                generated = outputs[entry["source"]]
                if generated.read_bytes() != checked_in.read_bytes():
                    errors.append(
                        f"compiled SPIR-V is stale: {entry['source']} does not "
                        f"reproduce {entry['spv']}"
                    )
        except SpirvCheckError as error:
            errors.append(str(error))
    if errors:
        formatted = "\n  ".join(errors)
        raise SpirvCheckError(
            "SPIR-V verification failed:\n  "
            + formatted
            + "\nRun tools/spirv_tool.py rebuild and rebuild the application."
        )
    validation = f"{glslang} --spirv-val"
    if spirv_val is not None:
        validation += f" + external {spirv_val}"
    print(
        f"SPIR-V verification passed for {len(entries)} shaders; "
        f"validation: {validation}"
    )


def refresh_manifest(
    root: Path, manifest_path: Path, glslang: Path, spirv_val: Path | None
) -> None:
    entries, _ = discover_layout(root)
    errors = validate_artifact_pairs(root, entries)
    with tempfile.TemporaryDirectory(prefix="xpbd-spirv-refresh-") as directory:
        outputs = compile_shaders(root, entries, glslang, spirv_val, Path(directory))
        for entry in entries:
            if outputs[entry["source"]].read_bytes() != repository_path(
                root, entry["spv"]
            ).read_bytes():
                errors.append(
                    f"cannot refresh manifest: {entry['source']} does not reproduce "
                    f"{entry['spv']}"
                )
    if errors:
        raise SpirvCheckError("\n".join(errors))
    write_manifest(manifest_path, manifest_data(root))
    print(f"Updated {relative_name(root, manifest_path)}")


def rebuild_repository(
    root: Path, manifest_path: Path, glslang: Path, spirv_val: Path | None
) -> None:
    entries, _ = discover_layout(root)
    with tempfile.TemporaryDirectory(prefix="xpbd-spirv-rebuild-") as directory:
        outputs = compile_shaders(root, entries, glslang, spirv_val, Path(directory))
        generated: list[tuple[Path, bytes, Path, bytes]] = []
        for entry in entries:
            binary = outputs[entry["source"]].read_bytes()
            spv = repository_path(root, entry["spv"])
            inc = repository_path(root, entry["inc"])
            embedded = include_text(entry["source"], binary, inc).encode("utf-8")
            generated.append((spv, binary, inc, embedded))
        # Compilation and validation of every shader completed before the first
        # checked-in artifact is replaced.
        for spv, binary, inc, embedded in generated:
            atomic_write(spv, binary)
            atomic_write(inc, embedded)
            print(f"Generated {relative_name(root, spv)}")
            print(f"Generated {relative_name(root, inc)}")
    write_manifest(manifest_path, manifest_data(root))
    print(f"Updated {relative_name(root, manifest_path)}")


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="xpbd-spirv-selftest-") as directory:
        root = Path(directory).resolve()
        shader_directory = root / SHADER_DIRECTORY
        shader_directory.mkdir(parents=True)
        source = shader_directory / "stale_test.comp"
        spv = Path(f"{source}.spv")
        inc = Path(f"{source}.spv.inc")
        original_source = "#version 450\nvoid main() {}\n"
        binary = struct.pack("<5I", 0x07230203, 0x00010000, 0, 1, 0)
        source.write_bytes(original_source.replace("\n", "\r\n").encode("utf-8"))
        spv.write_bytes(binary)
        original_include = include_text(source.name, binary, None)
        inc.write_bytes(
            original_include.replace("\n", "\r\n").encode("utf-8")
        )
        manifest = manifest_data(root)
        if validate_manifest(root, manifest):
            raise SpirvCheckError("self-test baseline manifest was rejected")
        entries, _ = discover_layout(root)
        if validate_artifact_pairs(root, entries):
            raise SpirvCheckError("self-test baseline SPIR-V pair was rejected")

        source.write_bytes(original_source.encode("utf-8"))
        inc.write_bytes(original_include.encode("utf-8"))
        if validate_manifest(root, manifest):
            raise SpirvCheckError("self-test hashes changed across CRLF/LF checkout")

        source.write_text(original_source + "// stale\n", encoding="utf-8")
        if not validate_manifest(root, manifest):
            raise SpirvCheckError("self-test did not detect stale GLSL hash")
        source.write_text(original_source, encoding="utf-8")

        spv.write_bytes(binary[:-4] + struct.pack("<I", 7))
        if not validate_artifact_pairs(root, entries):
            raise SpirvCheckError("self-test did not detect stale .spv")
        spv.write_bytes(binary)

        inc.write_text(
            include_text(source.name, binary[:-4] + struct.pack("<I", 9), None),
            encoding="utf-8",
        )
        if not validate_artifact_pairs(root, entries):
            raise SpirvCheckError("self-test did not detect stale .spv.inc")
    print("SPIR-V stale-detection self-test passed")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command",
        choices=("check", "rebuild", "refresh-manifest", "self-test"),
    )
    default_root = Path(__file__).resolve().parent.parent
    parser.add_argument("--repo-root", type=Path, default=default_root)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--glslang")
    parser.add_argument("--spirv-val")
    parser.add_argument(
        "--require-spirv-val",
        action="store_true",
        help=(
            "fail when standalone spirv-val is absent "
            "(glslang built-in validation still runs)"
        ),
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.command == "self-test":
        self_test()
        return 0
    root = arguments.repo_root.resolve()
    manifest_path = (
        arguments.manifest.resolve()
        if arguments.manifest is not None
        else root / "tools/spirv_manifest.json"
    )
    glslang = resolve_tool(
        arguments.glslang,
        "GLSLANG_VALIDATOR",
        (
            root / "tools/glslang/bin/glslangValidator",
            root / "tools/glslang/bin/glslangValidator.exe",
        ),
        ("glslangValidator", "glslangValidator.exe"),
        required=True,
    )
    spirv_val = resolve_tool(
        arguments.spirv_val,
        "SPIRV_VAL",
        (
            root / "tools/spirv-tools/bin/spirv-val",
            root / "tools/spirv-tools/bin/spirv-val.exe",
            root / "tools/glslang/bin/spirv-val",
            root / "tools/glslang/bin/spirv-val.exe",
        ),
        ("spirv-val", "spirv-val.exe"),
        required=arguments.require_spirv_val,
    )
    assert glslang is not None
    if arguments.command == "check":
        check_repository(root, manifest_path, glslang, spirv_val)
    elif arguments.command == "refresh-manifest":
        refresh_manifest(root, manifest_path, glslang, spirv_val)
    else:
        rebuild_repository(root, manifest_path, glslang, spirv_val)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SpirvCheckError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
