"""Create the checked LevelSequence fixture asset used by UEPI golden scans.

Run with Unreal's PythonScript commandlet, PythonScriptPlugin, and SequencerScripting enabled:

UnrealEditor-Cmd.exe GasDemo.uproject -run=pythonscript ^
  -Script="exec(open(r'Plugins/UEProjectIntelligence/Tools/create_level_sequence_fixture.py').read())" ^
  -EnablePlugins=PythonScriptPlugin -EnablePlugins=SequencerScripting -unattended -nop4 -nosplash -NullRHI
"""

import unreal


PACKAGE_PATH = "/Game/UEPI/Fixtures/Cinematics"
ASSET_NAME = "LS_UEPI_Simple"
ASSET_PATH = f"{PACKAGE_PATH}/{ASSET_NAME}"


def frame(value):
    return unreal.FrameNumber(value)


def add_double_key(channel, frame_number, value):
    channel.add_key(
        frame(frame_number),
        float(value),
        interpolation=unreal.MovieSceneKeyInterpolation.LINEAR,
    )


def main():
    unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)

    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        if not unreal.EditorAssetLibrary.delete_asset(ASSET_PATH):
            raise RuntimeError(f"failed to delete existing fixture {ASSET_PATH}")

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    sequence = asset_tools.create_asset(
        ASSET_NAME,
        PACKAGE_PATH,
        unreal.LevelSequence,
        unreal.LevelSequenceFactoryNew(),
    )
    if not sequence:
        raise RuntimeError(f"failed to create fixture {ASSET_PATH}")

    sequence.set_display_rate(unreal.FrameRate(24, 1))
    sequence.set_tick_resolution(unreal.FrameRate(24000, 1001))
    sequence.set_playback_start(0)
    sequence.set_playback_end(48)
    sequence.set_view_range_start(0.0)
    sequence.set_view_range_end(2.0)
    sequence.set_work_range_start(0.0)
    sequence.set_work_range_end(2.0)

    camera_binding = sequence.add_spawnable_from_class(unreal.CameraActor)
    camera_binding.set_name("UEPI_Camera")
    camera_binding.set_display_name("UEPI Camera")

    transform_track = camera_binding.add_track(unreal.MovieScene3DTransformTrack)
    transform_section = transform_track.add_section()
    transform_section.set_range(0, 48)

    channels = transform_section.get_all_channels()
    if len(channels) < 9:
        raise RuntimeError("expected a 3D transform section to expose 9 channels")

    add_double_key(channels[0], 0, 0.0)
    add_double_key(channels[0], 48, 320.0)
    add_double_key(channels[1], 0, -80.0)
    add_double_key(channels[1], 48, 80.0)
    add_double_key(channels[2], 0, 180.0)
    add_double_key(channels[2], 48, 220.0)

    camera_cut_track = sequence.add_track(unreal.MovieSceneCameraCutTrack)
    camera_cut_section = camera_cut_track.add_section()
    camera_cut_section.set_range(0, 48)
    camera_cut_section.set_camera_binding_id(sequence.get_binding_id(camera_binding))

    marked_frame = unreal.MovieSceneMarkedFrame()
    marked_frame.frame_number = frame(24)
    marked_frame.label = "UEPI Midpoint"
    sequence.add_marked_frame(marked_frame)

    if not unreal.EditorAssetLibrary.save_asset(ASSET_PATH, only_if_is_dirty=False):
        raise RuntimeError(f"failed to save fixture {ASSET_PATH}")

    unreal.log_warning(f"UEPI LevelSequence fixture created: {ASSET_PATH}")


main()
