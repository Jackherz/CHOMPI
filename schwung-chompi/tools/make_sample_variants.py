#!/usr/bin/env python3
"""Write the sample flavours a user's library actually holds.

The engine streams raw bytes and assumes a 16-bit stereo stream starting at
byte 44; the port's WAV view (module/dsp/port/wav_view.h) translates anything
else. This writes one file per flavour so the probe can measure each:

    python3 tools/make_wav_variants.py /tmp/wavfmt/samples

Every file holds the same 440 Hz stereo tone unless its name says otherwise,
so "did it decode" is a question about the spectrum, not about taste.

    user_16bit_stereo.wav          the engine's own format (must be untouched)
    user_24bit_stereo.wav          most DAW/library files today
    user_32bit_stereo.wav
    user_float32_stereo.wav
    user_float64_stereo.wav
    user_8bit_stereo.wav           unsigned 8-bit
    user_extensible24_stereo.wav   24-bit in a 32-bit WAVE_FORMAT_EXTENSIBLE
    user_listchunk_stereo.wav      16-bit, but with a LIST/INFO chunk first
    user_junkchunk_stereo.wav      16-bit, with an unknown chunk before `data`
    user_16bit_mono.wav            mono (must not play at double speed)
    user_24bit_mono.wav
    user_quad_stereo.wav           4 channels (the first two are used)
    user_ulaw_stereo.wav           G.711 u-law
    user_alaw_stereo.wav           G.711 A-law
    user_truncated_stereo.wav      header claims 4x the data it has
    user_adpcm.wav                 IMA ADPCM: must be silent, not noise
    user_notawav.wav               random bytes with a .wav name: untouched
    user_16bit_stereo.aiff         AIFF: big-endian 16-bit
    user_24bit_stereo.aiff         AIFF: big-endian 24-bit
    user_8bit_stereo.aiff          AIFF: signed 8-bit (WAV's is unsigned)
    user_16bit_mono.aiff           AIFF: mono
    user_sowt16_stereo.aifc        AIFF-C: 'sowt' = little-endian PCM
    user_fl32_stereo.aifc          AIFF-C: big-endian float32
    user_ulaw_stereo.aifc          AIFF-C: G.711 u-law
    twotone_16bit_stereo.aiff      440 Hz then 880 Hz, AIFF
    ramp_16bit_stereo.wav          16-bit, quiet -> loud (for reverse)
    ramp_24bit_stereo.wav          24-bit, quiet -> loud (for reverse)
    twotone_24bit_stereo.wav       440 Hz then 880 Hz (for start/end regions)
"""
import math
import os
import struct
import sys

SR, SEC = 48000, 1.0
AMP = 0.5

# channel-count-independent payload helpers ---------------------------------


def tone(freq, n, ch=2, amp=AMP):
    """Interleaved float samples, `ch` per frame."""
    out = []
    for i in range(n):
        s = amp * math.sin(2 * math.pi * freq * i / SR)
        out.extend([s] * ch)
    return out


def ramp(n, ch=2, amp=AMP):
    """Quiet at the start, loud at the end: reverse is measurable."""
    out = []
    for i in range(n):
        s = amp * (i / max(1, n - 1)) * math.sin(2 * math.pi * 440.0 * i / SR)
        out.extend([s] * ch)
    return out


def twotone(n, ch=2, amp=AMP):
    """440 Hz for the first half, 880 Hz for the second."""
    out = []
    for i in range(n):
        f = 440.0 if i < n // 2 else 880.0
        s = amp * math.sin(2 * math.pi * f * i / SR)
        out.extend([s] * ch)
    return out


# encoders ------------------------------------------------------------------


def pcm(samples, bits):
    if bits == 8:
        return bytes(max(0, min(255, int(s * 128) + 128)) for s in samples)
    if bits == 16:
        return b"".join(struct.pack("<h", max(-32768, min(32767, int(s * 32767)))) for s in samples)
    if bits == 24:
        out = bytearray()
        for s in samples:
            v = max(-8388608, min(8388607, int(s * 8388607))) & 0xFFFFFF
            out += struct.pack("<I", v)[:3]
        return bytes(out)
    if bits == 32:
        return b"".join(struct.pack("<i", max(-2**31, min(2**31 - 1, int(s * 2147483647)))) for s in samples)
    raise ValueError(bits)


def float_pcm(samples, bits):
    fmt = "<f" if bits == 32 else "<d"
    return b"".join(struct.pack(fmt, s) for s in samples)


def ulaw_encode(s):
    """G.711 u-law: the standard encoder, so the decoder is checked against
    something that is not itself."""
    bias, clip = 0x84, 32635
    x = max(-clip, min(clip, int(s * 32767)))
    sign = 0x80 if x < 0 else 0
    if sign:
        x = -x
    x += bias
    exp = 7
    mask = 0x4000
    while exp > 0 and not (x & mask):
        exp -= 1
        mask >>= 1
    mant = (x >> (exp + 3)) & 0x0F
    return (~(sign | (exp << 4) | mant)) & 0xFF


