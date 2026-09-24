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

    // The mixer source is added later by dgpd_StartAudio(), once loading is done.
    return true;
}

void dgpd_StartAudio(void)
{
    PlaydateAPI *pd = pd_glue_api();

    if (sound_initialized || pd == NULL || pd->sound == NULL)
        return;

    pd->sound->addSource(mix_callback, NULL, 1);
    sound_initialized = 1;
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
//
// PDSynthInstrument turned out to be non-functional on this device/firmware
// (3.1.2): newInstrument()/addVoice()/setInstrument() all report success,
// but nothing is ever audible, whether driven directly via
// instrument->playNote() or via a SoundSequence/SequenceTrack built with
// addNoteEvent() (which also sidesteps sequence->loadMIDIFile(), separately
// confirmed broken for runtime-written files - see git history). A plain
// PDSynth's own synth->playNote() DOES work, including scheduled for the
// future via the `when` parameter (confirmed on-device). So music here is a
// from-scratch scheduler: MUS is converted to MIDI (mus2mid) and walked once
// into a flat note list, then played back by handing plain PDSynth voices to
// synth->playNote() with sample-accurate `when` timestamps, a little ahead
// of when each note is actually due (see MUSIC_LOOKAHEAD_SAMPLES), polled
// once per game tic via the music_module_t Poll hook.

// MIDI channels 0-15, indexed directly. Channel 9 (percussion) is dropped -
// its notes would come out as random pitched beeps through a plain waveform
// synth.
#define MUSIC_MAX_TRACKS 16
#define MUSIC_VOICES 4 // synths per channel, round-robined for light overlap
#define MUSIC_GAIN 0.5f
// MUS's native tick rate (and what mus2mid's fixed 70-tick/quarter-note,
// assumed-120bpm header amounts to); using it 1:1 as our step rate means we
// don't need any MIDI tempo-meta handling at all.
#define MUSIC_STEPS_PER_SECOND 140.0f
#define MUSIC_SAMPLE_RATE 44100
// How far ahead of actual playback time notes are hg->synth->playNote()'d.
// Generous relative to the ~28ms poll interval (35 ticspersec) so polling
// jitter/hiccups never starve the schedule.
#define MUSIC_LOOKAHEAD_SAMPLES (MUSIC_SAMPLE_RATE / 2) // 0.5s

typedef struct
{
    uint32_t start_tick;
    uint32_t length_ticks;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
} music_note_t;

typedef struct
{
    music_note_t *notes;
    int count;
    uint32_t total_ticks;
} music_song_t;

// One small pool of synths per channel, created lazily and reused for the
// life of the game (not per song - a plain PDSynth, unlike the broken
// PDSynthInstrument, has no per-song state to rebuild).
static PDSynth *channel_synth[MUSIC_MAX_TRACKS][MUSIC_VOICES];
static int channel_next_voice[MUSIC_MAX_TRACKS];

static music_song_t *current_song;
static uint32_t play_base_time;  // sample time corresponding to song tick 0
static uint32_t pause_start_time;
static int play_next_index;      // index of the next not-yet-scheduled note
static int play_active;
static int music_looping;
static int music_volume = 127;
static int music_paused;

static void ApplyMusicVolume(void)
{
    PlaydateAPI *pd = pd_glue_api();
    float v = MUSIC_GAIN * music_volume / 127.0f;
    int c, i;

    for (c = 0; c < MUSIC_MAX_TRACKS; ++c)
        for (i = 0; i < MUSIC_VOICES; ++i)
            if (channel_synth[c][i] != NULL)
                pd->sound->synth->setVolume(channel_synth[c][i], v, v);
}

// Lazily creates a channel's voice pool, then round-robins across it so an
// overlapping note doesn't always cut off the immediately preceding one.
static PDSynth *GetChannelVoice(PlaydateAPI *pd, int channel)
{
    static const SoundWaveform waves[4] = {kWaveformSquare, kWaveformSawtooth,
                                            kWaveformTriangle, kWaveformSine};
    int i, slot;

    if (channel < 0 || channel >= MUSIC_MAX_TRACKS)
        return NULL;

    if (channel_synth[channel][0] == NULL)
    {
        float v = MUSIC_GAIN * music_volume / 127.0f;

        for (i = 0; i < MUSIC_VOICES; ++i)
        {
            PDSynth *synth = pd->sound->synth->newSynth();

            channel_synth[channel][i] = synth;
            if (synth == NULL)
                continue;
            pd->sound->synth->setWaveform(synth, waves[channel % 4]);
            pd->sound->synth->setAttackTime(synth, 0.005f);
            pd->sound->synth->setDecayTime(synth, 0.03f);
            pd->sound->synth->setSustainLevel(synth, 0.7f);
            pd->sound->synth->setReleaseTime(synth, 0.03f);
            pd->sound->synth->setVolume(synth, v, v);
        }
    }

    slot = channel_next_voice[channel] % MUSIC_VOICES;
    channel_next_voice[channel]++;
    return channel_synth[channel][slot];
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

static void AddNote(music_song_t *song, uint32_t start, uint32_t length, uint8_t channel,
                     uint8_t note, uint8_t vel)
{
    if (song->count >= 0 && (song->count & (song->count - 1)) == 0)
    {
        // count is 0 or a power of two: grow (amortised doubling; count==0
        // allocates the first 1-entry block).
        int newcap = song->count ? song->count * 2 : 1;
        music_note_t *bigger = realloc(song->notes, newcap * sizeof(music_note_t));

        if (bigger == NULL)
            return;
        song->notes = bigger;
    }
    song->notes[song->count].start_tick = start;
    song->notes[song->count].length_ticks = length;
    song->notes[song->count].channel = channel;
    song->notes[song->count].note = note;
    song->notes[song->count].velocity = vel;
    song->count++;
    if (start + length > song->total_ticks)
        song->total_ticks = start + length;
}

// Walks the MIDI byte stream mus2mid produced into a flat, time-ordered note
// list (see the big comment above this section for why this isn't just fed
// to sequence->loadMIDIFile()).
static void ParseMIDIIntoSong(music_song_t *song, const uint8_t *midi, size_t n)
{
    // Per-(channel, note) step a currently-held note started at, or -1.
    static int32_t note_start[MUSIC_MAX_TRACKS][128];
    static uint8_t note_vel[MUSIC_MAX_TRACKS][128];
    size_t p = 22; // MThd (14 bytes) + MTrk header (8 bytes), per mus2mid.c
    uint32_t step = 0;
    uint8_t running = 0;
    int c, note;

    if (n < 22 || memcmp(midi, "MThd", 4) != 0 || memcmp(midi + 14, "MTrk", 4) != 0)
        return;

    for (c = 0; c < MUSIC_MAX_TRACKS; ++c)
        for (note = 0; note < 128; ++note)
            note_start[c][note] = -1;

    while (p < n)
    {
        uint32_t delta = GetVarLen(midi, n, &p);
        uint8_t status;

        if (p >= n)
            break;
        step += delta;

        status = midi[p];
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

        if (status == 0xff)
        {
            uint32_t len;

            p++; // meta type, unused
            len = GetVarLen(midi, n, &p);
            p += len;
        }
        else if (status == 0xf0 || status == 0xf7)
        {
            uint32_t len = GetVarLen(midi, n, &p);

            p += len;
        }
        else if (status >= 0x80)
        {
            uint8_t hi = status & 0xf0;
            uint8_t ch = status & 0x0f;

            if (hi == 0xc0 || hi == 0xd0) // program change / channel pressure: 1 data byte
            {
                p += 1;
            }
            else // note on/off, controller, pitch bend: 2 data bytes
            {
                uint8_t d1 = (p < n) ? midi[p] : 0;
                uint8_t d2 = (p + 1 < n) ? midi[p + 1] : 0;

                p += 2;
                if (ch != 9 && (hi == 0x90 || hi == 0x80))
                {
                    uint8_t note_num = d1 & 0x7f;

                    if (hi == 0x90 && d2 > 0) // note on
                    {
                        note_start[ch][note_num] = (int32_t)step;
                        note_vel[ch][note_num] = d2;
                    }
                    else // note off, or note-on with velocity 0
                    {
                        int32_t start = note_start[ch][note_num];

                        if (start >= 0)
                        {
                            uint32_t length = step - (uint32_t)start;

                            if (length < 1)
                                length = 1;
                            AddNote(song, (uint32_t)start, length, ch, note_num, note_vel[ch][note_num]);
                            note_start[ch][note_num] = -1;
                        }
                    }
                }
            }
        }
        else
        {
            break; // stray data byte with no status yet: malformed, bail out
        }
    }
}

static uint32_t TickToSample(uint32_t tick)
{
    return (uint32_t)((uint64_t)tick * MUSIC_SAMPLE_RATE / (uint32_t)MUSIC_STEPS_PER_SECOND);
}

// Schedules any notes due within the lookahead window, and handles
// end-of-song/looping. Called once per game tic (see music_module_t::Poll)
// and once immediately from PD_PlaySong to seed the initial window.
static void PD_PollMusic(void)
{
    PlaydateAPI *pd = pd_glue_api();
    uint32_t now, horizon, song_end;

    if (!play_active || current_song == NULL || music_paused)
        return;

    now = pd->sound->getCurrentTime();
    horizon = now + MUSIC_LOOKAHEAD_SAMPLES;

    while (play_next_index < current_song->count)
    {
        music_note_t *n = &current_song->notes[play_next_index];
        uint32_t when = play_base_time + TickToSample(n->start_tick);
        PDSynth *voice;

        if (when > horizon)
            break;

        voice = GetChannelVoice(pd, n->channel);
        if (voice != NULL)
        {
            float freq = pd_noteToFrequency((float)n->note);
            float len_seconds = (float)n->length_ticks / MUSIC_STEPS_PER_SECOND;

            pd->sound->synth->playNote(voice, freq, n->velocity / 127.0f, len_seconds, when);
        }
        play_next_index++;
    }

    if (play_next_index >= current_song->count)
    {
        song_end = play_base_time + TickToSample(current_song->total_ticks);
        if (now >= song_end)
        {
            if (music_looping)
            {
                play_base_time = song_end;
                play_next_index = 0;
            }
            else
            {
                play_active = 0;
            }
        }
    }
}

static void SilenceAllChannels(void)
{
    PlaydateAPI *pd = pd_glue_api();
    int c, i;

    for (c = 0; c < MUSIC_MAX_TRACKS; ++c)
        for (i = 0; i < MUSIC_VOICES; ++i)
            if (channel_synth[c][i] != NULL)
                pd->sound->synth->noteOff(channel_synth[c][i], 0);
}

static boolean PD_InitMusic(void)
{
    return pd_glue_api()->sound != NULL;
}

static void PD_ShutdownMusic(void)
{
    play_active = 0;
    SilenceAllChannels();
}

static void PD_SetMusicVolume(int volume)
{
    music_volume = volume;
    ApplyMusicVolume();
}

static void PD_PauseMusic(void)
{
    if (music_paused)
        return;
    music_paused = 1;
    pause_start_time = pd_glue_api()->sound->getCurrentTime();
    SilenceAllChannels();
}

static void PD_ResumeMusic(void)
{
    if (!music_paused)
        return;
    music_paused = 0;
    // Shift the song's timeline forward by exactly how long we were paused,
    // so playback resumes where it left off instead of firing a burst of
    // "overdue" notes.
    play_base_time += pd_glue_api()->sound->getCurrentTime() - pause_start_time;
}

static void *PD_RegisterSong(void *data, int len)
{
    MEMFILE *in, *out;
    void *midi;
    size_t midilen;
    music_song_t *song;

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

    song = malloc(sizeof(*song));
    if (song == NULL)
    {
        mem_fclose(in);
        mem_fclose(out);
        return NULL;
    }
    song->notes = NULL;
    song->count = 0;
    song->total_ticks = 0;
    ParseMIDIIntoSong(song, midi, midilen);
    DiagLog("parsed %d-byte song (len=%d) into %d notes, %u ticks (%.1fs @ %d/s)",
            (int)midilen, len, song->count, song->total_ticks,
            (double)(song->total_ticks / MUSIC_STEPS_PER_SECOND), (int)MUSIC_STEPS_PER_SECOND);

    mem_fclose(in);
    mem_fclose(out);
    return song;
}

static void PD_UnRegisterSong(void *handle)
{
    music_song_t *song = handle;

    if (song == NULL)
        return;
    if (current_song == song)
    {
        play_active = 0;
        current_song = NULL;
        SilenceAllChannels();
    }
    free(song->notes);
    free(song);
}

static void PD_PlaySong(void *handle, boolean looping)
{
    music_song_t *song = handle;

    if (song == NULL)
        return;

    current_song = song;
    music_looping = looping;
    music_paused = 0;
    play_base_time = pd_glue_api()->sound->getCurrentTime();
    play_next_index = 0;
    play_active = 1;
    PD_PollMusic(); // seed the initial lookahead window immediately
    DiagLog("PlaySong: %d notes, looping=%d musicvol=%d",
            song->count, looping, music_volume);
}

static void PD_StopSong(void)
{
    play_active = 0;
    current_song = NULL;
    SilenceAllChannels();
}

static boolean PD_MusicIsPlaying(void)
{
    return play_active;
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
    PD_PollMusic,
};
