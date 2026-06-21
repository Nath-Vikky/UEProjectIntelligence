"""Create checked AI fixture assets used by UEPI golden scans.

Run with Unreal's PythonScript commandlet and PythonScriptPlugin enabled:

UnrealEditor-Cmd.exe GasDemo.uproject -run=pythonscript ^
  -Script="exec(open(r'Plugins/UEProjectIntelligence/Tools/create_ai_fixture.py').read())" ^
  -EnablePlugins=PythonScriptPlugin -unattended -nop4 -nosplash -NullRHI
"""

import unreal


PACKAGE_PATH = "/Game/UEPI/Fixtures/AI"
BLACKBOARD_NAME = "BB_UEPI_Agent"
BEHAVIOR_TREE_NAME = "BT_UEPI_Patrol"
ENV_QUERY_NAME = "EQS_UEPI_FindPoint"

BLACKBOARD_PATH = f"{PACKAGE_PATH}/{BLACKBOARD_NAME}"
BEHAVIOR_TREE_PATH = f"{PACKAGE_PATH}/{BEHAVIOR_TREE_NAME}"
ENV_QUERY_PATH = f"{PACKAGE_PATH}/{ENV_QUERY_NAME}"


def delete_if_exists(asset_path):
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        if not unreal.EditorAssetLibrary.delete_asset(asset_path):
            raise RuntimeError(f"failed to delete existing fixture {asset_path}")


def make_key_type(class_path, outer):
    key_class = unreal.load_class(None, class_path)
    if not key_class:
        raise RuntimeError(f"failed to load blackboard key type {class_path}")
    return unreal.new_object(key_class, outer=outer)


def make_blackboard_entry(name, class_path, outer, instance_synced=False):
    entry = unreal.BlackboardEntry()
    entry.set_editor_property("entry_name", name)
    entry.set_editor_property("entry_description", f"UEPI fixture key {name}")
    entry.set_editor_property("entry_category", "UEPI")
    entry.set_editor_property("key_type", make_key_type(class_path, outer))
    entry.set_editor_property("instance_synced", instance_synced)
    return entry


def create_blackboard(asset_tools):
    blackboard = asset_tools.create_asset(
        BLACKBOARD_NAME,
        PACKAGE_PATH,
        unreal.BlackboardData,
        unreal.BlackboardDataFactory(),
    )
    if not blackboard:
        raise RuntimeError(f"failed to create fixture {BLACKBOARD_PATH}")

    keys = [
        make_blackboard_entry("TargetActor", "/Script/AIModule.BlackboardKeyType_Object", blackboard, True),
        make_blackboard_entry("PatrolLocation", "/Script/AIModule.BlackboardKeyType_Vector", blackboard),
        make_blackboard_entry("HasLineOfSight", "/Script/AIModule.BlackboardKeyType_Bool", blackboard),
    ]
    blackboard.set_editor_property("keys", keys)
    return blackboard


def create_behavior_tree(asset_tools, blackboard):
    behavior_tree = asset_tools.create_asset(
        BEHAVIOR_TREE_NAME,
        PACKAGE_PATH,
        unreal.BehaviorTree,
        unreal.BehaviorTreeFactory(),
    )
    if not behavior_tree:
        raise RuntimeError(f"failed to create fixture {BEHAVIOR_TREE_PATH}")

    # UBehaviorTree's core node fields are not exposed to UE Python in 5.3, but
    # the blackboard reference is preserved by the factory-created asset when set
    # through import text.
    for prop in ("blackboard_asset", "BlackboardAsset"):
        try:
            behavior_tree.set_editor_property(prop, blackboard)
            break
        except Exception:
            pass
    return behavior_tree


def try_create_env_query(asset_tools):
    try:
        env_query = asset_tools.create_asset(
            ENV_QUERY_NAME,
            PACKAGE_PATH,
            unreal.EnvQuery,
            None,
        )
    except Exception as exc:
        unreal.log_warning(f"UEPI EnvQuery fixture skipped: {exc}")
        return None

    if not env_query:
        unreal.log_warning("UEPI EnvQuery fixture skipped: create_asset returned None")
        return None
    return env_query


def save_required(asset_path):
    if not unreal.EditorAssetLibrary.save_asset(asset_path, only_if_is_dirty=False):
        raise RuntimeError(f"failed to save fixture {asset_path}")


def main():
    unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)
    for path in (ENV_QUERY_PATH, BEHAVIOR_TREE_PATH, BLACKBOARD_PATH):
        delete_if_exists(path)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    blackboard = create_blackboard(asset_tools)
    behavior_tree = create_behavior_tree(asset_tools, blackboard)
    env_query = try_create_env_query(asset_tools)

    save_required(BLACKBOARD_PATH)
    save_required(BEHAVIOR_TREE_PATH)
    if env_query:
        save_required(ENV_QUERY_PATH)

    unreal.log_warning(f"UEPI Blackboard fixture created: {BLACKBOARD_PATH}")
    unreal.log_warning(f"UEPI BehaviorTree fixture created: {BEHAVIOR_TREE_PATH}")
    if env_query:
        unreal.log_warning(f"UEPI EnvQuery fixture created: {ENV_QUERY_PATH}")


main()
