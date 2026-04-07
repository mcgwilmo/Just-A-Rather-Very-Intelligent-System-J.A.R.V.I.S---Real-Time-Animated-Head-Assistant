# J.A.R.V.I.S. Real-Time Animated Head Assistant

This project is a real-time 3D talking-head demo built on top of `GLOO`. It renders a floating stylized head, drives mouth shapes from phoneme timing, layers in emotion presets, and plays generated speech through a small Python text-to-speech pipeline. The result is a lightweight embodied assistant prototype rather than a text-only chatbot.

The repository contains the head viewer, mesh deformation code, and the Python scripts that turn text or an LLM response into audio plus a simple phoneme alignment JSON. The code currently assumes that it is being run inside a larger course/project environment where `gloo/` and the asset folders already exist.

## What It Does

- Loads a head mesh and morph targets for phoneme-driven facial deformation.
- Plays synthesized speech and animates the mouth against a generated alignment file.
- Supports `Generate Speech From Text` in the ImGui UI, which uses local text input and runs `tts_pipeline.py`.
- Supports `Talk to JARVIS`, which sends the prompt to OpenAI, gets back spoken text plus an emotion label, then runs TTS.
- Adds simple expressive behavior on top of lip sync, including blinking, idle head motion, speaking micro-motions, and emotion-driven playback.
- Exposes manual controls for phoneme blending and material tuning.

## Repository Layout

- `main.cpp`: app entry point for the head viewer.
- `HeadViewerApp.cpp` / `HeadViewerApp.hpp`: scene setup, GUI, audio playback, lip sync, idle animation, and Python pipeline integration.
- `HeadNode.cpp` / `HeadNode.hpp`: mesh loading, phoneme pose loading, additive blendshape-style deformation, and emotion state.
- `tts_pipeline.py`: local text-to-speech plus phoneme-shape alignment generation.
- `llm_tts_pipeline.py`: OpenAI prompt -> spoken reply + emotion -> TTS pipeline wrapper.
- `SkeletonViewerApp.cpp` / `SkeletonViewerApp.hpp`, `SkeletonNode.cpp` / `SkeletonNode.hpp`: separate skeleton viewer / debugging utilities.
- `CubeMap.cpp` / `CubeMap.hpp`: cubemap support for environment rendering.
- `json.hpp`: bundled `nlohmann/json` header.
- `Project Paper + Description + Methods.pdf`: project writeup.

## External Dependencies

This folder is not fully standalone. The C++ code expects a surrounding environment that provides:

- `GLOO` and its headers/source tree
- SDL2
- SDL2_mixer
- OpenGL / GLM / ImGui as required by `GLOO`
- the mesh and phoneme assets referenced by relative path

The Python pipeline expects:

- Python 3
- `openai`
- `pyttsx3`
- `soundfile`
- `nltk`

Example install:

```bash
pip install openai pyttsx3 soundfile nltk
```

On first run, `tts_pipeline.py` downloads the CMU pronunciation dictionary via `nltk.download("cmudict")`.

## Expected Asset Layout

The current source uses hardcoded relative paths, so the working directory matters.

The app expects:

- head meshes such as `head_variants/head3/mesh/head.obj`
- phoneme data at `../assets/phonemes/head_phonemes.json`
- generated audio files in `../assets/audio/`

At runtime, the viewer writes or reads:

- `../assets/audio/script.txt`
- `../assets/audio/line.wav`
- `../assets/audio/line_align.json`

Because these paths are relative, the executable is expected to run from a build directory whose parent contains the `assets/` folder.

## OpenAI Setup

`llm_tts_pipeline.py` looks for an API key in this order:

1. `OPENAI_API_KEY`
2. `openai_key.txt` placed next to `llm_tts_pipeline.py`

When the user presses `Talk to JARVIS`, the script:

1. Reads the prompt from `script.txt`
2. Calls OpenAI for JSON containing `{ text, emotion }`
3. Overwrites `script.txt` with the spoken reply
4. Runs `tts_pipeline.py`
5. Produces `line.wav` and `line_align.json`

The current model in code is `gpt-4.1-mini`.

## Running the Demo

There is no build file in this folder, so build instructions depend on the parent `GLOO` project you are integrating into. Once the app is compiled, `main.cpp` launches `HeadViewerApp` and optionally accepts a mesh path:

```bash
./your_app head_variants/head3/mesh/head.obj
```

If no mesh path is provided, it defaults to:

```text
head_variants/head3/mesh/head.obj
```

Inside the UI:

- Use `Generate Speech From Text` to synthesize speech from the text box without calling the LLM.
- Use `Talk to JARVIS` to send the prompt through OpenAI and speak the returned response.
- Use `Phoneme Visualization` to inspect individual mouth shapes manually.
- Use `Emotion Controls` and `Material Controls` to tune the rendered result.

## How Lip Sync Works

The lip-sync path is intentionally simple:

1. Text is converted to ARPAbet-like phones using CMUdict.
2. Phones are mapped into a smaller set of mouth shapes such as `AA`, `EE`, `OH`, `pp`, `ff`, and `TH`.
3. The script divides the generated audio duration across those shapes uniformly.
4. The C++ viewer reads `line_align.json`, plays the WAV through SDL_mixer, and blends the corresponding facial targets over time.

This is enough for a convincing demo, but it is not true forced alignment and does not estimate real phoneme timings from the waveform.

## Current Limitations

- This repository does not include the `GLOO` framework or a build configuration.
- Required mesh / phoneme / audio asset folders are referenced but not included here.
- The phoneme timing is duration-sliced, not audio-aligned.
- The TTS pipeline uses offline system speech via `pyttsx3`, so voice quality and behavior depend on the host machine.
- Several paths are hardcoded and should be made configurable if this is turned into a reusable project.

## Suggested Next Cleanup

If this project is going to be shared outside the original course environment, the next high-value improvements are:

- add a `CMakeLists.txt`
- move hardcoded asset paths into config/CLI flags
- include or document the exact expected asset bundle
- add a `requirements.txt` for the Python scripts
- replace naive phoneme timing with a real aligner
