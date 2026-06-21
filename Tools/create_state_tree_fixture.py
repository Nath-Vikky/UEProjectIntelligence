"""Create the checked StateTree fixture asset used by UEPI golden scans.

Run with Unreal's PythonScript commandlet, PythonScriptPlugin, and StateTree editor support enabled:

UnrealEditor-Cmd.exe GasDemo.uproject -run=pythonscript ^
  -Script="exec(open(r'Plugins/UEProjectIntelligence/Tools/create_state_tree_fixture.py').read())" ^
  -EnablePlugins=PythonScriptPlugin -EnablePlugins=StateTreeEditorModule -unattended -nop4 -nosplash -NullRHI
"""

import unreal


PACKAGE_PATH = "/Game/UEPI/Fixtures/StateTree"
ASSET_NAME = "ST_UEPI_Minimal"
ASSET_PATH = f"{PACKAGE_PATH}/{ASSET_NAME}"
PROBE_ASSET_PATH = f"{PACKAGE_PATH}/ST_UEPI_Minimal_Test"


def main():
    unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)

    if unreal.EditorAssetLibrary.does_asset_exist(PROBE_ASSET_PATH):
        if not unreal.EditorAssetLibrary.delete_asset(PROBE_ASSET_PATH):
            raise RuntimeError(f"failed to delete probe fixture {PROBE_ASSET_PATH}")

    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        if not unreal.EditorAssetLibrary.delete_asset(ASSET_PATH):
            raise RuntimeError(f"failed to delete existing fixture {ASSET_PATH}")

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    state_tree = asset_tools.create_asset(
        ASSET_NAME,
        PACKAGE_PATH,
        unreal.StateTree,
        None,
    )
    if not state_tree:
        raise RuntimeError(f"failed to create fixture {ASSET_PATH}")

    if not unreal.EditorAssetLibrary.save_asset(ASSET_PATH, only_if_is_dirty=False):
        raise RuntimeError(f"failed to save fixture {ASSET_PATH}")

    unreal.log_warning(f"UEPI StateTree fixture created: {ASSET_PATH}")


main()
