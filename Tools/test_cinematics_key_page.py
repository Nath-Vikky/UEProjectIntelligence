#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[3]
DAEMON_PATH = REPO_ROOT / "Plugins" / "UEProjectIntelligence" / "Services" / "uepi_daemon" / "uepi_daemon.py"
LEVEL_SEQUENCE_SCAN = REPO_ROOT / "Saved" / "UEProjectIntelligence" / "l2_level_sequence_uepi_simple_scan.json"
LEVEL_SEQUENCE_ENTITY = "/Game/UEPI/Fixtures/Cinematics/LS_UEPI_Simple.LS_UEPI_Simple"


def load_daemon():
    spec = importlib.util.spec_from_file_location("uepi_daemon", DAEMON_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load daemon module from {DAEMON_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def hex_id(*parts: object) -> str:
    return hashlib.sha256("|".join(str(part) for part in parts).encode("utf-8")).hexdigest()


def inject_key_rows(scan: dict) -> tuple[str, str]:
    for entity in scan.get("entities", []):
        snapshot = entity.get("snapshot")
        level_sequence = snapshot.get("level_sequence") if isinstance(snapshot, dict) else None
        if not isinstance(level_sequence, dict):
            continue
        level_sequence["key_storage"] = "inline_key_times_daemon_artifact"
        for section in level_sequence.get("sections", []):
            if not isinstance(section, dict) or section.get("key_count") != 6:
                continue
            section["key_storage"] = "inline_key_times_daemon_artifact"
            first_channel_id = ""
            for channel in section.get("channels", []):
                if not isinstance(channel, dict) or channel.get("key_count") != 2:
                    continue
                if not first_channel_id:
                    first_channel_id = channel["id"]
                keys = []
                for key_index, frame_number in enumerate((0, 48)):
                    keys.append(
                        {
                            "id": hex_id("movie_scene_key", channel["id"], key_index, frame_number),
                            "index": key_index,
                            "track_id": section["track_id"],
                            "section_id": section["id"],
                            "channel_id": channel["id"],
                            "channel_index": channel["index"],
                            "channel_name": channel["channel_name"],
                            "display_name": channel["display_name"],
                            "frame_number": frame_number,
                            "time_seconds": frame_number * 1001 / 24000,
                        }
                    )
                channel["key_time_count"] = len(keys)
                channel["keys"] = keys
            if not first_channel_id:
                raise RuntimeError("Fixture transform section did not contain keyed channels.")
            return section["id"], first_channel_id
    raise RuntimeError("Fixture scan did not contain a LevelSequence snapshot.")


def main() -> int:
    if not LEVEL_SEQUENCE_SCAN.exists():
        raise RuntimeError(f"Missing scan fixture: {LEVEL_SEQUENCE_SCAN}")

    daemon = load_daemon()
    with tempfile.TemporaryDirectory(prefix="uepi_cinematics_key_page_") as temp_dir:
        temp_path = Path(temp_dir)
        scan = json.loads(LEVEL_SEQUENCE_SCAN.read_text(encoding="utf-8-sig"))
        section_id, channel_id = inject_key_rows(scan)
        scan_path = temp_path / "level_sequence_keys_scan.json"
        scan_path.write_text(json.dumps(scan, ensure_ascii=False, indent=2), encoding="utf-8")

        db_path = temp_path / "index.sqlite3"
        ingest = daemon.ingest(scan_path, db_path)
        assert ingest["entity_count"] > 0

        first_page = daemon.cinematics_key_page(db_path, LEVEL_SEQUENCE_ENTITY, limit=2)
        assert first_page["entity"]["canonical_key"] == LEVEL_SEQUENCE_ENTITY
        assert first_page["sequence_path"] == LEVEL_SEQUENCE_ENTITY
        assert first_page["count"] == 2
        assert first_page["total_count"] == 6
        assert first_page["items"][0]["index"] == 0
        assert first_page["items"][0]["section_id"] == section_id
        assert first_page["has_more"]
        assert first_page["next_cursor"]

        second_page = daemon.cinematics_key_page(
            db_path,
            LEVEL_SEQUENCE_ENTITY,
            limit=2,
            cursor=first_page["next_cursor"],
        )
        assert second_page["start_index"] == 2
        assert second_page["items"][0]["channel_id"] != first_page["items"][0]["channel_id"]

        section_page = daemon.cinematics_key_page(db_path, LEVEL_SEQUENCE_ENTITY, section=section_id, limit=10)
        assert section_page["count"] == 6
        assert all(item["section_id"] == section_id for item in section_page["items"])

        channel_page = daemon.cinematics_key_page(db_path, LEVEL_SEQUENCE_ENTITY, channel=channel_id, limit=10)
        assert channel_page["count"] == 2
        assert all(item["channel_id"] == channel_id for item in channel_page["items"])

        artifact_page = daemon.cinematics_key_page(
            db_path,
            LEVEL_SEQUENCE_ENTITY,
            limit=1,
            include_artifact=True,
        )
        artifact = artifact_page["key_artifact"]
        artifact_path = Path(artifact["path"])
        assert artifact["artifact_uri"].startswith("uepi://cinematics-key-artifact/")
        assert artifact["scope_kind"] == "level_sequence"
        assert artifact["item_count"] == 6
        assert artifact["byte_count"] > 0
        assert artifact_path.exists()
        assert json.loads(artifact_path.read_text(encoding="utf-8"))["item_count"] == 6

    print("cinematics key page assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
