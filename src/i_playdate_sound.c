// Playdate sound module for doomgeneric. Sound effects: Doom's 8-bit mono
// samples are software-mixed into a Playdate callback audio source.
// Music: MUS lumps are converted to MIDI (mus2mid), the percussion channel is
// dropped, and the result is played by a Playdate sequence over simple synths.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "memio.h"
#include "mus2mid.h"

#include "pd_api.h"
#include "pd_glue.h"
#include "dgpd.h"

// Diagnostic log (Data/.../musiclog.txt) that survives a hard-fault reboot,
// unlike logToConsole (which needs a live console attached to see). Each
// call opens, appends, and closes immediately so the last line written is
// flushed even if we crash moments later. Used while tracking down a
// device-only hard fault in the music code (now fixed - see PD_RegisterSong)
// and left in since it's cheap and may be useful again.
#include <stdarg.h>
static void DiagLog(const char *fmt, ...)
{
    PlaydateAPI *pd = pd_glue_api();
    char buf[192];
    va_list ap;
    SDFile *f;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    f = pd->file->open("musiclog.txt", kFileAppend);
    if (f != NULL)
    {
        pd->file->write(f, buf, strlen(buf));
        pd->file->write(f, "\n", 1);
        pd->file->close(f);
    }
}

#define MIX_CHANNELS 16
#define OUT_RATE 44100

typedef struct
{
    const uint8_t *data; // unsigned 8-bit samples
    uint32_t length;     // in samples
    uint32_t pos;        // 16.16 fixed-point read position
    uint32_t step;       // 16.16 fixed-point increment per output sample
    int left, right;     // 0..254 gains
    volatile int active;
} mix_channel_t;

static mix_channel_t mix_channels[MIX_CHANNELS];
static int sound_initialized;

// Config variables i_sound.c binds under FEATURE_SOUND (normally from i_sdlsound.c).
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

typedef struct
{
    const uint8_t *data;
    uint32_t length;
    uint32_t rate;
} sfx_data_t;

// Runs on the audio thread.
static int mix_callback(void *context, int16_t *left, int16_t *right, int len)
{
    int produced = 0;
    int c, i;

    (void)context;

    for (i = 0; i < len; ++i)
    {
        left[i] = 0;
        right[i] = 0;
    }

    for (c = 0; c < MIX_CHANNELS; ++c)
    {
        mix_channel_t *ch = &mix_channels[c];

        if (!ch->active)
            continue;

        for (i = 0; i < len; ++i)
        {
            uint32_t idx = ch->pos >> 16;
            int s, l, r;

            if (idx >= ch->length)
            {
                ch->active = 0;
                break;
            }

            s = (int)ch->data[idx] - 128;
            l = left[i] + s * ch->left;
            r = right[i] + s * ch->right;
            left[i] = l > 32767 ? 32767 : (l < -32768 ? -32768 : l);
            right[i] = r > 32767 ? 32767 : (r < -32768 ? -32768 : r);
            ch->pos += ch->step;
        }
        produced = 1;
    }

    return produced;
}

static boolean PD_InitSound(boolean use_sfx_prefix)
{
    PlaydateAPI *pd = pd_glue_api();

    (void)use_sfx_prefix;

    if (pd == NULL || pd->sound == NULL)
        return false;

    pd->sound->addSource(mix_callback, NULL, 1);
    sound_initialized = 1;
    return true;
}

static void PD_ShutdownSound(void)
{
    int c;

    for (c = 0; c < MIX_CHANNELS; ++c)
        mix_channels[c].active = 0;
}

static int PD_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[9];

    if (sfx->link != NULL)
        sfx = sfx->link;

    snprintf(name, sizeof(name), "ds%s", sfx->name);
    return W_CheckNumForName(name);
}

