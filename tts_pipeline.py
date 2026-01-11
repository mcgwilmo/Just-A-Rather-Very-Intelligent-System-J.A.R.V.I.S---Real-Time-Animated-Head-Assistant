import sys
import json
import string
from pathlib import Path



import pyttsx3
import soundfile as sf
import nltk
nltk.download('cmudict')
from nltk.corpus import cmudict

SHAPES = {"AA", "CH", "EE", "IH", "OH", "OU", "TH", "dd", "ff", "kk", "nn", "pp", "rr", "ss"}

_cmu = None
def get_cmu():
    global _cmu
    if _cmu is None:
        _cmu = cmudict.dict()
    return _cmu

def arpabet_to_shape(arpa: str) -> str:
    a = arpa.upper()
    def pick(*candidates):
        for c in candidates:
            if c in SHAPES: return c
        return ""

    if a in {"AA", "AE", "AH"}: return pick("AA")
    if a in {"IY", "IH", "EY", "IX"}: return pick("EE", "IH")
    if a == "EH": return pick("IH", "EE")
    if a in {"OW", "AO"}: return pick("OH", "OU")
    if a in {"AW", "AY", "OY", "UH", "UW"}: return pick("OU", "OH")

    if a in {"P", "B", "M"}: return pick("pp")
    if a in {"F", "V"}: return pick("ff")
    if a in {"K", "G"}: return pick("kk")
    if a in {"N", "NG"}: return pick("nn")
    if a in {"D", "T"}: return pick("dd")
    if a in {"R", "ER"}: return pick("rr")
    if a in {"S", "Z", "SH", "ZH"}: return pick("ss")
    if a in {"CH", "JH"}: return pick("CH")
    if a in {"TH", "DH"}: return pick("TH")

    if a in SHAPES: return a
    return ""

def text_to_arpabet(text: str):
    cmu = get_cmu()
    phones = []
    for raw_word in text.split():
        word = "".join(ch for ch in raw_word if ch.isalpha() or ch == "'").upper()
        if not word: continue

        if word in cmu:
            pron = cmu[word][0]
            for p in pron:
                if p and p[-1].isdigit(): p = p[:-1]
                phones.append(p)
        else:
            for ch in word:
                phones.append(ch.upper())
    return phones

def phones_to_shapes(phones):
    return [s for p in phones if (s := arpabet_to_shape(p))]

def synthesize_tts(text: str, wav_path: Path, emotion: str = "neutral"):
    engine = pyttsx3.init()

    base_rate = engine.getProperty("rate")
    base_volume = engine.getProperty("volume")

    # crude emotion mapping
    if emotion == "happy":
        engine.setProperty("rate", int(base_rate * 1.15))
        engine.setProperty("volume", min(base_volume * 1.1, 1.0))
    elif emotion == "excited":
        engine.setProperty("rate", int(base_rate * 1.25))
        engine.setProperty("volume", min(base_volume * 1.1, 1.0))
    elif emotion in ("sad", "gloomy"):
        engine.setProperty("rate", int(base_rate * 0.85))
        engine.setProperty("volume", base_volume * 0.9)
    elif emotion == "angry":
        engine.setProperty("rate", int(base_rate * 1.1))
    # else: neutral/energetic → default

    wav_path = wav_path.resolve()
    engine.save_to_file(text, str(wav_path))
    engine.runAndWait()

def main():
    # NEW: expect 4 args
    if len(sys.argv) != 5:
        print("Usage: python tts_pipeline.py <script_txt> <out_wav> <out_json> <emotion>")
        sys.exit(1)

    script_path = Path(sys.argv[1])
    out_wav = Path(sys.argv[2])
    out_json = Path(sys.argv[3])
    emotion = sys.argv[4]  # "neutral", "happy", ...

    text = script_path.read_text(encoding="utf-8").strip()
    if not text:
        print("ERROR: script.txt empty")
        sys.exit(1)

    print(f"[TTS] Synthesizing {out_wav} with emotion={emotion}")
    synthesize_tts(text, out_wav, emotion=emotion)

    audio, sr = sf.read(str(out_wav))
    sf.write(str(out_wav), audio, sr, format="WAV", subtype="PCM_16")
    duration = len(audio) / sr
    print(f"[AUDIO] Duration = {duration:.3f}s")

    phones = text_to_arpabet(text)
    shapes = phones_to_shapes(phones)

    slots = []
    N = len(shapes)
    for i, sh in enumerate(shapes):
        start = (i / N) * duration
        end = ((i + 1) / N) * duration
        slots.append({"shape": sh, "start": start, "end": end})

    align = {
        "audio": str(out_wav).replace("\\", "/"),
        "phonemes": slots,
        "emotion": emotion,  # IMPORTANT: HeadViewerApp reads this
    }

    out_json.write_text(json.dumps(align, indent=2))
    print(f"[ALIGN] Wrote alignment JSON to {out_json}")

if __name__ == "__main__":
    main()