def alaw_encode(s):
    """G.711 A-law, standard encoder."""
    x = int(s * 32767)
    sign = 0x00 if x >= 0 else 0x80
    if sign:
        x = -x
    x = min(x, 32767)
    if x < 256:
        seg, mant = 0, x >> 4
    else:
        seg = 1
        while seg < 7 and x >= (256 << seg):
            seg += 1
        mant = (x >> (seg + 3)) & 0x0F
    return ((sign | (seg << 4) | mant) ^ 0x55) & 0xFF


# container -----------------------------------------------------------------


def chunk(cid, body):
    return cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) % 2 else b"")


def fmt_chunk(fmt, ch, bits, valid_bits=None, rate=SR):
    block = ch * bits // 8
    if fmt == 0xFFFE:
        guid = struct.pack("<H", 1 if valid_bits else 3) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
        if valid_bits:
            guid = struct.pack("<H", 1) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
        body = struct.pack("<HHIIHH", fmt, ch, rate, rate * block, block, bits)
        body += struct.pack("<HHI", 22, valid_bits or bits, 3) + guid
    else:
        body = struct.pack("<HHIIHH", fmt, ch, rate, rate * block, block, bits)
    return chunk(b"fmt ", body)


def write_wav(path, data, fmt=1, ch=2, bits=16, pre=b"", valid_bits=None,
              declared_len=None, rate=SR):
    body = b"WAVE" + pre + fmt_chunk(fmt, ch, bits, valid_bits, rate) + chunk(b"data", data)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body) if declared_len is None
                                      else declared_len) + body)
    print(f"{os.path.basename(path):32s} {len(data):8d} bytes of data")


def ext80(rate):
    """80-bit IEEE extended, which is how AIFF stores the sample rate."""
    if rate == 0:
        return b"\x00" * 10
    e = 16383 + 31
    m = int(rate)
    while m < (1 << 31):
        m <<= 1
        e -= 1
    while m >= (1 << 32):
        m >>= 1
        e += 1
    # 2 bytes of exponent + 8 bytes of mantissa = the 10 bytes AIFF expects
    return struct.pack(">H", e) + struct.pack(">I", m) + b"\x00\x00\x00\x00"


def aiff_chunk(cid, body):
    out = cid + struct.pack(">I", len(body)) + body
    if len(body) % 2:
        out += b"\x00"
    return out


def write_aiff(path, data, ch=2, bits=16, rate=SR, aifc=False, compression=b"NONE",
               frames=None):
    """AIFF (big-endian PCM) or AIFF-C (with a compression type)."""
    if frames is None:
        frames = len(data) // (ch * bits // 8)
    comm = struct.pack(">HIH", ch, frames, bits) + ext80(rate)
    if aifc:
        comm += compression + b"\x00\x00"  # 4CC + empty pstring
    sound = struct.pack(">II", 0, 0) + data
    form = b"AIFF" if not aifc else b"AIFC"
    if aifc:
        form += aiff_chunk(b"FVER", struct.pack(">I", 0xA2805140))
    body = form + aiff_chunk(b"COMM", comm) + aiff_chunk(b"SSND", sound)
    with open(path, "wb") as f:
        f.write(b"FORM" + struct.pack(">I", len(body)) + body)
    print(f"{os.path.basename(path):32s} {len(data):8d} bytes of data (AIFF{'-C' if aifc else ''})")


def pcm_be(samples, bits):
    if bits == 8:
        return bytes(max(0, min(255, int(s * 128) + 128)) for s in samples)
    if bits == 16:
        return b"".join(struct.pack(">h", max(-32768, min(32767, int(s * 32767)))) for s in samples)
    if bits == 24:
        out = bytearray()
        for s in samples:
            v = max(-8388608, min(8388607, int(s * 8388607))) & 0xFFFFFF
            out += struct.pack(">I", v)[1:]
        return bytes(out)
    if bits == 32:
        return b"".join(struct.pack(">i", max(-2**31, min(2**31 - 1, int(s * 2147483647)))) for s in samples)
    raise ValueError(bits)


def pcm_be8_signed(samples):
    """AIFF's 8-bit PCM is SIGNED, where WAV's is unsigned: the one place the
    two containers disagree, and a classic source of static."""
    return bytes(max(0, min(255, int(s * 127) & 0xFF)) for s in samples)


def float_be(samples, bits):
    fmt = ">f" if bits == 32 else ">d"
    return b"".join(struct.pack(fmt, s) for s in samples)


