from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class ContextPack:
    route: str
    confidence: float
    question: str
    interpretation: str
    matches: list[dict[str, Any]] = field(default_factory=list)
    relations: list[dict[str, Any]] = field(default_factory=list)
    sections: dict[str, Any] = field(default_factory=dict)
    evidence: list[dict[str, Any]] = field(default_factory=list)
    diagnostics: list[dict[str, Any]] = field(default_factory=list)
    uncertainties: list[str] = field(default_factory=list)
    next_actions: list[dict[str, Any]] = field(default_factory=list)
    query_source: str = "snapshot_fragments"

    def to_result(self, scope: list[str], max_items: int) -> dict[str, Any]:
        return {
            "schema_version": "uepi.context-pack.v1",
            "question": self.question,
            "scope": scope,
            "route": self.route,
            "route_confidence": round(max(0.0, min(self.confidence, 1.0)), 3),
            "interpretation": self.interpretation,
            "matches": self.matches[:max_items],
            "relations": self.relations[:max_items],
            "sections": self.sections,
            "query_source": self.query_source,
            "uncertainties": self.uncertainties,
        }
