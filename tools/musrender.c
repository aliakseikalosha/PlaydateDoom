// Build-time music renderer: scans WADs for music lumps (MUS or MIDI), renders
// each one with a small built-in software synth and writes <out>/<hash>.wav
// (mono 16-bit). pdc then converts the WAVs to .pda, which the game streams
// with a FilePlayer. <hash> is FNV-1a-32 of the raw lump bytes; the game
// computes the same hash over the data it is given in RegisterSong.
//
// usage: musrender <wad-dir> <out-dir>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define RATE 11025
#define MAX_VOICES 48
#define MAX_SECONDS 600

typedef struct
{
    uint32_t t;      // time in samples
    uint8_t type;    // 0 note off, 1 note on, 2 program, 3 volume, 4 pan(unused), 5 pitch bend, 6 all off, 7 expression
    uint8_t ch, a, b;
    int v;
    int seq;
} ev_t;

typedef struct
{
    ev_t *e;
    int n, cap;
} evlist_t;

static void add_ev(evlist_t *l, uint32_t t, int type, int ch, int a, int b, int v)
{
    if (l->n == l->cap)
    {
        l->cap = l->cap ? l->cap * 2 : 1024;
        l->e = realloc(l->e, l->cap * sizeof(ev_t));
    }
    l->e[l->n] = (ev_t){t, type, ch, a, b, v, l->n};
    l->n++;
}

uint32_t fnv(const uint8_t *d, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *d++) * 16777619u;
    return h;
}

// ---- MUS parsing (140 ticks/s) ----
static int parse_mus(const uint8_t *d, size_t n, evlist_t *l)
{
    static const int ctrl_map[] = {-1, -1, -1, 3, -1, -1, -1, -1, -1, -1};
    uint16_t len, off;
    size_t p;
    uint32_t tick = 0;
    int vel[16] = {0};
    int i;

    for (i = 0; i < 16; ++i)
        vel[i] = 127;
    if (n < 16)
        return 0;
    len = d[4] | (d[5] << 8);
    off = d[6] | (d[7] << 8);
    p = off;
    (void)len;
    (void)ctrl_map;

    while (p < n)
    {
        int last, type, ch, b1, b2;
        uint8_t ev = d[p++];
        uint32_t t = (uint32_t)((uint64_t)tick * RATE / 140);

        last = ev & 0x80;
        type = (ev >> 4) & 7;
        ch = ev & 15;
        // MUS channel 15 is percussion; MIDI maps that to 9, and 9 to 15.
        if (ch == 15)
            ch = 9;
        else if (ch == 9)
            ch = 15;

        switch (type)
        {
        case 0: // release
            if (p >= n) return 1;
            add_ev(l, t, 0, ch, d[p++] & 127, 0, 0);
            break;
        case 1: // play
            if (p >= n) return 1;
            b1 = d[p++];
            if (b1 & 0x80)
            {
                if (p >= n) return 1;
                vel[ch] = d[p++] & 127;
            }
            add_ev(l, t, 1, ch, b1 & 127, vel[ch], 0);
            break;
        case 2: // pitch wheel (0..255, 128 centre)
            if (p >= n) return 1;
            add_ev(l, t, 5, ch, 0, 0, ((int)d[p++] - 128) * 64);
            break;
        case 3: // system event
            if (p >= n) return 1;
            b1 = d[p++];
            if (b1 == 10 || b1 == 11)
                add_ev(l, t, 6, ch, 0, 0, 0);
            break;
        case 4: // controller
            if (p + 1 >= n) return 1;
            b1 = d[p++];
            b2 = d[p++];
            if (b1 == 0)
                add_ev(l, t, 2, ch, b2 & 127, 0, 0);
            else if (b1 == 3)
                add_ev(l, t, 3, ch, b2 & 127, 0, 0);
            else if (b1 == 4) // MUS pan (unused: output is mono)
                ;
            break;
        case 6: // score end
            add_ev(l, t, 6, 0, 0, 0, 1); // v=1 marks end of score
            return 0;
        default:
            if (type == 5) // end of measure
                ;
            else
                p++;
            break;
        }

        if (last)
        {
            uint32_t delay = 0;
            uint8_t b;
            do
            {
                if (p >= n) return 0;
                b = d[p++];
                delay = (delay << 7) | (b & 127);
            } while (b & 0x80);
            tick += delay;
        }
    }
    return 0;
}

