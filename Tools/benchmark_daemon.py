from __future__ import annotations

import argparse
import gc
import json
from pathlib import Path
import subprocess
import sys
import time
from typing import Any


def cleanup_db(db_path: Path) -> None:
    for candidate in [db_path, db_path.with_name(db_path.name + "-wal"), db_path.with_name(db_path.name + "-shm")]:
        if candidate.exists():
            candidate.unlink()


def timed(label: str, fn: Any) -> dict[str, Any]:
    start = time.perf_counter()
    value = fn()
    return {"label": label, "elapsed_ms": round((time.perf_counter() - start) * 1000, 3), "result": value}


def make_synthetic_scan(path: Path, entity_count: int) -> None:
    entities = []
    relations = []
    for index in range(entity_count):
        object_path = f"/Game/UEPI/Synthetic/E{index}.E{index}"
        entity_id = f"synthetic_entity_{index:06d}"
        entities.append(
            {
                "id": entity_id,
                "kind": "asset",
                "canonical_key": object_path,
                "display_name": f"E{index}",
                "source_layer": "synthetic_benchmark",
                "attributes": {
                    "object_path": object_path,
                    "package_name": f"/Game/UEPI/Synthetic/E{index}",
                    "package_path": "/Game/UEPI/Synthetic",
                    "asset_class_path": "/Script/Engine.DataAsset",
                },
                "snapshot": {},
                "completeness": {"state": "metadata_only", "covered": ["synthetic"], "omitted": [], "warnings": []},
                "diagnostics": [],
                "evidence": [{"source": "benchmark_daemon", "detail": "Synthetic performance fixture."}],
            }
        )
        if index:
            relations.append(
                {
                    "id": f"synthetic_relation_{index:06d}",
                    "type": "asset_reference",
                    "from_id": f"synthetic_entity_{index - 1:06d}",
                    "to_id": entity_id,
                    "source_layer": "synthetic_benchmark",
                    "derived": False,
                    "confidence": 1.0,
                    "attributes": {"synthetic": True},
                    "evidence": [{"source": "benchmark_daemon", "detail": "Synthetic chain edge."}],
                }
            )

    scan = {
        "schema_version": "uepi.scan.v1",
        "project_id": "uepi:project:synthetic",
        "project_name": "UEPI Synthetic Benchmark",
        "project_file": str(Path.cwd() / "GasDemo.uproject"),
        "engine_version": "5.3",
        "started_at_utc": "2026-01-01T00:00:00Z",
        "finished_at_utc": "2026-01-01T00:00:01Z",
        "completeness": {"state": "metadata_only", "covered": ["synthetic"], "omitted": [], "warnings": []},
        "entities": entities,
        "relations": relations,
        "diagnostics": [],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(scan, ensure_ascii=False, sort_keys=True), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Benchmark UEPI daemon ingest and query paths.")
    parser.add_argument("--entities", type=int, default=2000)
    parser.add_argument("--db", type=Path, default=Path("Saved/UEProjectIntelligence/uepi_perf.sqlite3"))
    parser.add_argument("--scan", type=Path, default=Path("Saved/UEProjectIntelligence/uepi_perf_synthetic_scan.json"))
    parser.add_argument("--report", type=Path, default=Path("Saved/UEProjectIntelligence/performance_baseline.json"))
    parser.add_argument(
        "--daemon",
        type=Path,
        default=Path("Plugins/UEProjectIntelligence/Services/uepi_daemon/uepi_daemon.py"),
    )
    args = parser.parse_args(argv)

    cleanup_db(args.db)
    gc.collect()
    gc_before = gc.get_count()
    make_synthetic_scan(args.scan, args.entities)
    base = [sys.executable, "-B", str(args.daemon), "--db", str(args.db)]
    measurements: list[dict[str, Any]] = []

    measurements.append(timed("ingest", lambda: json.loads(subprocess.check_output(base + ["ingest", "--scan", str(args.scan)], text=True))))
    measurements.append(timed("entities_page", lambda: json.loads(subprocess.check_output(base + ["entities", "--limit", "500"], text=True))["count"]))
    measurements.append(timed("relations_page", lambda: json.loads(subprocess.check_output(base + ["relations", "--limit", "500"], text=True))["count"]))
    measurements.append(
        timed(
            "subgraph_depth_2",
            lambda: json.loads(
                subprocess.check_output(base + ["subgraph", "/Game/UEPI/Synthetic/E0.E0", "--depth", "2", "--limit", "500"], text=True)
            )["edge_count"],
        )
    )
    measurements.append(timed("integrity", lambda: json.loads(subprocess.check_output(base + ["integrity"], text=True))["ok"]))

    report = {
        "entity_count": args.entities,
        "relation_count": max(args.entities - 1, 0),
        "scan_path": str(args.scan),
        "db_path": str(args.db),
        "db_size_bytes": args.db.stat().st_size if args.db.exists() else 0,
        "python_gc_count_before": gc_before,
        "python_gc_count_after": gc.get_count(),
        "measurements": measurements,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
