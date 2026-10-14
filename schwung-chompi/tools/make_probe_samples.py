#!/usr/bin/env python3
"""Write the tone-labelled sample set mode_bank_probe.cpp expects.

Each file is a pure tone at a different frequency, so a spectrum tells you
*which* sample played -- the probe can then say "the bank knob works" instead
of "something made a noise". Written where the module will look for them:
<out_dir>/ as the module's samples/ directory.

    python3 tools/make_probe_samples.py /tmp/fullmod/samples
"""
import math
import os
import struct
import sys
import wave

SR, SEC, AMP = 48000, 1.0, 16000

TONES = {
    "cubbi_a1.wav": 440, "cubbi_a2.wav": 480, "cubbi_a3.wav": 520,
    "cubbi_a4.wav": 560, "cubbi_a5.wav": 600,
    "cubbi_b1.wav": 640, "cubbi_b2.wav": 680,
    # jammi_a1 is the card's own tone: C4, 261.63 Hz. The 840 that used to be
    # here was never in the file, and mode_bank_probe's reference was made from
    # it -- see the note on `one` in mode_bank_probe.cpp.
    "jammi_a1.wav": 261.63, "jammi_a2.wav": 880, "jammi_a3.wav": 920,
    "jammi_b1.wav": 1000,
}


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for name, freq in TONES.items():
        path = os.path.join(out_dir, name)
        with wave.open(path, "w") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(SR)
            frames = bytearray()
            for i in range(int(SR * SEC)):
                s = int(AMP * math.sin(2 * math.pi * freq * i / SR))
                frames += struct.pack("<hh", s, s)
            w.writeframes(bytes(frames))
        print(f"{path}  {freq} Hz")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/fullmod/samples")
