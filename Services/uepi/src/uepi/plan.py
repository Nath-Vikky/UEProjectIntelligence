from __future__ import annotations

import hashlib
import json
from typing import Any


PLAN_SCHEMA = "uepi.edit_plan.v2"


def canonical_plan_hash(plan: dict[str, Any]) -> str:
    value = json.loads(json.dumps({key: child for key, child in plan.items() if key != "plan_hash"}, ensure_ascii=False))
    if isinstance(value.get("approval"), dict):
        value["approval"].pop("plan_hash", None)
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def verify_plan_hash(plan: dict[str, Any]) -> bool:
    return str(plan.get("plan_hash") or "") == canonical_plan_hash(plan)