// ---- MIDI parsing (format 0/1, tempo aware) ----
static uint32_t rd32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

typedef struct { uint32_t tick; int idx; } tmp_t;

static int cmp_ev(const void *a, const void *b)
{
    const ev_t *x = a, *y = b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    return x->seq - y->seq;
}

static int parse_midi(const uint8_t *d, size_t n, evlist_t *l)
{
    int ntracks, div, tr;
    size_t p = 14;
    // tempo map
    uint32_t tm_tick[1024]; uint32_t tm_us[1024]; int ntm = 0;
    evlist_t raw = {0}; // t holds ticks for now
    uint32_t end_tick = 0;
    int i;

    if (n < 14 || memcmp(d, "MThd", 4)) return 1;
    ntracks = (d[10] << 8) | d[11];
    div = (d[12] << 8) | d[13];
    if (div & 0x8000 || div == 0) return 1;

    for (tr = 0; tr < ntracks && p + 8 <= n; ++tr)
    {
        uint32_t tl = rd32(d + p + 4);
        size_t q = p + 8, e = q + tl;
        uint32_t tick = 0;
        uint8_t running = 0;

        if (e > n) e = n;
        p = e;
        while (q < e)
        {
            uint32_t delta = 0;
            uint8_t b, st;
            do { b = d[q++]; delta = (delta << 7) | (b & 127); } while ((b & 0x80) && q < e);
            tick += delta;
            if (q >= e) break;
            st = d[q];
            if (st & 0x80) { q++; if (st < 0xf0) running = st; } else st = running;
            if (st == 0xff)
            {
                uint8_t type = d[q++];
                uint32_t len = 0;
                do { b = d[q++]; len = (len << 7) | (b & 127); } while ((b & 0x80) && q < e);
                if (type == 0x51 && len == 3 && ntm < 1024)
                {
                    tm_tick[ntm] = tick;
                    tm_us[ntm++] = (d[q] << 16) | (d[q + 1] << 8) | d[q + 2];
                }
                q += len;
            }
            else if (st == 0xf0 || st == 0xf7)
            {
                uint32_t len = 0;
                do { b = d[q++]; len = (len << 7) | (b & 127); } while ((b & 0x80) && q < e);
                q += len;
            }
            else
            {
                int hi = st & 0xf0, ch = st & 15;
                int a = q < e ? d[q] & 127 : 0, bb = (q + 1 < e) ? d[q + 1] & 127 : 0;
                if (hi == 0xc0 || hi == 0xd0) q += 1; else q += 2;
                if (hi == 0x80 || (hi == 0x90 && bb == 0)) add_ev(&raw, tick, 0, ch, a, 0, 0);
                else if (hi == 0x90) add_ev(&raw, tick, 1, ch, a, bb, 0);
                else if (hi == 0xc0) add_ev(&raw, tick, 2, ch, a, 0, 0);
                else if (hi == 0xb0 && a == 7) add_ev(&raw, tick, 3, ch, bb, 0, 0);
                else if (hi == 0xb0 && a == 123) add_ev(&raw, tick, 6, ch, 0, 0, 0);
                else if (hi == 0xe0)
                {
                    int bend = ((bb << 7) | a) - 8192;
                    add_ev(&raw, tick, 5, ch, 0, 0, bend);
                }
            }
        }
        if (tick > end_tick) end_tick = tick;
    }

    // ticks -> samples via tempo map
    {
        // sort tempo map by tick
        for (i = 1; i < ntm; ++i)
            for (int j = i; j > 0 && tm_tick[j] < tm_tick[j - 1]; --j)
            {
                uint32_t t1 = tm_tick[j]; tm_tick[j] = tm_tick[j - 1]; tm_tick[j - 1] = t1;
                t1 = tm_us[j]; tm_us[j] = tm_us[j - 1]; tm_us[j - 1] = t1;
            }
        for (i = 0; i < raw.n; ++i)
        {
            uint32_t tk = raw.e[i].t;
            double sec = 0, us = 500000;
            uint32_t last = 0;
            for (int k = 0; k < ntm && tm_tick[k] < tk; ++k)
            {
                sec += (double)(tm_tick[k] - last) * us / 1e6 / div;
                last = tm_tick[k];
                us = tm_us[k];
            }
            sec += (double)(tk - last) * us / 1e6 / div;
            raw.e[i].t = (uint32_t)(sec * RATE);
        }
    }
    qsort(raw.e, raw.n, sizeof(ev_t), cmp_ev); // not stable, but only same-time order is affected
    *l = raw;
    return 0;
}

