#include "wav_loader.h"
#include "../Debug/logger.h"
#include "../buildconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

static int16_t wav_clip16(float v) {
    if (v >= 1.0f) return 32767;
    if (v <= -1.0f) return -32768;
    return (int16_t)lrintf(v * 32767.0f);
}

static uint32_t rd_u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const unsigned char* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Takes ownership of pcm (interleaved int16, malloc'd). Keeps at most two
 * channels, which is what the voice engine plays. */
static wav_data* wav_from_s16(short* pcm, size_t frames, int channels, int sample_rate) {
    if (!pcm || frames == 0 || channels <= 0 || sample_rate <= 0 || frames > (size_t)INT_MAX) {
        free(pcm);
        return NULL;
    }
    if (channels > 2) {
        for (size_t i = 0; i < frames; ++i) {
            pcm[i * 2] = pcm[i * (size_t)channels];
            pcm[i * 2 + 1] = pcm[i * (size_t)channels + 1];
        }
        channels = 2;
    }
    wav_data* data = (wav_data*)calloc(1, sizeof(wav_data));
    if (!data) {
        free(pcm);
        return NULL;
    }
    data->data = pcm;
    data->num_samples = (int)frames;
    data->num_channels = channels;
    data->sample_rate = sample_rate;
    data->root_key = -1;
    return data;
}

static short* wav_convert(const unsigned char* raw, size_t frames, int channels, int format, int bits) {
    size_t count = frames * (size_t)channels;
    short* out = (short*)malloc((count ? count : 1) * sizeof(short));
    if (!out) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (format == 3 && bits == 32) {
            float v;
            memcpy(&v, raw + i * 4, 4);
            out[i] = wav_clip16(v);
        } else if (format == 3 && bits == 64) {
            double v;
            memcpy(&v, raw + i * 8, 8);
            out[i] = wav_clip16((float)v);
        } else if (bits == 8) {
            out[i] = (short)(((int)raw[i] - 128) * 256);
        } else if (bits == 16) {
            out[i] = (short)rd_u16(raw + i * 2);
        } else if (bits == 24) {
            const unsigned char* p = raw + i * 3;
            out[i] = (short)(p[1] | (p[2] << 8));
        } else {
            const unsigned char* p = raw + i * 4;
            out[i] = (short)(p[2] | (p[3] << 8));
        }
    }
    return out;
}

wav_data* wav_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        logger_log("Failed to open WAV file: %s\n", path);
        return NULL;
    }

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        logger_log("Not a WAVE file: %s\n", path);
        fclose(f);
        return NULL;
    }

    int format = 0, channels = 0, bits = 0, rate = 0;
    int root_key = -1, has_loop = 0, loop_start = 0, loop_end = 0;
    float root_fraction = 0.0f;
    unsigned char* raw = NULL;
    size_t raw_size = 0;
    unsigned char ch[8];

    while (fread(ch, 1, 8, f) == 8) {
        uint32_t size = rd_u32(ch + 4);
        long next = ftell(f) + (long)size + (long)(size & 1u);
        if (memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            unsigned char fmt[40];
            size_t take = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, take, f) != take) break;
            format = rd_u16(fmt);
            channels = rd_u16(fmt + 2);
            rate = (int)rd_u32(fmt + 4);
            bits = rd_u16(fmt + 14);
            if (format == 0xFFFE && take >= 26) format = rd_u16(fmt + 24);
        } else if (memcmp(ch, "data", 4) == 0 && !raw) {
            raw = (unsigned char*)malloc(size ? size : 1);
            if (!raw) break;
            raw_size = fread(raw, 1, size, f);
        } else if (memcmp(ch, "smpl", 4) == 0 && size >= 36) {
            unsigned char smpl[60];
            size_t take = size < sizeof(smpl) ? size : sizeof(smpl);
            if (fread(smpl, 1, take, f) == take) {
                uint32_t unity = rd_u32(smpl + 12);
                if (unity <= 127u) root_key = (int)unity;
                root_fraction = (float)((double)rd_u32(smpl + 16) / 4294967296.0);
                if (rd_u32(smpl + 28) > 0 && take >= 60) {
                    has_loop = 1;
                    loop_start = (int)rd_u32(smpl + 44);
                    loop_end = (int)rd_u32(smpl + 48);
                }
            }
        }
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    fclose(f);

    int supported = channels > 0 && rate > 0 &&
        ((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
         (format == 3 && (bits == 32 || bits == 64)));
    if (!raw || !supported) {
        logger_log("Unsupported or incomplete WAV file: %s\n", path);
        free(raw);
        return NULL;
    }

    size_t frames = raw_size / ((size_t)channels * (size_t)(bits / 8));
    short* pcm = wav_convert(raw, frames, channels, format, bits);
    free(raw);
    wav_data* data = wav_from_s16(pcm, frames, channels, rate);
    if (data) {
        data->root_key = root_key;
        data->root_key_fraction = root_fraction;
        data->has_loop = has_loop;
        data->loop_start = loop_start;
        data->loop_end = loop_end;
    }
    return data;
}

wav_data* wav_make_waveform(const char* name, int sample_rate) {
    const int len = 2048;
    int looped = 1;
    if (_stricmp(name, "*silence") == 0) looped = 0;
    else if (_stricmp(name, "*sine") != 0 && _stricmp(name, "*saw") != 0 &&
             _stricmp(name, "*square") != 0 && _stricmp(name, "*triangle") != 0 &&
             _stricmp(name, "*tri") != 0 && _stricmp(name, "*noise") != 0)
        return NULL;
    short* pcm = (short*)malloc((size_t)len * sizeof(short));
    if (!pcm) return NULL;
    uint32_t seed = 0x9E3779B9u;
    for (int i = 0; i < len; ++i) {
        float t = (float)i / (float)len, v = 0.0f;
        if (!looped) v = 0.0f;
        else if (_stricmp(name, "*sine") == 0) v = sinf(6.28318530718f * t);
        else if (_stricmp(name, "*saw") == 0) v = 2.0f * t - 1.0f;
        else if (_stricmp(name, "*square") == 0) v = t < 0.5f ? 1.0f : -1.0f;
        else if (_stricmp(name, "*noise") == 0) {
            seed = seed * 1664525u + 1013904223u;
            v = (float)(seed >> 8) / 8388608.0f - 1.0f;
        } else v = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
        pcm[i] = wav_clip16(v * 0.5f);
    }
    if (sample_rate <= 0) sample_rate = 44100;
    wav_data* data = wav_from_s16(pcm, (size_t)len, 1, looped ? (int)lrintf(261.6256f * (float)len) : sample_rate);
    if (data && looped) {
        data->root_key = 60;
        data->has_loop = 1;
        data->loop_start = 0;
        data->loop_end = len;
    }
    return data;
}

void wav_free(wav_data* data) {
    if (data) {
        if (data->data) {
            free(data->data);
        }
        free(data);
    }
}