// Parse a DMX sound lump once and keep it resident.
static sfx_data_t *LoadSfx(sfxinfo_t *sfx)
{
    sfx_data_t *sd;
    const uint8_t *lump;
    uint32_t len, lumplen;
    int lumpnum;

    if (sfx->driver_data != NULL)
        return sfx->driver_data;

    lumpnum = PD_GetSfxLumpNum(sfx);
    if (lumpnum < 0)
        return NULL;

    lumplen = W_LumpLength(lumpnum);
    if (lumplen < 8)
        return NULL;

    lump = W_CacheLumpNum(lumpnum, PU_STATIC);
    if (lump[0] != 0x03 || lump[1] != 0x00)
        return NULL;

    len = lump[4] | (lump[5] << 8) | (lump[6] << 16) | ((uint32_t)lump[7] << 24);
    if (len > lumplen - 8 || len <= 48)
        return NULL;

    // 16 bytes of padding on each side of the sample data.
    sd = malloc(sizeof(*sd));
    if (sd == NULL)
        return NULL;
    sd->rate = lump[2] | (lump[3] << 8);
    sd->data = lump + 8 + 16;
    sd->length = len - 32;

    sfx->driver_data = sd;
    return sd;
}

static void PD_UpdateSound(void)
{
}

static void SetGains(mix_channel_t *ch, int vol, int sep)
{
    // vol is 0..127 and sep 0..254; each sample (+-128) * gain (<=254) fits int16.
    ch->left = ((254 - sep) * vol) / 127;
    ch->right = (sep * vol) / 127;
}

static void PD_UpdateSoundParams(int channel, int vol, int sep)
{
    if (!sound_initialized || channel < 0 || channel >= MIX_CHANNELS)
        return;

    SetGains(&mix_channels[channel], vol, sep);
}

static int PD_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    mix_channel_t *ch;
    sfx_data_t *sd;

    if (!sound_initialized || channel < 0 || channel >= MIX_CHANNELS)
        return -1;

    sd = LoadSfx(sfx);
    if (sd == NULL)
        return -1;

    ch = &mix_channels[channel];
    ch->active = 0;
    __sync_synchronize();

    ch->data = sd->data;
    ch->length = sd->length;
    ch->pos = 0;
    ch->step = (uint32_t)(((uint64_t)sd->rate << 16) / OUT_RATE);
    SetGains(ch, vol, sep);

    __sync_synchronize();
    ch->active = 1;
    return channel;
}

static void PD_StopSound(int channel)
{
    if (channel >= 0 && channel < MIX_CHANNELS)
        mix_channels[channel].active = 0;
}

static boolean PD_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= MIX_CHANNELS)
        return false;

    return mix_channels[channel].active != 0;
}

static snddevice_t sound_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
                                      SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS,
                                      SNDDEVICE_AWE32};

sound_module_t DG_sound_module = {
    sound_devices,
    sizeof(sound_devices) / sizeof(sound_devices[0]),
    PD_InitSound,
    PD_ShutdownSound,
    PD_GetSfxLumpNum,
    PD_UpdateSound,
    PD_UpdateSoundParams,
    PD_StartSound,
    PD_StopSound,
    PD_SoundIsPlaying,
    NULL,
};

// ---- Music ----

// MIDI only has 16 channels, and Playdate's MIDI loader gives each channel
// that's actually used its own SequenceTrack, so 16 covers any song.
#define MUSIC_MAX_TRACKS 16
#define MUSIC_VOICES 2
#define MUSIC_PATH "music.mid"
#define MUSIC_GAIN 0.45f

// Every track of the currently-playing song gets its own instrument (and its
// own synths). Sharing one instrument across two tracks of the same sequence
// crashed the device (see i_playdate_sound.c history) - the sound engine
// apparently doesn't expect one PDSynthInstrument to be driven by two tracks
// at once. These are torn down in PD_UnRegisterSong.
static PDSynthInstrument *song_instruments[MUSIC_MAX_TRACKS];
static PDSynth *song_synths[MUSIC_MAX_TRACKS][MUSIC_VOICES];
static int song_track_count;

