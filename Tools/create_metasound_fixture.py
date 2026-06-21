"""Create the checked MetaSound fixture asset used by UEPI golden scans.

Run with Unreal's PythonScript commandlet and PythonScriptPlugin enabled:

UnrealEditor-Cmd.exe GasDemo.uproject -run=pythonscript ^
  -Script="exec(open(r'Plugins/UEProjectIntelligence/Tools/create_metasound_fixture.py').read())" ^
  -EnablePlugins=PythonScriptPlugin -unattended -nop4 -nosplash -NullRHI
"""

import unreal


PACKAGE_PATH = "/Game/UEPI/Fixtures/MetaSound"
ASSET_NAME = "MS_UEPI_Tone"
ASSET_PATH = f"{PACKAGE_PATH}/{ASSET_NAME}"


def metasound_class_name(namespace, name, variant):
    class_name = unreal.MetasoundFrontendClassName()
    class_name.set_editor_property("namespace", namespace)
    class_name.set_editor_property("name", name)
    class_name.set_editor_property("variant", variant)
    return class_name


def require_result(result, action):
    if result != unreal.MetaSoundBuilderResult.SUCCEEDED:
        raise RuntimeError(f"{action} failed with result {result}")


def main():
    unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)

    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        if not unreal.EditorAssetLibrary.delete_asset(ASSET_PATH):
            raise RuntimeError(f"failed to delete existing fixture {ASSET_PATH}")

    builder_subsystem = unreal.get_engine_subsystem(unreal.MetaSoundBuilderSubsystem)
    editor_subsystem = unreal.get_editor_subsystem(unreal.MetaSoundEditorSubsystem)
    if not builder_subsystem or not editor_subsystem:
        raise RuntimeError("MetaSound builder/editor subsystems are unavailable")

    (
        builder,
        _on_play_node_output,
        _on_finished_node_input,
        audio_out_node_inputs,
        result,
    ) = builder_subsystem.create_source_builder(
        "UEPI_MetaSoundFixture",
        output_format=unreal.MetaSoundOutputAudioFormat.MONO,
        is_one_shot=True,
    )
    require_result(result, "create_source_builder")
    if not audio_out_node_inputs:
        raise RuntimeError("source builder did not create an audio output input")

    sine_node, result = builder.add_node_by_class_name(
        metasound_class_name("UE", "Sine", "Audio"),
        1,
    )
    require_result(result, "add Sine node")

    sine_audio_output, result = builder.find_node_output_by_name(sine_node, "Audio")
    require_result(result, "find Sine Audio output")

    result = builder.connect_nodes(sine_audio_output, audio_out_node_inputs[0])
    require_result(result, "connect Sine Audio output to graph output")

    metasound_asset, result = editor_subsystem.build_to_asset(
        builder,
        "UEProjectIntelligence",
        ASSET_NAME,
        PACKAGE_PATH,
    )
    require_result(result, "build_to_asset")
    if not metasound_asset:
        raise RuntimeError("build_to_asset returned no asset")

    if not unreal.EditorAssetLibrary.save_asset(ASSET_PATH, only_if_is_dirty=False):
        raise RuntimeError(f"failed to save fixture {ASSET_PATH}")

    unreal.log_warning(f"UEPI MetaSound fixture created: {ASSET_PATH}")


main()
