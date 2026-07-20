"""UE Project Intelligence v2 Snapshot query package."""

from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path

__all__ = [
    "__version__",
    "SERVICE_BUILD_ID",
    "SERVICE_MODULE_PATH",
    "SERVICE_PROCESS_ID",
    "SERVICE_PROCESS_START_TIME",
    "SERVICE_SOURCE_HASH",
    "compute_service_source_hash",
]

__version__ = "2.0.0-beta.6"


def compute_service_source_hash() -> str:
    digest = hashlib.sha256()
    package_root = Path(__file__).resolve().parent
    for path in sorted(package_root.glob("*.py"), key=lambda item: item.name.casefold()):
        digest.update(path.name.encode("utf-8"))
        try:
            digest.update(path.read_bytes())
        except OSError:
            digest.update(b"<unreadable>")
    return "sha256:" + digest.hexdigest()


SERVICE_MODULE_PATH = str(Path(__file__).resolve())
SERVICE_PROCESS_ID = os.getpid()
SERVICE_PROCESS_START_TIME = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
SERVICE_SOURCE_HASH = compute_service_source_hash()
SERVICE_BUILD_ID = f"uepi-service-{__version__}-{SERVICE_SOURCE_HASH.removeprefix('sha256:')[:12]}"