static SoundSequence *sequence;
static int music_looping;
static int music_volume = 127;
static int music_enabled = 1;
static int music_paused;

static void ApplyMusicVolume(void)
{
    PlaydateAPI *pd = pd_glue_api();
    float v = MUSIC_GAIN * music_volume / 127.0f;
    int i;

    for (i = 0; i < song_track_count; ++i)
        if (song_instruments[i] != NULL)
            pd->sound->instrument->setVolume(song_instruments[i], v, v);
}

// Frees any instruments/synths left over from the previous song.
static void FreeSongInstruments(void)
{
    PlaydateAPI *pd = pd_glue_api();
    int i, v;

    for (i = 0; i < song_track_count; ++i)
    {
        for (v = 0; v < MUSIC_VOICES; ++v)
        {
            if (song_synths[i][v] != NULL)
                pd->sound->synth->freeSynth(song_synths[i][v]);
            song_synths[i][v] = NULL;
        }
        if (song_instruments[i] != NULL)
            pd->sound->instrument->freeInstrument(song_instruments[i]);
        song_instruments[i] = NULL;
    }
    song_track_count = 0;
}

// One fresh instrument (with its own synths) per track, sized to this song.
static void CreateSongInstruments(int tracks)
{
    static const SoundWaveform waves[4] = {kWaveformSquare, kWaveformSawtooth,
                                            kWaveformTriangle, kWaveformSine};
    PlaydateAPI *pd = pd_glue_api();
    int i, v;

    FreeSongInstruments();

    if (tracks > MUSIC_MAX_TRACKS)
        tracks = MUSIC_MAX_TRACKS;
    song_track_count = tracks;

    for (i = 0; i < tracks; ++i)
    {
        song_instruments[i] = pd->sound->instrument->newInstrument();
        if (song_instruments[i] == NULL)
            continue;

        for (v = 0; v < MUSIC_VOICES; ++v)
        {
            PDSynth *synth = pd->sound->synth->newSynth();

            song_synths[i][v] = synth;
            if (synth == NULL)
                continue;

            pd->sound->synth->setWaveform(synth, waves[i % 4]);
            pd->sound->synth->setAttackTime(synth, 0.01f);
            pd->sound->synth->setDecayTime(synth, 0.1f);
            pd->sound->synth->setSustainLevel(synth, 0.6f);
            pd->sound->synth->setReleaseTime(synth, 0.1f);
            pd->sound->instrument->addVoice(song_instruments[i], synth, 0, 127, 0);
        }
    }
    ApplyMusicVolume();
}

static size_t PutVarLen(uint8_t *out, uint32_t v)
{
    uint8_t tmp[5];
    size_t n = 0, i;

    tmp[n++] = v & 0x7f;
    while ((v >>= 7) != 0)
        tmp[n++] = (v & 0x7f) | 0x80;
    for (i = 0; i < n; ++i)
        out[i] = tmp[n - 1 - i];
    return n;
}

static uint32_t GetVarLen(const uint8_t *in, size_t n, size_t *p)
{
    uint32_t v = 0;
    uint8_t b;

    do
    {
        if (*p >= n)
            return 0;
        b = in[(*p)++];
        v = (v << 7) | (b & 0x7f);
    } while (b & 0x80);
    return v;
}

