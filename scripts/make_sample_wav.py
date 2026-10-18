"""Writes samples/swoosh.wav: a short rising whoosh used by the demo's "wave_peak" event."""
import math
import random
import struct
import sys
import wave

RATE = 44100
DURATION = 0.6
out = sys.argv[1] if len(sys.argv) > 1 else "samples/swoosh.wav"
random.seed(7)
frames = []
smooth = 0.0
for i in range(int(RATE * DURATION)):
    t = i / RATE
    env = math.sin(math.pi * t / DURATION) ** 2          # fade in and out
    smooth = smooth * 0.85 + random.uniform(-1, 1) * 0.15  # filtered noise
    tone = math.sin(2 * math.pi * (300 + 900 * t / DURATION) * t)
    v = env * (0.6 * smooth * 3 + 0.25 * tone)
    pan = t / DURATION                                     # sweeps left to right
    frames.append(struct.pack("<hh", int(max(-1, min(1, v * (1 - pan))) * 32767), int(max(-1, min(1, v * pan)) * 32767)))
with wave.open(out, "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(b"".join(frames))
print("wrote", out)
