from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .store import SnapshotState, SnapshotStore


@dataclass
class SnapshotView:
    """Read facade for the UEPI saved/live/tombstone current view."""

    store: SnapshotStore
    state: SnapshotState

    @classmethod
    def open(cls, store: SnapshotStore) -> "SnapshotView":
        return cls(store=store, state=store.load_state())

    @property
    def data_mode(self) -> str:
        return self.state.data_mode

    @property
    def generation(self) -> int:
        return self.state.generation

    def envelope_state(self) -> dict[str, Any]:
        return self.state.envelope_state()

    def load_current_view(self) -> dict[str, Any]:
        return self.store.load_project_scan(self.state)

    def active_editor_session(self, max_age_seconds: float = 30.0) -> dict[str, Any] | None:
        return self.store.active_editor_session(max_age_seconds=max_age_seconds)

    def latest_incremental_event(self, identifier: str | None = None) -> dict[str, Any] | None:
        return self.store.latest_incremental_event(identifier)

    def observed_at(self, identifier: str) -> Any:
        return self.store.asset_fragment_observed_at(identifier, self.state)

    def find_tombstone(self, identifier: str) -> dict[str, Any] | None:
        return self.store.find_tombstone(identifier, self.state)

    def request_refresh(self, targets: list[str], *, reason: str, tool_name: str, data_mode: str = "live"):
        return self.store.request_refresh(targets, reason=reason, tool_name=tool_name, data_mode=data_mode)