// Rewrite a single-track MIDI file without the percussion channel (10), whose
// notes would otherwise come out as random pitched beeps. Returns the new
// length, or 0 if the data isn't the expected layout.
static size_t StripPercussion(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t p = 22, o = 22;
    uint32_t pending = 0;
    uint8_t running = 0;

    if (n < 22 || memcmp(in, "MThd", 4) != 0 || memcmp(in + 14, "MTrk", 4) != 0)
        return 0;
    memcpy(out, in, 22);

    while (p < n)
    {
        uint32_t delta = GetVarLen(in, n, &p);
        uint8_t status;
        size_t start, len;
        int keep = 1;

        if (p >= n)
            break;
        pending += delta;

        status = in[p];
        if (status >= 0x80)
        {
            p++;
            if (status < 0xf0)
                running = status;
        }
        else
        {
            status = running;
        }

        start = p;
        if (status == 0xff)
        {
            p++; // meta type
            len = GetVarLen(in, n, &p);
            p += len;
        }
        else if (status == 0xf0 || status == 0xf7)
        {
            len = GetVarLen(in, n, &p);
            p += len;
        }
        else
        {
            uint8_t hi = status & 0xf0;

            p += (hi == 0xc0 || hi == 0xd0) ? 1 : 2;
            keep = (status & 0x0f) != 9;
        }
        if (p > n)
            break;

        if (keep)
        {
            o += PutVarLen(out + o, pending);
            out[o++] = status;
            memcpy(out + o, in + start, p - start);
            o += p - start;
            pending = 0;
        }
    }

    out[18] = (o - 22) >> 24;
    out[19] = (o - 22) >> 16;
    out[20] = (o - 22) >> 8;
    out[21] = (o - 22);
    return o;
}

static boolean PD_InitMusic(void)
{
    return pd_glue_api()->sound != NULL;
}

static void PD_ShutdownMusic(void)
{
    if (sequence != NULL)
        pd_glue_api()->sound->sequence->stop(sequence);
    FreeSongInstruments();
}

static void PD_SetMusicVolume(int volume)
{
    music_volume = volume;
    ApplyMusicVolume();
}

static void PD_PauseMusic(void)
{
    music_paused = 1;
    if (sequence != NULL)
        pd_glue_api()->sound->sequence->stop(sequence);
}

static void PD_ResumeMusic(void)
{
    music_paused = 0;
    if (sequence != NULL && music_enabled)
        pd_glue_api()->sound->sequence->play(sequence, NULL, NULL);
}

static void *PD_RegisterSong(void *data, int len)
{
    PlaydateAPI *pd = pd_glue_api();
    MEMFILE *in, *out;
    void *midi;
    size_t midilen, outlen;
    uint8_t *filtered;
    SDFile *f;
    SoundSequence *seq;
    int i, tracks;

    in = mem_fopen_read(data, len);
    out = mem_fopen_write();
    if (mus2mid(in, out)) // returns true on failure
    {
        DiagLog("mus2mid failed (len=%d)", len);
        mem_fclose(in);
        mem_fclose(out);
        return NULL;
    }
    mem_get_buf(out, &midi, &midilen);

    filtered = malloc(midilen * 2 + 64);
    outlen = filtered ? StripPercussion(midi, midilen, filtered) : 0;
    mem_fclose(in);
    if (outlen == 0)
    {
        DiagLog("StripPercussion failed (midilen=%d)", (int)midilen);
        free(filtered);
        mem_fclose(out);
        return NULL;
    }

    f = pd->file->open(MUSIC_PATH, kFileWrite);
    if (f == NULL)
    {
        DiagLog("could not open %s for write: %s", MUSIC_PATH, pd->file->geterr());
        free(filtered);
        mem_fclose(out);
        return NULL;
    }
    pd->file->write(f, filtered, outlen);
    pd->file->close(f);
    free(filtered);
    mem_fclose(out);

    // NOTE: as of SDK/firmware 3.1.2, loadMIDIFile() below reliably fails
    // (returns 0) for every file tested here, including a trivial hand-built
    // MIDI file written to the Data directory the same way - not just our
    // generated ones - which points at loadMIDIFile() only finding files
    // bundled into the .pdx at compile time, not ones written at runtime.
    // Until that's confirmed/resolved this path always ends in "no music",
    // same as before this feature existed, but importantly no longer
    // crashes (see the comment on the missing pd->sound->getError() call
    // below - calling it here reliably hard-faulted the device).
    seq = pd->sound->sequence->newSequence();
    if (!pd->sound->sequence->loadMIDIFile(seq, MUSIC_PATH))
    {
        // Do not call pd->sound->getError() here - see note above the
        // NOTE this function's comment references was written from: it
        // consistently hard-faults the device (Error e0) on this firmware.
        DiagLog("loadMIDIFile failed for %s", MUSIC_PATH);
        pd->sound->sequence->freeSequence(seq);
        return NULL;
    }

    tracks = pd->sound->sequence->getTrackCount(seq);
    DiagLog("loaded %s, %d bytes, %d track(s), length %d steps",
            MUSIC_PATH, (int)outlen, tracks, (int)pd->sound->sequence->getLength(seq));

    // Every track gets its own instrument: two tracks of the same sequence
    // sharing one instrument previously crashed the device (see comment on
    // song_instruments above).
    CreateSongInstruments(tracks);
    for (i = 0; i < tracks && i < MUSIC_MAX_TRACKS; ++i)
    {
        SequenceTrack *track = pd->sound->sequence->getTrackAtIndex(seq, i);

        if (track != NULL && song_instruments[i] != NULL)
            pd->sound->track->setInstrument(track, song_instruments[i]);
    }

    sequence = seq;
    return seq;
}