def adpcm_block(samples):
    """A minimal IMA ADPCM mono block: the probe only cares that the port
    recognises the codec and stays silent, not that it decodes it."""
    step_table = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
                  34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
                  130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
                  408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
                  1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
                  3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
                  8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
                  20350, 22385, 24623, 27086, 29794, 32767]
    out = bytearray()
    for i in range(0, len(samples), 8):
        blk = samples[i:i + 8]
        if len(blk) % 2:
            blk = blk + [0.0]
        pred = 0
        index = 0
        out += struct.pack("<hBB", 0, 0, 0)  # predictor, step index, reserved
        nibbles = 0
        count = 0
        for s in blk:
            target = int(max(-1, min(1, s)) * 32767)
            step = step_table[index]
            diff = target - pred
            code = 0
            if diff < 0:
                code = 8
                diff = -diff
            delta = step >> 3
            if diff >= step:
                code |= 4
                diff -= step
                delta += step
            if diff >= step >> 1:
                code |= 2
                diff -= step >> 1
                delta += step >> 1
            if diff >= step >> 2:
                code |= 1
                delta += step >> 2
            pred += -delta if code & 8 else delta
            pred = max(-32768, min(32767, pred))
            index += [0, 0, 0, 1, 2, 3, 4, 5, 6, 7][code & 7 if False else (code & 7)] if False else 0
            nibbles |= code << (4 * count)
            count += 1
            if count == 2:
                out.append(nibbles & 0xFF)
                nibbles = 0
                count = 0
        if count:
            out.append(nibbles & 0xFF)
    return bytes(out)


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    n = int(SR * SEC)
    j = lambda name: os.path.join(out_dir, name)

    write_wav(j("user_16bit_stereo.wav"), pcm(tone(440.0, n), 16))
    write_wav(j("user_24bit_stereo.wav"), pcm(tone(440.0, n), 24), bits=24)
    write_wav(j("user_32bit_stereo.wav"), pcm(tone(440.0, n), 32), bits=32)
    write_wav(j("user_float32_stereo.wav"), float_pcm(tone(440.0, n), 32), fmt=3, bits=32)
    write_wav(j("user_float64_stereo.wav"), float_pcm(tone(440.0, n), 64), fmt=3, bits=64)
    write_wav(j("user_8bit_stereo.wav"), pcm(tone(440.0, n), 8), bits=8)

    # 24-bit in a 32-bit extensible container (what a DAW writes for 24-bit)
    d32 = pcm(tone(440.0, n), 32)
    write_wav(j("user_extensible24_stereo.wav"), d32, fmt=0xFFFE, bits=32, valid_bits=24)

    info = chunk(b"INFO", chunk(b"ISFT", b"Audacity 3.4.2"))
    write_wav(j("user_listchunk_stereo.wav"), pcm(tone(440.0, n), 16), pre=chunk(b"LIST", info))
    write_wav(j("user_junkchunk_stereo.wav"), pcm(tone(440.0, n), 16),
              pre=chunk(b"junk", b"\x00" * 1000) + chunk(b"bext", b"x" * 601))

    write_wav(j("user_16bit_mono.wav"), pcm(tone(440.0, n, 1), 16), ch=1)
    write_wav(j("user_24bit_mono.wav"), pcm(tone(440.0, n, 1), 24), ch=1, bits=24)

    quad = tone(440.0, n, 1)
    quad = [s for s in quad for _ in range(4)]
    write_wav(j("user_quad_stereo.wav"), pcm(quad, 16), ch=4)

    write_wav(j("user_ulaw_stereo.wav"), bytes(ulaw_encode(s) for s in tone(440.0, n)),
              fmt=7, bits=8)
    write_wav(j("user_alaw_stereo.wav"), bytes(alaw_encode(s) for s in tone(440.0, n)),
              fmt=6, bits=8)

    write_wav(j("user_truncated_stereo.wav"), pcm(tone(440.0, n), 24), bits=24,
              declared_len=36 + 12 + n * 6 * 4)  # lies: claims 4x the data

    write_wav(j("user_adpcm.wav"), adpcm_block(tone(440.0, n, 1)), fmt=0x11, ch=1, bits=4)

    with open(j("user_notawav.wav"), "wb") as f:
        f.write(bytes((i * 37 + 11) & 0xFF for i in range(60000)))

    # --- AIFF / AIFF-C, which is what a Mac's library is full of -----------
    write_aiff(j("user_16bit_stereo.aiff"), pcm_be(tone(440.0, n), 16))
    write_aiff(j("user_24bit_stereo.aiff"), pcm_be(tone(440.0, n), 24), bits=24)
    write_aiff(j("user_8bit_stereo.aiff"), pcm_be8_signed(tone(440.0, n)), bits=8)
    write_aiff(j("user_16bit_mono.aiff"), pcm_be(tone(440.0, n, 1), 16), ch=1)
    write_aiff(j("user_sowt16_stereo.aifc"), pcm(tone(440.0, n), 16), aifc=True,
               compression=b"sowt")
    write_aiff(j("user_fl32_stereo.aifc"), float_be(tone(440.0, n), 32), bits=32, aifc=True,
               compression=b"fl32")
    write_aiff(j("user_ulaw_stereo.aifc"),
               bytes(ulaw_encode(s) for s in tone(440.0, n)), bits=8, aifc=True,
               compression=b"ulaw")
    write_aiff(j("twotone_16bit_stereo.aiff"), pcm_be(twotone(n), 16))

    write_wav(j("ramp_16bit_stereo.wav"), pcm(ramp(n), 16))
    write_wav(j("ramp_24bit_stereo.wav"), pcm(ramp(n), 24), bits=24)
    write_wav(j("twotone_24bit_stereo.wav"), pcm(twotone(n), 24), bits=24)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/wavfmt/samples")
