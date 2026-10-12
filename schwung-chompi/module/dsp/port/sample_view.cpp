#include "sample_view.h"

#include "chompi_port.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace chompi
{

/* ---------------------------------------------------------------- helpers */

static inline uint16_t rd16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

static inline uint16_t rd16be(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t rd32be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)
           | (uint32_t)p[3];
}

/** An 80-bit IEEE extended float, as AIFF's COMM chunk stores the rate. Only
 *  the integer part is used (to rebuild a header nobody reads), so this is
 *  deliberately simple. */
static double ext80_to_double(const uint8_t* p)
{
    const int      exp  = (int)(((uint32_t)(p[0] & 0x7F) << 8) | p[1]);
    const uint32_t mant = rd32be(p + 2);
    if(exp == 0 && mant == 0)
        return 0.0;
    const double f = ldexp((double)mant, exp - 16383 - 31);
    return (p[0] & 0x80) ? -f : f;
}

static bool read_exact(int fd, void* dst, size_t n, int64_t off)
{
    uint8_t* p   = (uint8_t*)dst;
    size_t   got = 0;
    while(got < n)
    {
        const ssize_t r = pread(fd, p + got, n - got, (off_t)(off + (int64_t)got));
        if(r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

/* ------------------------------------------------------------ the format */

enum SrcKind
{
    kPcm = 0,
    kFloat,
    kALaw,
    kULaw,
    kSilent, /* parsed, but a codec we cannot decode: play nothing */
};

struct SampleView
{
    int     fd        = -1;
    int64_t src_off   = 0; /* first byte of the audio */
    int64_t src_len   = 0; /* bytes of audio */
    int     channels  = 2;
    int     bits      = 16; /* container bits per sample */
    int     valid_bits = 16; /* EXTENSIBLE: significant bits */
    int     bytes_per_sample = 2;
    int     frame_stride     = 4; /* bytes per source frame */
    int     kind = kPcm;
    int     format_code = 0; /* the format tag / AIFC compression, as written */
    bool    big_endian  = false; /* AIFF (and AIFC NONE/fl32) store MSB first */
    bool    signed8     = false; /* AIFF's 8-bit PCM is signed, WAV's is not */
    int64_t frames       = 0;
    int64_t virtual_size = 0;
    uint8_t header[44];
    /* One block of source bytes. 16 bytes is the widest frame there is
     * (64-bit float, stereo); sizing this for 8 was exactly the kind of
     * off-by-a-format that makes a file silent. */
    uint8_t scratch[16 * 512];
    char    describe[96];
};

static const char* kind_name(int kind, int bits)
{
    switch(kind)
    {
        case kFloat: return "float";
        case kALaw: return "A-law";
        case kULaw: return "u-law";
        case kSilent: return "unsupported";
        default: return bits == 8 ? "8-bit PCM" : (bits == 16 ? "16-bit PCM" : "PCM");
    }
}

/* Walks the RIFF chunks and returns what the engine needs to know. Only the
 * headers are read, so a file with a huge LIST/bext chunk costs a few preads
 * and no memory. */
struct Chunks
{
    int      format_code = 0;
    int      channels    = 0;
    int      bits        = 0;
    int      valid_bits  = 0;
    uint32_t rate        = 0;
    uint16_t block_align = 0;
    int64_t  data_off    = 0;
    int64_t  data_len    = 0;
    bool     have_fmt    = false;
    bool     have_data   = false;
    /* AIFF stores its samples big-endian, and its 8-bit PCM is signed where
     * WAV's is unsigned. Both flags exist so one decoder handles either. */
    bool big_endian = false;
    bool signed8    = false;
    bool is_aiff    = false;
};

static bool parse_chunks(int fd, int64_t file_size, Chunks& c)
{
    uint8_t hdr[12];
    if(file_size < 44 || !read_exact(fd, hdr, sizeof hdr, 0))
        return false;
    if(memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;

    int64_t off = 12;
    while(off + 8 <= file_size)
    {
        uint8_t        ck[8];
        if(!read_exact(fd, ck, sizeof ck, off))
            break;
        const uint32_t sz   = rd32(ck + 4);
        const int64_t  body = off + 8;

        if(memcmp(ck, "fmt ", 4) == 0 && sz >= 16 && body + 16 <= file_size)
        {
            uint8_t f[40];
            const size_t want = (sz < sizeof f) ? (size_t)sz : sizeof f;
            if(read_exact(fd, f, want, body))
            {
                c.format_code = rd16(f + 0);
                c.channels    = rd16(f + 2);
                c.rate        = rd32(f + 4);
                c.block_align = rd16(f + 12);
                c.bits        = rd16(f + 14);
                c.valid_bits  = c.bits;
                /* WAVE_FORMAT_EXTENSIBLE: the real format is the first two
                 * bytes of the subformat GUID, and the real bit depth is
                 * `wValidBitsPerSample`. DAWs write 24-in-32 and float32 this
                 * way; without this they all decode to noise. */
                if(c.format_code == 0xFFFE && sz >= 26)
                {
                    c.valid_bits  = rd16(f + 18);
                    c.format_code = rd16(f + 24);
                    if(c.valid_bits == 0 || c.valid_bits > c.bits)
                        c.valid_bits = c.bits;
                }
                c.have_fmt = true;
            }
        }
        else if(memcmp(ck, "data", 4) == 0)
        {
            c.data_off  = body;
            int64_t len = (int64_t)(int32_t)sz;
            /* A streaming WAV writes 0 or 0xFFFFFFFF here, and a truncated
             * download can claim more than is there: trust the file size. */
            if(len <= 0 || len > file_size - body)
                len = file_size - body;
            c.data_len  = len;
            c.have_data = true;
            break;
        }
        else if(memcmp(ck, "RIFF", 4) == 0 || memcmp(ck, "RIFX", 4) == 0)
        {
            /* A nested RIFF (a wav inside a container chunk): not ours. */
            return false;
        }

        if(sz == 0xFFFFFFFFu)
            break; /* malformed: stop instead of walking forever */
        off = body + (int64_t)sz + (int64_t)(sz & 1u);
    }
    return c.have_fmt && c.have_data;
}

/* AIFF / AIFF-C: FORM/AIFF, a COMM chunk with channels and the sample format,
 * and an SSND chunk holding the samples. macOS writes these, so a user's
 * library usually has some. `sowt` (twos spelled backwards) is little-endian
 * PCM, `fl32`/`fl64` are big-endian floats, and NONE is big-endian PCM. */
static bool parse_aiff(int fd, int64_t file_size, Chunks& c)
{
    uint8_t hdr[12];
    if(file_size < 40 || !read_exact(fd, hdr, sizeof hdr, 0))
        return false;
    if(memcmp(hdr, "FORM", 4) != 0
       || (memcmp(hdr + 8, "AIFF", 4) != 0 && memcmp(hdr + 8, "AIFC", 4) != 0))
        return false;

    const bool aifc = (memcmp(hdr + 8, "AIFC", 4) == 0);
    c.is_aiff = true;

    int32_t declared_frames = 0;
    int64_t ssnd_body        = 0, ssnd_size = 0;

    int64_t off = 12;
    while(off + 8 <= file_size)
    {
        uint8_t        ck[8];
        if(!read_exact(fd, ck, sizeof ck, off))
            break;
        const uint32_t sz   = rd32be(ck + 4);
        const int64_t  body = off + 8;

        if(memcmp(ck, "COMM", 4) == 0 && sz >= 8 && body + 8 <= file_size)
        {
            uint8_t f[32];
            const size_t want = (sz < sizeof f) ? (size_t)sz : sizeof f;
            if(read_exact(fd, f, want, body))
            {
                c.channels       = rd16be(f + 0);
                declared_frames  = (int32_t)rd32be(f + 2);
                c.bits           = rd16be(f + 6);
                c.rate           = 48000u;
                if(want >= 18) /* the 80-bit rate is optional in practice */
                {
                    const double rate = ext80_to_double(f + 8);
                    c.rate = (rate > 1.0 && rate < 1e7) ? (uint32_t)(rate + 0.5) : 48000u;
                }
                c.valid_bits      = c.bits;
                c.format_code     = 1; /* PCM unless AIFC says otherwise */
                c.big_endian      = true;
                c.signed8         = true; /* AIFF's 8-bit PCM is signed */
                if(aifc && want >= 22 && sz >= 22)
                {
                    const uint8_t* t = f + 18; /* compressionType 4CC */
                    if(memcmp(t, "sowt", 4) == 0)
                        c.big_endian = false;
                    else if(memcmp(t, "fl32", 4) == 0 || memcmp(t, "FL32", 4) == 0)
                        c.format_code = 3;
                    else if(memcmp(t, "fl64", 4) == 0 || memcmp(t, "FL64", 4) == 0)
                    {
                        c.format_code = 3;
                        c.bits        = 64;
                        c.valid_bits  = 64;
                    }
                    else if(memcmp(t, "ulaw", 4) == 0 || memcmp(t, "ULAW", 4) == 0)
                        c.format_code = 7;
                    else if(memcmp(t, "alaw", 4) == 0 || memcmp(t, "ALAW", 4) == 0)
                        c.format_code = 6;
                    else if(memcmp(t, "raw ", 4) == 0)
                    {
                        c.big_endian = false;
                        c.signed8    = false;
                    }
                    else if(memcmp(t, "NONE", 4) != 0)
                        c.format_code = 0x100 + (int)t[0]; /* unknown: silence */
                }
                c.have_fmt = true;
            }
        }
        else if(memcmp(ck, "SSND", 4) == 0 && body + 8 <= file_size)
        {
            uint8_t o[8];
            if(read_exact(fd, o, sizeof o, body))
            {
                const uint32_t offset  = rd32be(o + 0); /* bytes before the audio */
                const int64_t  data    = body + 8 + (int64_t)offset;
                int64_t        len     = (int64_t)(int32_t)sz - 8 - (int64_t)offset;
                if(len <= 0 || data + len > file_size)
                    len = file_size - data;
                if(len > 0)
                {
                    c.data_off  = data;
                    c.data_len  = len;
                    c.have_data = true;
                    ssnd_body   = body;
                    ssnd_size   = sz;
                }
            }
        }

        if(sz == 0xFFFFFFFFu)
            break;
        off = body + (int64_t)sz + (sz & 1);
    }

    /* COMM's numSampleFrames is authoritative when it is sane: SSND can carry
     * padding, and some writers round the chunk size up. */
    if(c.have_data && c.channels > 0 && c.bits > 0 && declared_frames > 0)
    {
        const int64_t frame_bytes = (int64_t)c.channels * (c.bits / 8);
        if(frame_bytes > 0)
        {
            const int64_t by_frames = (int64_t)declared_frames * frame_bytes;
            if(by_frames > 0 && by_frames <= c.data_len)
                c.data_len = by_frames;
        }
    }
    (void)ssnd_body;
    (void)ssnd_size;
    return c.have_fmt && c.have_data;
}

static void put_header(uint8_t* h, uint32_t rate, int64_t frames)
{
    const uint32_t data = (uint32_t)(frames * 4);
    memcpy(h, "RIFF", 4);
    const uint32_t riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fsz = 16;
    memcpy(h + 16, &fsz, 4);
    const uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
    const uint32_t br = rate * 4;
    memcpy(h + 20, &fmt, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
}

/* --------------------------------------------------------- sample decode */

/* G.711 u-law, the classic 8-bit telephony codec some libraries still hold. */
static int16_t ulaw_to_s16(uint8_t u)
{
    u = (uint8_t)~u;
    int t = ((int)(u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

/* G.711 A-law. */
static int16_t alaw_to_s16(uint8_t a)
{
    a = (uint8_t)(a ^ 0x55);
    int t   = ((int)(a & 0x0F) << 4);
    int seg = (a & 0x70) >> 4;
    switch(seg)
    {
        case 0: t += 8; break;
        case 1: t += 0x108; break;
        default: t += 0x108; t <<= seg - 1; break;
    }
    return (int16_t)((a & 0x80) ? t : -t);
}

static inline int16_t clamp16(int32_t v)
{
    if(v > 32767) return 32767;
    if(v < -32768) return -32768;
    return (int16_t)v;
}

/* One source sample, already decoded to the int16 the engine speaks. The
 * scaling mirrors libDaisy's s162f/f2s16 pair so a converted file measures the
 * same level as the same file in CHOMPI's own card format. */
static int16_t decode_sample(const SampleView* v, const uint8_t* p)
{
    switch(v->kind)
    {
        case kPcm:
            if(v->big_endian)
            {
                switch(v->bytes_per_sample)
                {
                    case 1: return v->signed8 ? clamp16((int)(int8_t)p[0] << 8)
                                              : clamp16(((int)p[0] - 128) << 8);
                    case 2: return (int16_t)rd16be(p);
                    case 3:
                    {
                        const int32_t x = (int32_t)((uint32_t)p[0] << 24
                                                    | (uint32_t)p[1] << 16
                                                    | (uint32_t)p[2] << 8);
                        return clamp16(x >> 16);
                    }
                    default:
                    {
                        const int32_t x = (int32_t)rd32be(p);
                        return clamp16(x >> 16);
                    }
                }
            }
            switch(v->bytes_per_sample)
            {
                case 1: return clamp16(((int)p[0] - 128) << 8); /* PCM in WAV is unsigned */
                case 2: return (int16_t)rd16(p);
                case 3:
                {
                    /* sign-extend the 24-bit value, then keep the top 16 bits */
                    const int32_t x = (int32_t)((uint32_t)p[2] << 24 | (uint32_t)p[1] << 16
                                                | (uint32_t)p[0] << 8);
                    return clamp16(x >> 16);
                }
                default:
                {
                    const int32_t x = (int32_t)rd32(p);
                    return clamp16(x >> 16);
                }
            }
        case kFloat:
            if(v->big_endian)
            {
                /* AIFF-C fl32/fl64 are big-endian: swap into host order. */
                uint8_t t[8];
                if(v->bytes_per_sample == 8)
                {
                    for(int i = 0; i < 8; i++)
                        t[i] = p[7 - i];
                    double d;
                    memcpy(&d, t, sizeof d);
                    if(!(d > -1.0))
                        return d < -1.0 ? -32767 : 0;
                    return clamp16((int32_t)lround(d > 1.0 ? 32767.0 : d * 32767.0));
                }
                for(int i = 0; i < 4; i++)
                    t[i] = p[3 - i];
                float f;
                memcpy(&f, t, sizeof f);
                if(!(f > -1.f))
                    return f < -1.f ? -32767 : 0;
                return clamp16((int32_t)lroundf(f > 1.f ? 32767.f : f * 32767.f));
            }
            if(v->bytes_per_sample == 8)
            {
                double d;
                memcpy(&d, p, sizeof d);
                if(!(d > -1.0)) /* also catches NaN */
                    return d < -1.0 ? -32767 : 0;
                if(d > 1.0) return 32767;
                return (int16_t)(d * 32767.0);
            }
            else
            {
                float f;
                memcpy(&f, p, sizeof f);
                if(!(f > -1.f)) /* also catches NaN */
                    return f < -1.f ? -32767 : 0;
                if(f > 1.f) return 32767;
                return (int16_t)(f * 32767.f);
            }
        case kALaw: return alaw_to_s16(p[0]);
        case kULaw: return ulaw_to_s16(p[0]);
        default: return 0;
    }
}

/* ------------------------------------------------------------ open / read */

static const int kBlockFrames = 512;

SampleView* SampleViewOpen(int fd, int64_t file_size, const char* path)
{
    Chunks c;
    if(!parse_chunks(fd, file_size, c) && !parse_aiff(fd, file_size, c))
        return nullptr; /* not a container we understand: leave it alone */

    /* Some writers leave `bits` at zero and put the truth in `blockAlign`.
     * Deriving it here is the difference between "your sample plays" and
     * "your sample is silent because a field was empty". */
    if(c.bits == 0 && c.block_align > 0 && c.channels > 0)
    {
        const int derived = (int)c.block_align / c.channels * 8;
        if(derived > 0 && derived <= 64)
        {
            c.bits       = derived;
            c.valid_bits = derived;
        }
    }

    const int container_bytes = c.bits / 8;
    int       kind            = kSilent;
    if(c.format_code == 1)
        kind = kPcm;
    else if(c.format_code == 3)
        kind = kFloat;
    else if(c.format_code == 6)
        kind = kALaw;
    else if(c.format_code == 7)
        kind = kULaw;

    const bool bits_ok
        = (kind == kPcm && (c.bits == 8 || c.bits == 16 || c.bits == 24 || c.bits == 32))
          || (kind == kFloat && (c.bits == 32 || c.bits == 64))
          || ((kind == kALaw || kind == kULaw) && c.bits == 8);

    if(c.channels <= 0 || c.channels > 32 || !bits_ok)
        kind = kSilent; /* parsed, but not decodable: silence, not noise */

    int       bps   = (kind == kSilent) ? (c.bits / 8 > 0 ? c.bits / 8 : 2) : container_bytes;
    const int stride = (c.channels > 0 ? c.channels : 1) * bps;

    /* Native — what the engine was written for — is left exactly as it is, so
     * the shipped card and the recorder's own files keep their raw path. */
    if(kind == kPcm && c.bits == 16 && c.valid_bits == 16 && c.channels == 2
       && c.data_off == 44 && c.data_len == file_size - 44 && !c.big_endian
       && !c.signed8)
        return nullptr;

    SampleView* v = new SampleView();
    v->fd              = fd;
    v->src_off         = c.data_off;
    v->src_len         = c.data_len;
    v->channels        = c.channels > 0 ? c.channels : 1;
    v->bits            = c.bits > 0 ? c.bits : bps * 8;
    v->valid_bits      = c.valid_bits > 0 ? c.valid_bits : v->bits;
    v->bytes_per_sample = bps;
    v->frame_stride     = stride;
    v->kind             = kind;
    v->big_endian       = c.big_endian;
    v->signed8          = c.signed8;
    v->format_code      = c.format_code;
    v->frames           = (stride > 0) ? (c.data_len / stride) : 0;
    v->virtual_size     = 44 + v->frames * 4;
    put_header(v->header, c.rate > 0 ? c.rate : 48000u, v->frames);

    if(kind == kSilent)
        snprintf(v->describe, sizeof v->describe,
                 "format=%d bits=%d ch=%d -> silence (not decodable)",
                 c.format_code, c.bits, c.channels);
    else
        snprintf(v->describe, sizeof v->describe, "%s %d-bit %s %s -> 16-bit stereo, %lld frames",
                 c.is_aiff ? "AIFF" : "WAV", c.bits, v->channels == 1 ? "mono" : "stereo",
                 kind_name(kind, c.bits), (long long)v->frames);

    g_sample_view_count++;
    snprintf(g_last_sample_view, sizeof g_last_sample_view, "%s: %s",
             path ? path : "?", v->describe);
    fprintf(stderr, "chompi: sample view for %s: %s\n", path ? path : "?", v->describe);
    return v;
}

int64_t SampleViewSize(const SampleView* v)
{
    return v ? v->virtual_size : 0;
}

const char* SampleViewDescribe(const SampleView* v)
{
    return v ? v->describe : "";
}

size_t SampleViewRead(SampleView* v, int64_t vpos, void* dst, size_t n)
{
    uint8_t* out  = (uint8_t*)dst;
    size_t   done = 0;

    while(done < n)
    {
        const int64_t pos = vpos + (int64_t)done;

        /* The synthesized header. The engine starts reading at byte 44, but a
         * seek to 0 (or a short read at the very start) lands here. */
        if(pos < 44)
        {
            size_t take = (size_t)(44 - pos);
            if(take > n - done)
                take = n - done;
            memcpy(out + done, v->header + pos, take);
            done += take;
            continue;
        }

        const int64_t dpos  = pos - 44;
        const int64_t frame = dpos / 4;
        const int64_t infr  = dpos % 4;

        if(frame >= v->frames)
            break; /* EOF */

        int64_t nframes = ((int64_t)(n - done) + infr + 3) / 4;
        if(nframes > kBlockFrames)
            nframes = kBlockFrames;
        if(frame + nframes > v->frames)
            nframes = v->frames - frame;

        const size_t sbytes = (size_t)(nframes * v->frame_stride);
        if(sbytes > sizeof v->scratch
           || !read_exact(v->fd, v->scratch, sbytes, v->src_off + frame * (int64_t)v->frame_stride))
            break;

        int16_t stage[kBlockFrames * 2];
        for(int64_t f = 0; f < nframes; f++)
        {
            const uint8_t* p = v->scratch + (size_t)f * (size_t)v->frame_stride;
            const int16_t  l = decode_sample(v, p);
            const int16_t  r = (v->channels > 1) ? decode_sample(v, p + v->bytes_per_sample) : l;
            stage[2 * f]     = l;
            stage[2 * f + 1] = r;
        }

        size_t take = (size_t)(4 * nframes - infr);
        if(take > n - done)
            take = n - done;
        memcpy(out + done, (const uint8_t*)stage + infr, take);
        done += take;
    }
    return done;
}

void SampleViewFree(SampleView* v)
{
    delete v;
}

} // namespace chompi