static void PD_UnRegisterSong(void *handle)
{
    PlaydateAPI *pd = pd_glue_api();

    if (handle == NULL)
        return;

    pd->sound->sequence->stop(handle);
    pd->sound->sequence->allNotesOff(handle);
    pd->sound->sequence->freeSequence(handle);
    if (sequence == handle)
        sequence = NULL;
    FreeSongInstruments();
}

// Called on the audio thread when a song reaches its end. Looping is
// implemented by hand (rewind + replay) instead of sequence->setLoops():
// setLoops(seq, 0, getLength(seq), 0) reliably hard-faulted the device the
// moment a level's (looping) music started - see i_playdate_sound.c history -
// while non-looping intro/menu music, which never calls setLoops, was fine.
static void SequenceFinished(SoundSequence *seq, void *userdata)
{
    PlaydateAPI *pd = pd_glue_api();

    (void)userdata;
    if (seq != sequence || !music_looping || !music_enabled || music_paused)
        return;

    pd->sound->sequence->setTime(seq, 0);
    pd->sound->sequence->play(seq, SequenceFinished, NULL);
}

static void PD_PlaySong(void *handle, boolean looping)
{
    PlaydateAPI *pd = pd_glue_api();

    if (handle == NULL)
        return;

    music_looping = looping;
    music_paused = 0;
    pd->sound->sequence->setTime(handle, 0);
    if (music_enabled)
        pd->sound->sequence->play(handle, SequenceFinished, NULL);
    pd->system->logToConsole("i_playdate_sound: PlaySong enabled=%d looping=%d isPlaying=%d vol=%d",
                              music_enabled, looping, pd->sound->sequence->isPlaying(handle),
                              music_volume);
}

static void PD_StopSong(void)
{
    if (sequence != NULL)
        pd_glue_api()->sound->sequence->stop(sequence);
}

static boolean PD_MusicIsPlaying(void)
{
    return sequence != NULL && pd_glue_api()->sound->sequence->isPlaying(sequence);
}

// System-menu toggle.
void dgpd_SetMusicEnabled(int on)
{
    music_enabled = on;
    if (sequence == NULL)
        return;

    if (!on)
    {
        pd_glue_api()->sound->sequence->stop(sequence);
        pd_glue_api()->sound->sequence->allNotesOff(sequence);
    }
    else if (!music_paused)
    {
        pd_glue_api()->sound->sequence->play(sequence, SequenceFinished, NULL);
    }
}

music_module_t DG_music_module = {
    sound_devices,
    sizeof(sound_devices) / sizeof(sound_devices[0]),
    PD_InitMusic,
    PD_ShutdownMusic,
    PD_SetMusicVolume,
    PD_PauseMusic,
    PD_ResumeMusic,
    PD_RegisterSong,
    PD_UnRegisterSong,
    PD_PlaySong,
    PD_StopSong,
    PD_MusicIsPlaying,
    NULL,
};
