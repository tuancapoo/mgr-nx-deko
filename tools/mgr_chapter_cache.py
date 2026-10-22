"""Export chapter captures, merge discoveries, and build a portable prewarm seed.

Python 3.10+, standard library only. This NEVER merges Vulkan driver blobs and
NEVER treats a captured/structurally valid shader as proven executable code.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import tempfile
import zipfile

RECORD_BYTES = 424
KEY_BYTES = 80
MAX_RECORDS = 65536
MAX_FILE_BYTES = 256 << 20
MAX_SNAPSHOT_BYTES = 512 << 20


def digest(data):
    return hashlib.sha256(data).hexdigest()


def chapter_name(value):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", value):
        raise ValueError("Chapter must be a short label such as R-00 or R-01-part2")
    return value


def atomic_write(path, data, *, replace=False):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and not replace:
        if path.read_bytes() != data:
            raise ValueError(f"Refusing to replace a different existing file: {path}")
        return
    handle, pending = tempfile.mkstemp(prefix=path.name + ".", suffix=".pending", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(pending, path)
    finally:
        if os.path.exists(pending):
            os.unlink(pending)


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def validate_record(record):
    if len(record) != RECORD_BYTES:
        raise ValueError("Wrong pipeline record size")
    vs, ps = struct.unpack_from("<II", record)
    vs_fp, ps_fp, attributes, bindings = struct.unpack_from("<QQII", record, KEY_BYTES)
    if not vs or attributes > 16 or bindings > 16:
        raise ValueError("Invalid pipeline shader/count fields")
    locations = set()
    for index in range(attributes):
        location, binding, fmt, offset = struct.unpack_from("<IIII", record, 104 + index * 16)
        if location >= 32 or binding >= bindings or not fmt or location in locations:
            raise ValueError("Invalid/duplicate pipeline vertex attribute")
        locations.add(location)
    # Zero fingerprints are legacy, readable locally but never portable.
    return bool(vs_fp and (not ps or ps_fp))


def canonical_record(record):
    validate_record(record)
    key = bytearray(record)
    vs_fp, ps_fp = struct.unpack_from("<QQ", record, KEY_BYTES)
    if vs_fp:
        struct.pack_into("<I", key, 0, 0)
    if ps_fp:
        struct.pack_into("<I", key, 4, 0)
    return bytes(key)


def read_list(data):
    if len(data) < 16:
        raise ValueError("Truncated NFPL header")
    magic, version, size, count = struct.unpack_from("<4sIII", data)
    if magic != b"NFPL" or version != 2 or size != RECORD_BYTES:
        raise ValueError("Unsupported NFPL version/record layout")
    if count > MAX_RECORDS or len(data) != 16 + count * size:
        raise ValueError("Damaged/oversized NFPL list")
    records = [data[16 + i * size:16 + (i + 1) * size] for i in range(count)]
    for record in records:
        validate_record(record)
    return records


def make_list(records):
    if len(records) > MAX_RECORDS:
        raise ValueError("More than 65536 pipelines: select fewer chapters; nothing was truncated")
    for record in records:
        validate_record(record)
    return struct.pack("<4sIII", b"NFPL", 2, RECORD_BYTES, len(records)) + b"".join(records)


def read_pipeline_file(data):
    if data[:4] == b"NFPL":
        return read_list(data), 0
    if len(data) < 24:
        raise ValueError("Truncated NFPC header")
    magic, version, list_bytes, cache_bytes = struct.unpack_from("<4sIQQ", data)
    if magic != b"NFPC" or version != 1 or len(data) != 24 + list_bytes + cache_bytes:
        raise ValueError("Damaged/unsupported NFPC file")
    return read_list(data[24:24 + list_bytes]) if list_bytes else [], cache_bytes


def validate_container(data, *, variant=False):
    if len(data) < 28 or len(data) > 8 << 20:
        raise ValueError("Truncated/oversized XDK shader container")
    magic, virtual, physical = struct.unpack_from(">III", data)
    header = struct.unpack_from(">I", data, 24)[0]
    if magic & 0xFFFFFF00 != 0x102A1100 or virtual < 28 or virtual + physical != len(data):
        raise ValueError("Invalid shader magic/virtual/physical bounds")
    if header > virtual or virtual - header < 8:
        raise ValueError("Shader header out of bounds")
    offset, size = struct.unpack_from(">II", data, header)
    if not size or size % 12 or offset > physical or size > physical - offset:
        raise ValueError("Shader sequencer out of bounds")
    if variant and magic & 1:
        if virtual - header < 36:
            raise ValueError("Truncated vertex declaration header")
        defs, fetches = struct.unpack_from(">II", data, header + 24)
        if header + 36 + defs * 4 + fetches * 4 > virtual:
            raise ValueError("Vertex declaration out of bounds")
    return "vs" if magic & 1 else "ps"


def relative_name(value):
    path = PurePosixPath(str(value).replace("\\", "/"))
    if path.is_absolute() or ".." in path.parts or any(":" in part for part in path.parts):
        raise ValueError("Unsafe snapshot path")
    parts = path.parts
    if parts[:2] == ("switch", "mgr-nx"):
        parts = parts[2:]
    return PurePosixPath(*parts).as_posix()


def allowed(name):
    return (name.startswith("collect/") or name.startswith("logs/")) or name in (
        "cache/sa2_native_pipelines.bin", "cache/mgr_pipeline_seed.bin", "snapshot.json", "scan.json")


def snapshot_files(source):
    source = Path(source)
    total = 0
    if source.is_file():
        with zipfile.ZipFile(source) as archive:
            seen = set()
            for info in sorted(archive.infolist(), key=lambda item: item.filename):
                if info.is_dir():
                    continue
                name = relative_name(info.filename)
                if name in seen:
                    raise ValueError("Duplicate snapshot member: " + name)
                seen.add(name)
                if not allowed(name):
                    continue
                total += info.file_size
                if info.file_size > MAX_FILE_BYTES or total > MAX_SNAPSHOT_BYTES:
                    raise ValueError("Snapshot exceeds bounded input size")
                yield name, archive.read(info)
    else:
        root = source.resolve(strict=True)
        for path in sorted(root.rglob("*")):
            if not path.is_file() or path.is_symlink():
                continue
            path.resolve().relative_to(root)
            name = relative_name(path.relative_to(root).as_posix())
            if not allowed(name):
                continue
            size = path.stat().st_size
            total += size
            if size > MAX_FILE_BYTES or total > MAX_SNAPSHOT_BYTES:
                raise ValueError("Snapshot exceeds bounded input size")
            yield name, path.read_bytes()


def export_snapshot(game_folder, chapter, output):
    chapter_name(chapter)
    output = Path(output)
    if output.exists():
        raise ValueError("Use a new output ZIP; an existing chapter export is never replaced")
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_suffix(output.suffix + ".pending")
    manifest = {"schema": 1, "chapter": chapter, "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "coverage": "observed gameplay only; not proof of complete chapter coverage", "files": {}}
    try:
        with zipfile.ZipFile(pending, "w", zipfile.ZIP_DEFLATED, compresslevel=3) as archive:
            for name, data in snapshot_files(game_folder):
                if name == "snapshot.json":
                    continue
                archive.writestr(name, data)
                manifest["files"][name] = {"bytes": len(data), "sha256": digest(data)}
            if not manifest["files"]:
                raise ValueError("No collect/cache/log files found in the selected game folder")
            archive.writestr("snapshot.json", json_bytes(manifest))
        with zipfile.ZipFile(pending) as archive:
            if archive.testzip() is not None:
                raise ValueError("ZIP verification failed")
        pending.rename(output)
    finally:
        if pending.exists():
            pending.unlink()
    return {"output": str(output), "chapter": chapter, "files": len(manifest["files"]),
            "bytes": output.stat().st_size}


def load_catalog(store):
    path = Path(store) / "catalog.json"
    result = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {
        "schema": 1, "assets": {}, "pipelines": {}, "sessions": []}
    if result.get("schema") != 1:
        raise ValueError("Unsupported capture store schema")
    return result


def put_object(store, data, suffix):
    name = digest(data) + suffix
    atomic_write(Path(store) / "objects" / name, data)
    return name


def merge_snapshot(source, chapter, store):
    chapter_name(chapter)
    store = Path(store)
    catalog = load_catalog(store)
    report = {"chapter": chapter, "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "new_assets": 0, "new_pipelines": 0, "legacy_pipelines_skipped": 0,
              "driver_cache_bytes_excluded": 0, "rejected": [], "capture_warnings": [],
              "assets": [], "pipelines": [], "files": {}}
    declared = None
    for name, data in snapshot_files(source):
        report["files"][name] = {"bytes": len(data), "sha256": digest(data)}
        if name == "snapshot.json":
            declared = json.loads(data)
            if declared.get("chapter") != chapter:
                raise ValueError("The supplied chapter differs from the snapshot label")
            continue
        try:
            if name in ("cache/sa2_native_pipelines.bin", "cache/mgr_pipeline_seed.bin"):
                records, driver_bytes = read_pipeline_file(data)
                report["driver_cache_bytes_excluded"] += driver_bytes
                for record in records:
                    if not validate_record(record):
                        report["legacy_pipelines_skipped"] += 1
                        continue
                    identity = digest(canonical_record(record))
                    if identity not in catalog["pipelines"]:
                        catalog["pipelines"][identity] = {"object": put_object(store, record, ".pipeline")}
                        report["new_pipelines"] += 1
                    report["pipelines"].append(identity)
            elif name.startswith("collect/") and name.endswith((".bin", ".bin.pending", ".ucode")):
                variant = ("/variants/" in name or "/missing/" in name or
                           PurePosixPath(name).name.startswith(("v_", "p_")))
                if name.endswith(".ucode"):
                    if not data or len(data) % 4:
                        raise ValueError("Truncated raw microcode")
                    stage = "vs" if PurePosixPath(name).name.startswith("vs_") else "ps"
                    kind, suffix = "raw", ".ucode"
                else:
                    stage = validate_container(data, variant=variant)
                    kind, suffix = ("variants" if variant else "containers"), ".bin"
                object_name = put_object(store, data, suffix)
                identity = kind + ":" + digest(data)
                if identity not in catalog["assets"]:
                    catalog["assets"][identity] = {"object": object_name, "kind": kind,
                                                   "stage": stage, "bytes": len(data),
                                                   "compatibility": "unproven; offline review required"}
                    report["new_assets"] += 1
                report["assets"].append(identity)
            elif name.startswith("logs/") and name.endswith((".log", ".txt")):
                for line in data.decode("utf-8", errors="replace").splitlines():
                    if any(marker in line for marker in ("limit reached", "capture failed", "write failed/conflicted",
                                                        "discovery written=false")):
                        report["capture_warnings"].append(line[:1000])
        except (ValueError, struct.error) as error:
            report["rejected"].append({"file": name, "reason": str(error)})
    if not report["files"]:
        raise ValueError("No collect/cache/log files found; select the exported ZIP or the game directory")
    if declared is not None:
        if set(declared.get("files", {})) != set(report["files"]) - {"snapshot.json"}:
            raise ValueError("Snapshot members differ from the integrity manifest")
        for name, expected in declared.get("files", {}).items():
            if report["files"].get(name) != expected:
                raise ValueError("Snapshot integrity check failed: " + name)
    report["assets"] = sorted(set(report["assets"]))
    report["pipelines"] = sorted(set(report["pipelines"]))
    run_key = digest(json_bytes({"chapter": chapter, "files": report["files"]}))
    if any(session["input_id"] == run_key for session in catalog["sessions"]):
        return {"already_imported": True, "chapter": chapter, "input_id": run_key}
    run_path = "sessions/" + chapter + "-" + run_key[:16] + ".json"
    atomic_write(store / run_path, json_bytes(report))
    catalog["sessions"].append({"chapter": chapter, "input_id": run_key, "report": run_path})
    atomic_write(store / "catalog.json", json_bytes(catalog), replace=True)
    return {key: report[key] for key in ("chapter", "new_assets", "new_pipelines", "legacy_pipelines_skipped",
                                        "driver_cache_bytes_excluded", "rejected", "capture_warnings")} | {
        "total_assets": len(catalog["assets"]), "total_pipelines": len(catalog["pipelines"]),
        "report": str(store / run_path)}


def selected(catalog, store, chapters, field):
    if not chapters:
        return sorted(catalog[field])
    result = set()
    for session in catalog["sessions"]:
        if session["chapter"] in chapters:
            report = json.loads((Path(store) / session["report"]).read_text(encoding="utf-8"))
            result.update(report[field])
    if not result:
        raise ValueError("No records for the requested chapters")
    return sorted(result)


def read_object(store, name):
    if not re.fullmatch(r"[0-9a-f]{64}\.(bin|ucode|pipeline)", name):
        raise ValueError("Invalid store object name")
    data = (Path(store) / "objects" / name).read_bytes()
    if digest(data) != name.split(".")[0]:
        raise ValueError("Corrupt store object: " + name)
    return data


def export_seed(store, output, chapters=None):
    catalog = load_catalog(store)
    keys = selected(catalog, store, chapters, "pipelines")
    records = [read_object(store, catalog["pipelines"][key]["object"]) for key in keys]
    data = make_list(records)
    atomic_write(output, data)
    return {"output": str(output), "pipelines": len(records), "bytes": len(data),
            "driver_cache_bytes": 0, "sha256": digest(data)}


def export_review(store, output, chapters=None):
    output = Path(output)
    if output.exists():
        raise ValueError("Use a new review folder")
    catalog = load_catalog(store)
    keys = selected(catalog, store, chapters, "assets")
    output.mkdir(parents=True)
    for key in keys:
        item = catalog["assets"][key]
        name = item["stage"] + "_" + item["object"]
        folder = "rebuilt-ps" if item["kind"] == "variants" and item["stage"] == "ps" else item["kind"]
        atomic_write(output / folder / name, read_object(store, item["object"]))
    atomic_write(output / "README.txt", b"All inputs are unproven. Validate original containers, prove vertex declaration variants\n"
                 b"against the current in_ok templates, and audit raw/rebuilt shaders before translating.\n"
                 b"No shader from this directory is installed automatically.\n")
    return {"output": str(output), "unproven_inputs": len(keys)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    export = actions.add_parser("export", help="Close the game, then snapshot the SD game folder into a new ZIP")
    export.add_argument("--game-folder", required=True, type=Path)
    export.add_argument("--chapter", required=True)
    export.add_argument("--output", required=True, type=Path)
    merge = actions.add_parser("merge", help="Import a chapter ZIP/folder without duplicating previous captures")
    merge.add_argument("--input", required=True, type=Path)
    merge.add_argument("--chapter", required=True)
    merge.add_argument("--store", required=True, type=Path)
    for name in ("seed", "review"):
        action = actions.add_parser(name)
        action.add_argument("--store", required=True, type=Path)
        action.add_argument("--output", required=True, type=Path)
        action.add_argument("--chapters", nargs="+")
    args = parser.parse_args()
    try:
        if args.action == "export":
            result = export_snapshot(args.game_folder, args.chapter, args.output)
        elif args.action == "merge":
            result = merge_snapshot(args.input, args.chapter, args.store)
        elif args.action == "seed":
            result = export_seed(args.store, args.output, args.chapters)
        else:
            result = export_review(args.store, args.output, args.chapters)
    except (ValueError, OSError, zipfile.BadZipFile, json.JSONDecodeError) as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