// ---- synth ----
typedef struct
{
    int active, ch, note, released;
    double phase, freq, vel;
    double env;       // 0..1
    int prog;
    uint32_t age;
    double lp;        // lowpass state / noise state
    uint32_t rng;
} voice_t;

static double ch_vol[16], ch_bend[16];
static int ch_prog[16];

static double sawf(double p) { return 2.0 * (p - floor(p)) - 1.0; }
static double sqrf(double p, double w) { return (p - floor(p)) < w ? 1.0 : -1.0; }
static double trif(double p) { double x = p - floor(p); return 4.0 * fabs(x - 0.5) - 1.0; }

// Timbre by GM program family; returns sample and sets attack/release/sustain.
static double timbre(voice_t *v, double dt, double *atk, double *rel, double *sus, double *decay)
{
    int fam = v->prog / 8;
    double p = v->phase;
    *atk = 0.004; *rel = 0.08; *sus = 0.8; *decay = 0;
    switch (fam)
    {
    case 0: // piano
    case 1: // chromatic perc
        *sus = 0.0; *decay = 3.0; *rel = 0.15;
        return 0.6 * sin(2 * M_PI * p) + 0.25 * sin(4 * M_PI * p) + 0.1 * sin(6 * M_PI * p);
    case 2: // organ
        *rel = 0.03;
        return 0.5 * sin(2 * M_PI * p) + 0.3 * sin(4 * M_PI * p) + 0.2 * sin(8 * M_PI * p);
    case 3: // guitar
        *sus = 0.15; *decay = 2.0; *rel = 0.1;
        return 0.5 * sawf(p) + 0.3 * sin(2 * M_PI * p);
    case 4: // bass
        *sus = 0.5; *decay = 1.2; *rel = 0.06;
        return 0.7 * sqrf(p, 0.5) * 0.6 + 0.5 * sin(2 * M_PI * p);
    case 5: // strings
    case 6: // ensemble
        *atk = 0.06; *rel = 0.15;
        return 0.5 * sawf(p) + 0.3 * sawf(p * 1.005);
    case 7: // brass
    case 8: // reed
        *atk = 0.03;
        return 0.6 * sqrf(p, 0.3) + 0.2 * sawf(p);
    case 9: // pipe
    case 10: // synth lead
        return 0.5 * sqrf(p, 0.5);
    case 11: // synth pad
        *atk = 0.08; *rel = 0.2;
        return 0.5 * trif(p) + 0.3 * sawf(p * 1.003);
    case 12: case 13: case 14: case 15: // fx/ethnic/percussive/sfx
    default:
        *sus = 0.3; *decay = 2.0;
        return 0.6 * trif(p);
    }
}

