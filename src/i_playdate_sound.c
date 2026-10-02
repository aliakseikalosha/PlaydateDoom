// Playdate sound module for doomgeneric. Sound effects: Doom's 8-bit mono
// samples are software-mixed into a Playdate callback audio source.
// Music: pre-rendered at build time to .pda files and streamed by a FilePlayer.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

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
// Music is pre-rendered at build time (tools/musrender.c writes one WAV per
// music lump, named by the FNV-1a hash of the lump bytes, into Source/music;
// pdc turns them into .pda). At runtime RegisterSong just hashes the lump
// data to find its file and a FilePlayer streams it.

typedef struct
{
    char path[32];
} music_song_t;

static FilePlayer *music_player;
static music_song_t *current_song;
static int music_volume = 127;
static int music_looping;
static int music_paused;

static uint32_t HashLump(const uint8_t *d, int n)
{
    uint32_t h = 2166136261u;

    while (n-- > 0)
        h = (h ^ *d++) * 16777619u;
    return h;
}

static void ApplyMusicVolume(void)
{
    float v = music_volume / 127.0f;

    if (music_player != NULL)
        pd_glue_api()->sound->fileplayer->setVolume(music_player, v, v);
}

static boolean PD_InitMusic(void)
{
    PlaydateAPI *pd = pd_glue_api();

    if (pd->sound == NULL)
        return false;
    music_player = pd->sound->fileplayer->newPlayer();
    ApplyMusicVolume();
    return music_player != NULL;
}

static void PD_StopSong(void)
{
    if (music_player != NULL)
        pd_glue_api()->sound->fileplayer->stop(music_player);
    current_song = NULL;
    music_paused = 0;
}

static void PD_ShutdownMusic(void)
{
    PD_StopSong();
    if (music_player != NULL)
    {
        pd_glue_api()->sound->fileplayer->freePlayer(music_player);
        music_player = NULL;
    }
}

static void PD_SetMusicVolume(int volume)
{
    music_volume = volume;
    ApplyMusicVolume();
}

static void PD_PauseMusic(void)
{
    if (music_player == NULL || music_paused || current_song == NULL)
        return;
    music_paused = 1;
    pd_glue_api()->sound->fileplayer->pause(music_player);
}

static void PD_ResumeMusic(void)
{
    if (music_player == NULL || !music_paused)
        return;
    music_paused = 0;
    pd_glue_api()->sound->fileplayer->play(music_player, music_looping ? 0 : 1);
}

static void *PD_RegisterSong(void *data, int len)
{
    music_song_t *song = malloc(sizeof(*song));

    if (song == NULL)
        return NULL;
    snprintf(song->path, sizeof(song->path), "music/%08x", (unsigned)HashLump(data, len));
    return song;
}

static void PD_UnRegisterSong(void *handle)
{
    if (handle == NULL)
        return;
    if (current_song == handle)
        PD_StopSong();
    free(handle);
}

static void PD_PlaySong(void *handle, boolean looping)
{
    music_song_t *song = handle;
    const struct playdate_sound_fileplayer *fp;

    if (song == NULL || music_player == NULL)
        return;

    fp = pd_glue_api()->sound->fileplayer;
    fp->stop(music_player);
    current_song = NULL;
    if (!fp->loadIntoPlayer(music_player, song->path))
    {
        DiagLog("music file missing: %s", song->path);
        return;
    }
    current_song = song;
    music_looping = looping;
    music_paused = 0;
    ApplyMusicVolume();
    fp->play(music_player, looping ? 0 : 1);
}

static boolean PD_MusicIsPlaying(void)
{
    return music_player != NULL &&
           (music_paused || pd_glue_api()->sound->fileplayer->isPlaying(music_player));
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
