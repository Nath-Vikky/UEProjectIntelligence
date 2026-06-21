# Third-Party Notices

UEProjectIntelligence is implemented with Unreal Engine APIs and Python standard library modules by default.

Optional host-side packages:

- `mcp`: official Python Model Context Protocol SDK, listed in `Services/uepi_daemon/requirements-mcp.txt` for environments that want SDK host experiments.
- `pyarrow`: optional Parquet writer used only when installed by the user.

No third-party JavaScript libraries are bundled in `Web/index.html`.

Unreal Engine, plugin APIs, and editor modules remain governed by the Unreal Engine license terms applicable to the host project.
