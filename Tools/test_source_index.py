#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[3]
DAEMON_PATH = REPO_ROOT / "Plugins" / "UEProjectIntelligence" / "Services" / "uepi_daemon" / "uepi_daemon.py"


def load_daemon():
    spec = importlib.util.spec_from_file_location("uepi_daemon", DAEMON_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load daemon module from {DAEMON_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    daemon = load_daemon()
    with tempfile.TemporaryDirectory(prefix="uepi_source_index_") as temp_dir:
        root = Path(temp_dir) / "FixtureProject"
        source_dir = root / "Source" / "Fixture" / "Public"
        private_dir = root / "Source" / "Fixture" / "Private"
        config_dir = root / "Config"
        source_dir.mkdir(parents=True)
        private_dir.mkdir(parents=True)
        config_dir.mkdir(parents=True)
        (root / "FixtureProject.uproject").write_text('{"FileVersion":3}', encoding="utf-8")
        (root / "compile_commands.json").write_text("[]", encoding="utf-8")
        (root / "Source" / "Fixture" / "Fixture.Build.cs").write_text(
            'using UnrealBuildTool;\npublic class Fixture : ModuleRules { public Fixture(ReadOnlyTargetRules Target) : base(Target) { PublicDependencyModuleNames.AddRange(new string[] {"Core"}); } }\n',
            encoding="utf-8",
        )
        (source_dir / "FixtureThing.h").write_text(
            """
#pragma once
#include "CoreMinimal.h"

UCLASS(BlueprintType)
class FIXTURE_API UFixtureThing : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(BlueprintReadWrite)
    TSoftObjectPtr<UObject> FeaturedAsset;

    UFUNCTION(BlueprintCallable)
    void DoThing();
};
""".lstrip(),
            encoding="utf-8",
        )
        (private_dir / "FixtureThing.cpp").write_text(
            'const TCHAR* AssetPath = TEXT("/Game/Fixture/BP_Fixture.BP_Fixture");\n',
            encoding="utf-8",
        )
        (config_dir / "DefaultGame.ini").write_text(
            """
[/Script/EngineSettings.GameMapsSettings]
GameDefaultMap="/Game/Maps/Old.Old"
GameDefaultMap="/Game/Maps/FixtureMap.FixtureMap"
+ActiveMaps="/Game/Maps/A.A"
+ActiveMaps="/Game/Maps/B.B"
-ActiveMaps="/Game/Maps/A.A"
""".lstrip(),
            encoding="utf-8",
        )

        db_path = Path(temp_dir) / "index.sqlite3"
        indexed = daemon.index_source(db_path, str(root / "FixtureProject.uproject"))
        assert indexed["ok"] is True
        assert indexed["file_count"] >= 4
        assert indexed["symbol_count"] >= 3
        assert indexed["reference_count"] >= 3
        assert indexed["config_value_count"] >= 5
        assert indexed["compile_database"]

        symbols = daemon.source_symbols(db_path, str(root), query="DoThing")
        assert any(symbol["name"] == "DoThing" and symbol["metadata"]["blueprint_callable"] for symbol in symbols["symbols"])

        refs = daemon.source_references(db_path, str(root), query="/Game/")
        assert any(reference["kind"] == "cpp_asset_reference" for reference in refs["references"])
        assert any(reference["kind"] == "config_asset_reference" for reference in refs["references"])

        search = daemon.source_search(db_path, "FixtureThing", str(root))
        assert search["symbols"]

        do_thing_symbols = [symbol for symbol in symbols["symbols"] if symbol["name"] == "DoThing"]
        assert do_thing_symbols
        assert do_thing_symbols[0]["qualified_name"] == "UFixtureThing::DoThing"
        assert do_thing_symbols[0]["metadata"]["owner_cpp_name"] == "UFixtureThing"
        assert do_thing_symbols[0]["metadata"]["owner_unreal_name"] == "FixtureThing"

        scan_path = Path(temp_dir) / "blueprint_call_scan.json"
        scan = {
            "schema_version": "uepi.scan.v1",
            "project_id": "fixture_project",
            "project_name": "FixtureProject",
            "project_file": str(root / "FixtureProject.uproject"),
            "engine_version": "5.3.2",
            "started_at_utc": "2026-01-01T00:00:00Z",
            "finished_at_utc": "2026-01-01T00:00:01Z",
            "completeness": {"state": "partial", "covered": ["blueprint_semantics_first_pass"], "omitted": [], "warnings": []},
            "entities": [
                {
                    "id": "node_call_do_thing",
                    "kind": "blueprint_node",
                    "canonical_key": "/Game/BP_Fixture.BP_Fixture:graph:EventGraph:node:CallDoThing",
                    "display_name": "Call DoThing",
                    "source_layer": "EditorSourceGraph",
                    "attributes": {"semantic_kind": "call_function"},
                    "snapshot": {},
                    "completeness": {"state": "partial", "covered": ["node_semantic_call_function"], "omitted": [], "warnings": []},
                    "diagnostics": [],
                    "evidence": [],
                },
                {
                    "id": "function_do_thing",
                    "kind": "u_function",
                    "canonical_key": "/Script/Fixture.FixtureThing:DoThing",
                    "display_name": "DoThing",
                    "source_layer": "Reflection",
                    "attributes": {
                        "function_name": "DoThing",
                        "owner_class_name": "FixtureThing",
                        "owner_class": "/Script/Fixture.FixtureThing",
                    },
                    "snapshot": {},
                    "completeness": {"state": "partial", "covered": [], "omitted": [], "warnings": []},
                    "diagnostics": [],
                    "evidence": [],
                },
            ],
            "relations": [
                {
                    "id": "rel_calls_do_thing",
                    "type": "calls_function",
                    "from_id": "node_call_do_thing",
                    "to_id": "function_do_thing",
                    "source_layer": "Derived",
                    "derived": True,
                    "confidence": 1.0,
                    "attributes": {
                        "function_name": "DoThing",
                        "function_path": "/Script/Fixture.FixtureThing:DoThing",
                        "owner_class": "/Script/Fixture.FixtureThing",
                        "owner_class_name": "FixtureThing",
                        "is_native_function": "true",
                    },
                    "evidence": [],
                }
            ],
            "diagnostics": [],
        }
        scan_path.write_text(json.dumps(scan, ensure_ascii=False), encoding="utf-8")
        daemon.ingest(scan_path, db_path)
        links = daemon.blueprint_cpp_links(db_path, project=str(root), query="DoThing")
        assert links["link_count"] == 1
        assert links["links"][0]["match_reason"] == "function_name_and_owner_class"
        assert links["links"][0]["source_symbol"]["qualified_name"] == "UFixtureThing::DoThing"
        assert links["links"][0]["derived_relation"]["type"] == "blueprint_calls_cpp_symbol"

        configs = daemon.config_values(
            db_path,
            str(root),
            section="/Script/EngineSettings.GameMapsSettings",
            key="GameDefaultMap",
            include_history=True,
        )
        assert configs["effective"][0]["value"] == "/Game/Maps/FixtureMap.FixtureMap"
        assert len(configs["effective"][0]["history"]) == 2

        active_maps = daemon.config_values(
            db_path,
            str(root),
            section="/Script/EngineSettings.GameMapsSettings",
            key="ActiveMaps",
        )
        assert active_maps["effective"][0]["values"] == ["/Game/Maps/B.B"]

    print("source index assertions ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
