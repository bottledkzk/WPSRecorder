"""Synthesize one line with the pinned local Audio8 0.1B INT8 runtime."""

from pathlib import Path
import json
import sys


def utterances(text: str):
    while len(text) > 120:
        stop = max(text.rfind(mark, 40, 121) for mark in "。！？.!?；;，,\n") + 1
        stop = stop or 120
        yield text[:stop].strip()
        text = text[stop:].strip()
    if text:
        yield text


def main() -> None:
    root, text_file, voice, output = sys.argv[1:]
    root = Path(root)
    sys.path.insert(0, str(root / "runtime"))
    from arktts_runtime.runtime import ArkTtsRuntime
    import numpy as np
    import soundfile

    text = Path(text_file).read_text(encoding="utf-8").strip()
    model = root / "model"
    voices = root / "voices"
    if voice == "mandarin-female":
        voices = Path(__file__).resolve().parent / "voices"
        profile = json.loads((voices / voice / "meta.json").read_text(encoding="utf-8"))
        model_manifest = json.loads((model / "runtime_manifest.json").read_text(encoding="utf-8"))
        if profile["model_fingerprint"] != model_manifest["model_fingerprint"]:
            raise ValueError("Mandarin female voice requires the bundled Audio8 0.1B model")
    runtime = ArkTtsRuntime(model, voices, None, None, 4)
    rate = int(runtime.manifest["sample_rate"])
    parts = []
    for utterance in utterances(text):
        maximum = min(1024, max(192, len(utterance) * 12))
        audio, codes = runtime.synthesize(
            text=utterance, voice=voice, max_new_tokens=maximum, temperature=0.7,
            top_p=0.9, top_k=50, seed=42,
        )
        if codes.shape[-1] >= maximum or audio.size == 0:
            raise ValueError("Audio8 reached the generation limit")
        if parts:
            parts.append(np.zeros(rate // 20, dtype=audio.dtype))
        parts.append(audio)
    soundfile.write(output, np.concatenate(parts), rate)


if __name__ == "__main__":
    main()
