"""Create checked SoundWave and SoundCue fixture assets used by UEPI golden scans.

Run with Unreal's PythonScript commandlet and PythonScriptPlugin enabled:

UnrealEditor-Cmd.exe GasDemo.uproject -run=pythonscript ^
  -Script="exec(open(r'Plugins/UEProjectIntelligence/Tools/create_audio_fixture.py').read())" ^
  -EnablePlugins=PythonScriptPlugin -unattended -nop4 -nosplash -NullRHI
"""

import math
import os
import struct
import wave

import unreal


PACKAGE_PATH = "/Game/UEPI/Fixtures/Audio"
SOUND_WAVE_NAME = "SW_UEPI_Tone"
SOUND_CUE_NAME = "SC_UEPI_Tone"
SOUND_WAVE_PATH = f"{PACKAGE_PATH}/{SOUND_WAVE_NAME}"
SOUND_CUE_PATH = f"{PACKAGE_PATH}/{SOUND_CUE_NAME}"

SAMPLE_RATE = 22050
DURATION_SECONDS = 0.5
FREQUENCY_HZ = 440.0


def write_fixture_wav(filename):
    os.makedirs(os.path.dirname(filename), exist_ok=True)

    sample_count = int(SAMPLE_RATE * DURATION_SECONDS)
    fade_samples = max(1, int(SAMPLE_RATE * 0.02))
    pcm = bytearray()

    for sample_index in range(sample_count):
        t = sample_index / SAMPLE_RATE
        envelope = 1.0
        if sample_index < fade_samples:
            envelope = sample_index / fade_samples
        elif sample_index >= sample_count - fade_samples:
            envelope = (sample_count - sample_index - 1) / fade_samples

        value = int(0.35 * 32767.0 * envelope * math.sin(2.0 * math.pi * FREQUENCY_HZ * t))
        pcm.extend(struct.pack("<h", value))

    with wave.open(filename, "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(SAMPLE_RATE)
        wav_file.writeframes(bytes(pcm))


def delete_existing_assets():
    for asset_path in (SOUND_CUE_PATH, SOUND_WAVE_PATH):
        if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
            if not unreal.EditorAssetLibrary.delete_asset(asset_path):
                raise RuntimeError(f"failed to delete existing fixture {asset_path}")


def import_sound_wave(wav_filename):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", wav_filename)
    task.set_editor_property("destination_path", PACKAGE_PATH)
    task.set_editor_property("destination_name", SOUND_WAVE_NAME)
    task.set_editor_property("factory", unreal.SoundFactory())
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    if not task.imported_object_paths:
        raise RuntimeError(f"failed to import fixture wav {wav_filename}")

    sound_wave = unreal.load_asset(SOUND_WAVE_PATH)
    if not sound_wave:
        raise RuntimeError(f"failed to load imported sound wave {SOUND_WAVE_PATH}")

    subtitle = unreal.SubtitleCue()
    subtitle.set_editor_property("time", 0.0)
    subtitle.set_editor_property("text", "UEPI tone")
    sound_wave.set_editor_property("subtitles", [subtitle])
    sound_wave.set_editor_property("single_line", True)

    if not unreal.EditorAssetLibrary.save_asset(SOUND_WAVE_PATH, only_if_is_dirty=False):
        raise RuntimeError(f"failed to save fixture {SOUND_WAVE_PATH}")

    return sound_wave


def create_sound_cue(sound_wave):
    cue = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        SOUND_CUE_NAME,
        PACKAGE_PATH,
        unreal.SoundCue,
        unreal.SoundCueFactoryNew(),
    )
    if not cue:
        raise RuntimeError(f"failed to create fixture {SOUND_CUE_PATH}")

    mixer = unreal.new_object(
        unreal.SoundNodeMixer,
        outer=cue,
        name="UEPI_Mixer",
    )
    wave_player = unreal.new_object(
        unreal.SoundNodeWavePlayer,
        outer=cue,
        name="UEPI_WavePlayer",
    )
    wave_player.set_editor_property("sound_wave_asset_ptr", sound_wave)
    mixer.set_editor_property("child_nodes", [wave_player])
    cue.set_editor_property("first_node", mixer)
    cue.set_editor_property("volume_multiplier", 0.75)
    cue.set_editor_property("pitch_multiplier", 1.0)

    if not cue.get_editor_property("first_node"):
        raise RuntimeError("SoundCueFactoryNew did not create a WavePlayer first node")

    if not unreal.EditorAssetLibrary.save_asset(SOUND_CUE_PATH, only_if_is_dirty=False):
        raise RuntimeError(f"failed to save fixture {SOUND_CUE_PATH}")

    return cue


def main():
    unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)
    delete_existing_assets()

    wav_filename = os.path.join(
        unreal.Paths.project_saved_dir(),
        "UEProjectIntelligence",
        "Fixtures",
        f"{SOUND_WAVE_NAME}.wav",
    )
    write_fixture_wav(wav_filename)

    sound_wave = import_sound_wave(wav_filename)
    create_sound_cue(sound_wave)

    unreal.log_warning(f"UEPI Audio fixtures created: {SOUND_WAVE_PATH}, {SOUND_CUE_PATH}")


main()