static double drum(voice_t *v, uint32_t age)
{
    double t = (double)age / RATE;
    int n = v->note;
    double s;

    v->rng = v->rng * 1664525u + 1013904223u;
    s = ((v->rng >> 16) / 32768.0) - 1.0;
    if (n == 35 || n == 36) // kick
    {
        double f = 120 * exp(-t * 18) + 45;
        v->phase += f / RATE;
        return sin(2 * M_PI * v->phase) * exp(-t * 9);
    }
    if (n == 38 || n == 40) // snare
        return (0.6 * s + 0.4 * sin(2 * M_PI * 190 * t)) * exp(-t * 18);
    if (n == 42 || n == 44) // closed hat
    {
        double hp = s - v->lp; v->lp = s;
        return hp * exp(-t * 60) * 0.7;
    }
    if (n == 46 || n == 49 || n == 51 || n == 52 || n == 55 || n == 57 || n == 59) // open hat/cymbals
    {
        double hp = s - v->lp; v->lp = s;
        return hp * exp(-t * 8) * 0.6;
    }
    if (n >= 41 && n <= 50) // toms
    {
        double f = 90 + (n - 41) * 18;
        v->phase += (f * (1 + 0.5 * exp(-t * 20))) / RATE;
        return sin(2 * M_PI * v->phase) * exp(-t * 10);
    }
    return s * exp(-t * 30) * 0.5;
}

static int16_t *render(evlist_t *l, size_t *out_n)
{
    static voice_t voices[MAX_VOICES];
    uint32_t end = 0, total;
    int i, ei = 0;
    int16_t *out;
    uint32_t t;
    int hit_end = 0;

    memset(voices, 0, sizeof(voices));
    for (i = 0; i < 16; ++i) { ch_vol[i] = 100 / 127.0; ch_bend[i] = 0; ch_prog[i] = 0; }
    for (i = 0; i < l->n; ++i)
    {
        if (l->e[i].t > end) end = l->e[i].t;
        if (l->e[i].type == 6 && l->e[i].v == 1) hit_end = 1;
    }
    (void)hit_end;
    total = end + RATE / 2; // short tail
    if (total > (uint32_t)MAX_SECONDS * RATE) total = MAX_SECONDS * RATE;
    out = calloc(total, sizeof(int16_t));

    for (t = 0; t < total; ++t)
    {
        double mix = 0;

        while (ei < l->n && l->e[ei].t <= t)
        {
            ev_t *e = &l->e[ei++];
            switch (e->type)
            {
            case 0:
                for (i = 0; i < MAX_VOICES; ++i)
                    if (voices[i].active && voices[i].ch == e->ch && voices[i].note == e->a && !voices[i].released)
                    { voices[i].released = 1; break; }
                break;
            case 1:
            {
                int slot = -1, oldest = 0;
                uint32_t oa = 0;
                for (i = 0; i < MAX_VOICES; ++i)
                {
                    if (!voices[i].active) { slot = i; break; }
                    if (voices[i].age >= oa) { oa = voices[i].age; oldest = i; }
                }
                if (slot < 0) slot = oldest;
                voices[slot] = (voice_t){0};
                voices[slot].active = 1;
                voices[slot].ch = e->ch;
                voices[slot].note = e->a;
                voices[slot].vel = e->b / 127.0;
                voices[slot].prog = ch_prog[e->ch];
                voices[slot].freq = 440.0 * pow(2.0, (e->a - 69) / 12.0);
                voices[slot].rng = 12345 + e->a;
                break;
            }
            case 2: ch_prog[e->ch] = e->a; break;
            case 3: ch_vol[e->ch] = e->a / 127.0; break;
            case 5: ch_bend[e->ch] = e->v / 8192.0 * 2.0; break; // +-2 semitones
            case 6:
                for (i = 0; i < MAX_VOICES; ++i)
                    if (voices[i].active && (e->v == 1 || voices[i].ch == e->ch)) voices[i].released = 1;
                break;
            }
        }

        for (i = 0; i < MAX_VOICES; ++i)
        {
            voice_t *v = &voices[i];
            double s, g;

            if (!v->active) continue;
            if (v->ch == 9)
            {
                s = drum(v, v->age++);
                if (v->age > RATE) v->active = 0;
                g = v->vel * ch_vol[9];
                mix += s * g * 0.6;
                continue;
            }
            {
                double atk, rel, sus, decay, f, level;

                f = v->freq * pow(2.0, ch_bend[v->ch] / 12.0);
                v->phase += f / RATE;
                s = timbre(v, 1.0 / RATE, &atk, &rel, &sus, &decay);
                {
                    double age_s = v->age / (double)RATE;
                    double target;

                    if (v->released)
                        v->env -= 1.0 / (rel * RATE);
                    else if (age_s < atk)
                        v->env = age_s / atk;
                    else if (decay > 0)
                    {
                        target = sus + (1.0 - sus) * exp(-(age_s - atk) * decay);
                        v->env = target;
                    }
                    else
                        v->env = sus > 0 ? 1.0 : 1.0;
                    if (v->env <= 0.0005 && v->released) { v->active = 0; continue; }
                    if (v->env < 0) v->env = 0;
                    level = v->env;
                    if (!v->released && decay == 0) level *= (sus > 0 ? 1.0 : 1.0);
                }
                v->age++;
                g = v->vel * ch_vol[v->ch] * level;
                if (!v->released && v->age > (uint32_t)RATE * 12 && decay > 0 && v->env < 0.01)
                    v->active = 0;
                mix += s * g * 0.35;
            }
        }

        {
            // soft clip
            double y = tanh(mix * 0.9);
            out[t] = (int16_t)(y * 30000);
        }
    }
    *out_n = total;
    return out;
}

