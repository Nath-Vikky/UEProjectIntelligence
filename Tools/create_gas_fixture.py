"""Create checked Gameplay Ability System fixture assets used by UEPI golden scans.

Run with Unreal's PythonScript commandlet, PythonScriptPlugin, and GameplayAbilities enabled:

UnrealEditor-Cmd.exe GasDemo.uproject -run=pythonscript ^
  -Script="exec(open(r'Plugins/UEProjectIntelligence/Tools/create_gas_fixture.py').read())" ^
  -EnablePlugins=PythonScriptPlugin -EnablePlugins=GameplayAbilities -unattended -nop4 -nosplash -NullRHI
"""

import unreal


PACKAGE_PATH = "/Game/UEPI/Fixtures/GAS"
ABILITY_NAME = "GA_UEPI_Pulse"
EFFECT_NAME = "GE_UEPI_Pulse"
CUE_NAME = "GCN_UEPI_Pulse_Static"

ABILITY_PATH = f"{PACKAGE_PATH}/{ABILITY_NAME}"
EFFECT_PATH = f"{PACKAGE_PATH}/{EFFECT_NAME}"
CUE_PATH = f"{PACKAGE_PATH}/{CUE_NAME}"


def delete_if_exists(asset_path):
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        if not unreal.EditorAssetLibrary.delete_asset(asset_path):
            raise RuntimeError(f"failed to delete existing fixture {asset_path}")


def create_ability_blueprint(asset_tools):
    factory = unreal.GameplayAbilitiesBlueprintFactory()
    supported_class = factory.get_editor_property("supported_class")
    ability = asset_tools.create_asset(ABILITY_NAME, PACKAGE_PATH, supported_class, factory)
    if not ability:
        raise RuntimeError(f"failed to create fixture {ABILITY_PATH}")
    return ability


def create_blueprint(asset_tools, asset_name, parent_class):
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    blueprint = asset_tools.create_asset(asset_name, PACKAGE_PATH, factory.get_editor_property("supported_class"), factory)
    if not blueprint:
        raise RuntimeError(f"failed to create fixture {PACKAGE_PATH}/{asset_name}")
    return blueprint


def save_required(asset_path):
    if not unreal.EditorAssetLibrary.save_asset(asset_path, only_if_is_dirty=False):
        raise RuntimeError(f"failed to save fixture {asset_path}")


def main():
    unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)
    for path in (CUE_PATH, EFFECT_PATH, ABILITY_PATH):
        delete_if_exists(path)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    ability = create_ability_blueprint(asset_tools)
    effect = create_blueprint(asset_tools, EFFECT_NAME, unreal.GameplayEffect.static_class())
    cue = create_blueprint(asset_tools, CUE_NAME, unreal.GameplayCueNotify_Static.static_class())

    for blueprint in (ability, effect, cue):
        blueprint.modify()

    save_required(ABILITY_PATH)
    save_required(EFFECT_PATH)
    save_required(CUE_PATH)

    unreal.log_warning(f"UEPI GameplayAbility fixture created: {ABILITY_PATH}")
    unreal.log_warning(f"UEPI GameplayEffect fixture created: {EFFECT_PATH}")
    unreal.log_warning(f"UEPI GameplayCueNotify fixture created: {CUE_PATH}")


main()
