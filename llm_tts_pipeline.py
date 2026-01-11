#!/usr/bin/env python3
import os
import sys
import json
import subprocess
from pathlib import Path

from openai import OpenAI

def make_client() -> OpenAI:
    """
    Create an OpenAI client using either:
      1) OPENAI_API_KEY environment variable, or
      2) an 'openai_key.txt' file next to this script.
    """
    api_key = os.getenv("OPENAI_API_KEY")

    # Fallback: read from openai_key.txt next to this script
    if not api_key:
        key_path = Path(__file__).with_name("openai_key.txt")
        if key_path.exists():
            api_key = key_path.read_text(encoding="utf-8").strip()

    if not api_key:
        raise RuntimeError(
            "No OpenAI API key found. Set the OPENAI_API_KEY env var "
            "or create an 'openai_key.txt' file next to llm_tts_pipeline.py "
            "containing your key."
        )

    return OpenAI(api_key=api_key)


# Global client used by the rest of the script
client = make_client()

ALLOWED_EMOTIONS = ["neutral", "happy", "sad", "angry", "excited", "energetic", "gloomy"]


def get_reply_and_emotion(prompt: str):
    """
    Ask ChatGPT for (text, emotion) as structured JSON.
    """
    resp = client.chat.completions.create(
        model="gpt-4.1-mini",  # or any chat-capable model you have access to
        messages=[
            {
                "role": "system",
                "content": (
                    "You are a friendly speaking assistant in a graphics demo. "
                    "Given a user prompt, produce:\n"
                    "  - text: what the assistant should say out loud\n"
                    "  - emotion: one of "
                    "['neutral','happy','sad','angry','excited','energetic','gloomy'] "
                    "describing the overall tone of the reply.\n"
                    "Keep replies 1–2 sentences. Respond ONLY as JSON."
                ),
            },
            {"role": "user", "content": prompt},
        ],
        response_format={
            "type": "json_schema",
            "json_schema": {
                "name": "speech_with_emotion",
                "schema": {
                    "type": "object",
                    "properties": {
                        "text": {"type": "string"},
                        "emotion": {
                            "type": "string",
                            "enum": ALLOWED_EMOTIONS,
                        },
                    },
                    "required": ["text", "emotion"],
                    "additionalProperties": False,
                },
                "strict": True,
            },
        },
        temperature=0.7,
    )

    content = resp.choices[0].message.content
    data = json.loads(content)
    text = data["text"].strip()
    emotion = data["emotion"]
    if emotion not in ALLOWED_EMOTIONS:
        emotion = "neutral"
    return text, emotion


def main():
    # We expect 3 args from C++:
    #   llm_tts_pipeline.py <script_txt> <out_wav> <out_json>
    if len(sys.argv) != 4:
        print("Usage: python llm_tts_pipeline.py <script_txt> <out_wav> <out_json>")
        sys.exit(1)

    script_path = Path(sys.argv[1])
    out_wav = Path(sys.argv[2])
    out_json = Path(sys.argv[3])

    if not script_path.exists():
        print(f"ERROR: {script_path} does not exist.")
        sys.exit(1)

    prompt = script_path.read_text(encoding="utf-8").strip()
    if not prompt:
        print("ERROR: script.txt (prompt) is empty.")
        sys.exit(1)

    print("[LLM] Sending prompt to ChatGPT...")
    reply_text, emotion = get_reply_and_emotion(prompt)
    print(f"[LLM] Got reply with emotion={emotion!r}")

    # Overwrite script.txt with the actual spoken text
    script_path.write_text(reply_text, encoding="utf-8")

    # Call your existing TTS pipeline, which expects:
    #   tts_pipeline.py <script_txt> <out_wav> <out_json> <emotion>
    tts_script = Path(__file__).with_name("tts_pipeline.py")

    cmd = [
        sys.executable,
        str(tts_script),
        str(script_path),
        str(out_wav),
        str(out_json),
        emotion,
    ]
    print("[TTS] Running:", " ".join(cmd))
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