static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int ima_idx[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

#define BLOCK_BYTES 256
#define BLOCK_SAMPLES ((BLOCK_BYTES - 4) * 2 + 1)

static int ima_encode(int sample, int *pred, int *index)
{
    int step = ima_step[*index], diff = sample - *pred, code = 0, dq;

    if (diff < 0) { code = 8; diff = -diff; }
    dq = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; dq += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; dq += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; dq += step; }
    *pred += (code & 8) ? -dq : dq;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *index += ima_idx[code];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return code;
}

// Writes a mono IMA-ADPCM WAV; pdc passes it straight into the .pda (4 bits/sample).
static void write_wav(const char *path, const int16_t *s, size_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t nblocks = (uint32_t)((n + BLOCK_SAMPLES - 1) / BLOCK_SAMPLES), v32;
    uint32_t dlen = nblocks * BLOCK_BYTES;
    uint16_t v16;
    size_t pos = 0;
    uint32_t b;

    if (!f) { perror(path); return; }
    fwrite("RIFF", 1, 4, f); v32 = 4 + 8 + 20 + 12 + 8 + dlen; fwrite(&v32, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v32 = 20; fwrite(&v32, 4, 1, f);
    v16 = 0x11; fwrite(&v16, 2, 1, f); v16 = 1; fwrite(&v16, 2, 1, f);
    v32 = RATE; fwrite(&v32, 4, 1, f); v32 = (uint32_t)((uint64_t)RATE * BLOCK_BYTES / BLOCK_SAMPLES); fwrite(&v32, 4, 1, f);
    v16 = BLOCK_BYTES; fwrite(&v16, 2, 1, f); v16 = 4; fwrite(&v16, 2, 1, f);
    v16 = 2; fwrite(&v16, 2, 1, f); v16 = BLOCK_SAMPLES; fwrite(&v16, 2, 1, f);
    fwrite("fact", 1, 4, f); v32 = 4; fwrite(&v32, 4, 1, f); v32 = (uint32_t)n; fwrite(&v32, 4, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dlen, 4, 1, f);

    for (b = 0; b < nblocks; ++b)
    {
        uint8_t blk[BLOCK_BYTES] = {0};
        int pred = pos < n ? s[pos] : 0, index = 0, i;

        blk[0] = pred & 255; blk[1] = (pred >> 8) & 255; blk[2] = 0; blk[3] = 0;
        pos++;
        for (i = 0; i < (BLOCK_BYTES - 4) * 2; ++i, ++pos)
        {
            int code = ima_encode(pos < n ? s[pos] : 0, &pred, &index);
            if (i & 1) blk[4 + i / 2] |= code << 4; else blk[4 + i / 2] = code;
        }
        // header index byte must reflect state after first sample (0 for the first block start)
        fwrite(blk, 1, BLOCK_BYTES, f);
    }
    fclose(f);
}

static int skip_wad(const char *name)
{
    static const char *skip[] = {"heretic", "hexen", "hexdd", "strife", "voices"};
    for (int i = 0; i < 5; ++i)
        if (strcasestr(name, skip[i])) return 1;
    return 0;
}

static void do_wad(const char *path, const char *outdir)
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[12];
    uint32_t nl, dir, i;
    uint8_t *dirbuf;

    if (!f) return;
    if (fread(hdr, 1, 12, f) != 12 || (memcmp(hdr, "IWAD", 4) && memcmp(hdr, "PWAD", 4))) { fclose(f); return; }
    nl = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    dir = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    dirbuf = malloc((size_t)nl * 16);
    fseek(f, dir, SEEK_SET);
    if (fread(dirbuf, 16, nl, f) != nl) { free(dirbuf); fclose(f); return; }

    for (i = 0; i < nl; ++i)
    {
        const uint8_t *e = dirbuf + i * 16;
        uint32_t pos = e[0] | (e[1] << 8) | (e[2] << 16) | ((uint32_t)e[3] << 24);
        uint32_t size = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
        uint8_t *data;
        uint32_t h;
        char wav[1024], name[9] = {0};
        struct stat st;
        evlist_t l = {0};
        int bad;

        memcpy(name, e + 8, 8);
        if (size < 16 || (name[0] != 'D' || name[1] != '_')) continue;
        data = malloc(size);
        fseek(f, pos, SEEK_SET);
        if (fread(data, 1, size, f) != size) { free(data); continue; }
        h = fnv(data, size);
        snprintf(wav, sizeof(wav), "%s/%08x.wav", outdir, h);
        if (stat(wav, &st) == 0) { free(data); continue; } // already rendered (dup or earlier build)

        if (!memcmp(data, "MUS\x1a", 4)) bad = parse_mus(data, size, &l);
        else if (!memcmp(data, "MThd", 4)) bad = parse_midi(data, size, &l);
        else bad = 1;
        if (!bad && l.n > 0)
        {
            size_t n;
            int16_t *pcm;

            pcm = render(&l, &n);
            write_wav(wav, pcm, n);
            printf("music: %s %s -> %08x.wav (%.1fs)\n", path, name, h, (double)n / RATE);
            free(pcm);
        }
        free(l.e);
        free(data);
    }
    free(dirbuf);
    fclose(f);
}

int main(int argc, char **argv)
{
    DIR *d;
    struct dirent *de;

    if (argc < 3) { fprintf(stderr, "usage: %s wad-dir out-dir\n", argv[0]); return 1; }
    mkdir(argv[2], 0755);
    d = opendir(argv[1]);
    if (!d) return 0;
    while ((de = readdir(d)))
    {
        size_t n = strlen(de->d_name);
        char path[1024];

        if (n < 5 || strcasecmp(de->d_name + n - 4, ".wad") || skip_wad(de->d_name)) continue;
        snprintf(path, sizeof(path), "%s/%s", argv[1], de->d_name);
        do_wad(path, argv[2]);
    }
    closedir(d);
    return 0;
}
