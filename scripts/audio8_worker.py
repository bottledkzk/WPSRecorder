"""Synthesize a batch with one load of the pinned local Audio8 0.1B runtime."""

from pathlib import Path
import json
import os
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
    root, input_file, voice, *legacy_output = sys.argv[1:]
    source = Path(input_file).read_text(encoding="utf-8")
    lines = [{"text": source, "output": legacy_output[0]}] if legacy_output else json.loads(source)
    if not lines or any(not line["text"].strip() for line in lines):
        raise ValueError("Provide non-empty speech text")
    root = Path(root)
    sys.path.insert(0, str(root / "runtime"))
    from arktts_runtime.runtime import ArkTtsRuntime
    import numpy as np
    import soundfile

    model = root / "model"
    voices = root / "voices"
    if voice == "mandarin-female":
        voices = Path(__file__).resolve().parent / "voices"
        profile = json.loads((voices / voice / "meta.json").read_text(encoding="utf-8"))
        model_manifest = json.loads((model / "runtime_manifest.json").read_text(encoding="utf-8"))
        if profile["model_fingerprint"] != model_manifest["model_fingerprint"]:
            raise ValueError("Mandarin female voice requires the bundled Audio8 0.1B model")
    threads = max(1, min(4, (os.cpu_count() or 2) // 2))
    runtime = ArkTtsRuntime(model, voices, None, None, threads)
    rate = int(runtime.manifest["sample_rate"])
    for line in lines:
        parts = []
        for utterance in utterances(line["text"].strip()):
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
        soundfile.write(line["output"], np.concatenate(parts), rate)


if __name__ == "__main__":
    main()
