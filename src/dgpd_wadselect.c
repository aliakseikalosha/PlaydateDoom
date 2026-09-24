// Start-up WAD picker: see dgpd_wadselect.h.
//
// Cover Flow style carousel: each WAD's own title screen (its TITLEPIC lump,
// read straight out of the WAD) is the cover art. The selected cover faces
// the player, the others are turned toward it and shrink away in perspective,
// and every cover has a faded reflection on the "floor" below it.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_api.h"

#include "dgpd_wadselect.h"
#include "pd_glue.h"

// Any *.wad in the wad folder is offered; Doom identifies E1M1/MAP01
// games by contents when the name is not a standard one. These engines'
// IWADs would be misdetected as Doom, so they are skipped by name.
static const char *const unsupported_wads[] = {
    "heretic.wad", "heretic1.wad", "hexen.wad", "hexendemo.wad", "hexdd.wad",
    "strife0.wad", "strife1.wad", "voices.wad",
};

// Caption shown under the cover for well-known WADs (other files show their
// file name without the extension).
static const struct
{
    const char *file;
    const char *title;
} wad_titles[] = {
    {"doom1.wad", "DOOM Shareware"},
    {"doom.wad", "DOOM"},
    {"doomu.wad", "The Ultimate DOOM"},
    {"doom2.wad", "DOOM II: Hell on Earth"},
    {"doom2f.wad", "DOOM II (French)"},
    {"plutonia.wad", "Final DOOM: Plutonia"},
    {"tnt.wad", "Final DOOM: TNT Evilution"},
    {"chex.wad", "Chex Quest"},
    {"chex3.wad", "Chex Quest 3"},
};

#define WAD_DIR "wad" // folder in the game bundle (Source/wad/)
#define MAX_WADS 32
#define WAD_NAME_LEN 40

#define SCREEN_W 400
#define SCREEN_H 240
#define ROW_BYTES 52 // bytes per row of the Playdate frame buffer

// Cover art, 8-bit greyscale (Doom's own title screens are 4:3 once the
// non-square pixels are accounted for, so the covers are 4:3 too).
#define COVER_W 160
#define COVER_H 120

// Cover Flow geometry, in world units of a camera FLOW_D away from the
// selected cover (so a cover at the selected depth is drawn 1:1).
#define FLOW_D 480.0f
#define FLOW_W 144.0f      // width of a cover
#define FLOW_H 108.0f      // height of a cover
#define FLOW_YC 92         // screen row of the covers' vertical centre
#define FLOW_ANGLE 1.31f   // how far side covers are turned (75 degrees)
#define FLOW_GAP 112.0f    // x of the first cover beside the selected one
#define FLOW_STEP 46.0f    // x spacing of the covers after that
#define FLOW_BACK 60.0f    // how far side covers sit behind the selected one
#define FLOW_VISIBLE 3     // covers drawn on each side of the selected one
#define FLOW_MIRROR 0.45f  // reflection brightness right under the cover

#define CRANK_STEP 30.0f // degrees of crank per cover
#define REPEAT_DELAY 14  // frames a direction is held before it repeats
#define REPEAT_RATE 5    // frames between repeats

enum
{
    COVER_PENDING,
    COVER_LOADED,
    COVER_NONE // WAD has no usable title picture
};

static char wad_names[MAX_WADS][WAD_NAME_LEN];
static int wad_count;
static int wad_selected;
static uint8_t *wad_cover[MAX_WADS];
static uint8_t wad_cover_state[MAX_WADS];
static char wad_path[WAD_NAME_LEN + sizeof(WAD_DIR)];

static float flow_pos;     // position the carousel is drawn at (eases to wad_selected)
static int flow_dirty = 1; // something changed since the last draw
static int held_dir;       // -1/+1 while a direction is held, else 0
static int held_frames;
static float crank_accum;

// True if the file starts with a WAD header. "PWAD" is accepted too: some
// standalone games (Chex Quest) ship that way.
static int is_wad(const char *name)
{
    PlaydateAPI *pd = pd_glue_api();
    char path[WAD_NAME_LEN + sizeof(WAD_DIR)];
    char magic[4] = {0};
    SDFile *f;
    int n;

    snprintf(path, sizeof(path), WAD_DIR "/%s", name);
    f = pd->file->open(path, kFileRead | kFileReadData);
    if (f == NULL)
        return 0;
    n = pd->file->read(f, magic, 4);
    pd->file->close(f);
    return n == 4 && (memcmp(magic, "IWAD", 4) == 0 || memcmp(magic, "PWAD", 4) == 0);
}

static void wad_found(const char *filename, void *ud)
{
    size_t i, len = strlen(filename);

    (void)ud;
    if (wad_count >= MAX_WADS || len >= WAD_NAME_LEN || len < 5
        || strcasecmp(filename + len - 4, ".wad") != 0)
        return;

    for (i = 0; i < sizeof(unsupported_wads) / sizeof(unsupported_wads[0]); ++i)
        if (strcasecmp(filename, unsupported_wads[i]) == 0)
            return;

    if (is_wad(filename))
        strcpy(wad_names[wad_count++], filename);
}

void dgpd_wad_Scan(void)
{
    int i, j;

    wad_count = 0;
    pd_glue_api()->file->listfiles(WAD_DIR, wad_found, NULL, 0);

    for (i = 1; i < wad_count; ++i) // tiny insertion sort, alphabetical
    {
        char tmp[WAD_NAME_LEN];

        strcpy(tmp, wad_names[i]);
        for (j = i - 1; j >= 0 && strcasecmp(wad_names[j], tmp) > 0; --j)
            strcpy(wad_names[j + 1], wad_names[j]);
        strcpy(wad_names[j + 1], tmp);
    }
}

// ---------------------------------------------------------------------------
// Cover art: TITLEPIC out of the WAD, downsampled to COVER_W x COVER_H grey.
// ---------------------------------------------------------------------------

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

typedef struct
{
    uint32_t pos, size;
} lump_t;

static int lump_name_is(const uint8_t *name, const char *s)
{
    return strncmp((const char *)name, s, 8) == 0;
}

// Finds the title picture and the palette in a WAD's directory.
static int find_lumps(PlaydateAPI *pd, SDFile *f, lump_t *pic, lump_t *pal)
{
    uint8_t head[12], dir[64 * 16];
    uint32_t numlumps, dirpos, i, n;
    lump_t title = {0, 0}, inter = {0, 0};

    pal->size = 0;
    if (pd->file->read(f, head, 12) != 12)
        return 0;
    numlumps = le32(head + 4);
    dirpos = le32(head + 8);
    if (numlumps > 100000 || pd->file->seek(f, (int)dirpos, SEEK_SET) != 0)
        return 0;

    for (i = 0; i < numlumps; i += n)
    {
        uint32_t k;

        n = numlumps - i < 64 ? numlumps - i : 64;
        if (pd->file->read(f, dir, (unsigned int)(n * 16)) != (int)(n * 16))
            break;
        for (k = 0; k < n; ++k)
        {
            const uint8_t *e = dir + k * 16;
            lump_t l = {le32(e), le32(e + 4)};

            if (lump_name_is(e + 8, "TITLEPIC"))
                title = l;
            else if (lump_name_is(e + 8, "INTERPIC"))
                inter = l;
            else if (lump_name_is(e + 8, "PLAYPAL") && pal->size == 0)
                *pal = l;
        }
        if (title.size != 0 && pal->size != 0)
            break;
    }

    *pic = title.size != 0 ? title : inter;
    return pic->size != 0;
}

static int read_lump(PlaydateAPI *pd, SDFile *f, lump_t l, uint8_t *dst)
{
    return pd->file->seek(f, (int)l.pos, SEEK_SET) == 0
        && pd->file->read(f, dst, l.size) == (int)l.size;
}

// Draws a Doom patch (or, for 64000-byte lumps, a raw 320x200 flat) into a
// 320x200 buffer of palette indices. Returns the picture's size in *pw/*ph.
static int decode_pic(const uint8_t *buf, uint32_t size, uint8_t *out, int *pw, int *ph)
{
    int w, h, x;

    memset(out, 0, 320 * 200);
    if (size == 320 * 200)
    {
        memcpy(out, buf, size);
        *pw = 320;
        *ph = 200;
        return 1;
    }
    if (size < 8)
        return 0;

    w = le16(buf);
    h = le16(buf + 2);
    if (w < 1 || w > 320 || h < 1 || h > 200 || size < 8 + (uint32_t)w * 4)
        return 0;

    for (x = 0; x < w; ++x)
    {
        uint32_t o = le32(buf + 8 + x * 4);

        while (o + 1 < size && buf[o] != 0xFF)
        {
            int top = buf[o], len = buf[o + 1], j;

            if (o + 3 + len > size)
                break;
            for (j = 0; j < len && top + j < h; ++j)
                out[(top + j) * 320 + x] = buf[o + 3 + j];
            o += len + 4;
        }
    }
    *pw = w;
    *ph = h;
    return 1;
}

// Builds the greyscale cover for wad_names[idx]; NULL if it has no title picture.
static uint8_t *load_cover(int idx)
{
    PlaydateAPI *pd = pd_glue_api();
    char path[WAD_NAME_LEN + sizeof(WAD_DIR)];
    uint8_t lum[256], *lump = NULL, *idxbuf = NULL, *cover = NULL;
    uint8_t pal[768];
    lump_t pic = {0, 0}, palette = {0, 0};
    SDFile *f;
    int w, h, x, y, i, ok = 0;

    snprintf(path, sizeof(path), WAD_DIR "/%s", wad_names[idx]);
    f = pd->file->open(path, kFileRead | kFileReadData);
    if (f == NULL)
        return NULL;

    if (find_lumps(pd, f, &pic, &palette) && pic.size <= 128 * 1024)
    {
        lump = malloc(pic.size);
        idxbuf = malloc(320 * 200);
        cover = malloc(COVER_W * COVER_H);

        if (lump && idxbuf && cover && read_lump(pd, f, pic, lump)
            && decode_pic(lump, pic.size, idxbuf, &w, &h))
        {
            // Palette index -> luma (falls back to the index itself).
            if (palette.size >= 768 && read_lump(pd, f, (lump_t){palette.pos, 768}, pal))
                for (i = 0; i < 256; ++i)
                    lum[i] = (uint8_t)((pal[i * 3] * 77 + pal[i * 3 + 1] * 150 + pal[i * 3 + 2] * 29) >> 8);
            else
                for (i = 0; i < 256; ++i)
                    lum[i] = (uint8_t)i;
            ok = 1;
        }
    }
    pd->file->close(f);

    if (ok)
    {
        int hist[256] = {0}, lo = 0, hi = 255, acc;

        for (y = 0; y < COVER_H; ++y)
        {
            int sy0 = y * h / COVER_H, sy1 = (y + 1) * h / COVER_H;

            if (sy1 <= sy0)
                sy1 = sy0 + 1;
            for (x = 0; x < COVER_W; ++x)
            {
                int sx0 = x * w / COVER_W, sx1 = (x + 1) * w / COVER_W;
                int sx, sy, sum = 0, n = 0;

                if (sx1 <= sx0)
                    sx1 = sx0 + 1;
                for (sy = sy0; sy < sy1; ++sy)
                    for (sx = sx0; sx < sx1; ++sx, ++n)
                        sum += lum[idxbuf[sy * 320 + sx]];
                cover[y * COVER_W + x] = (uint8_t)(sum / n);
                hist[sum / n]++;
            }
        }

        // Stretch the 2nd..98th percentile to full range: the covers only
        // have 1 bit to show themselves with, so they need all the contrast
        // they can get.
        for (acc = 0; lo < 255 && acc + hist[lo] < COVER_W * COVER_H / 50; ++lo)
            acc += hist[lo];
        for (acc = 0; hi > 0 && acc + hist[hi] < COVER_W * COVER_H / 50; --hi)
            acc += hist[hi];
        if (hi > lo + 8)
            for (i = 0; i < COVER_W * COVER_H; ++i)
            {
                int v = (cover[i] - lo) * 255 / (hi - lo);

                cover[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
    }
    else
    {
        free(cover);
        cover = NULL;
    }

    free(lump);
    free(idxbuf);
    return cover;
}

// Loads at most one missing cover per call, nearest to the selection first,
// among those close enough to be on screen. Returns 1 if it loaded one.
static int load_next_cover(void)
{
    int r, s;

    for (r = 0; r <= FLOW_VISIBLE; ++r)
        for (s = -1; s <= 1; s += 2)
        {
            int i = wad_selected + s * r;

            if (i < 0 || i >= wad_count || wad_cover_state[i] != COVER_PENDING)
                continue;
            wad_cover[i] = load_cover(i);
            wad_cover_state[i] = wad_cover[i] ? COVER_LOADED : COVER_NONE;
            return 1;
        }
    return 0;
}

static void free_covers(void)
{
    int i;

    for (i = 0; i < MAX_WADS; ++i)
    {
        free(wad_cover[i]);
        wad_cover[i] = NULL;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void draw_text_line(const char *msg, int y, int selected)
{
    PlaydateAPI *pd = pd_glue_api();
    int w = (int) strlen(msg) * 9 + 16;
    int x = (SCREEN_W - w) / 2;

    if (selected)
    {
        pd->graphics->fillRect(x, y - 2, w, 20, kColorWhite);
        pd->graphics->setDrawMode(kDrawModeFillBlack);
    }
    else
    {
        pd->graphics->setDrawMode(kDrawModeFillWhite);
    }
    pd->graphics->drawText(msg, strlen(msg), kASCIIEncoding, x + 8, y);
}

void dgpd_wad_DrawMessage(const char *msg)
{
    pd_glue_api()->graphics->clear(kColorBlack);
    draw_text_line(msg, 112, 0);
}

// 8x8 Bayer ordered-dither thresholds (0-63), built from the 4x4 one.
static uint8_t bayer8[8][8];

static int bayer_ready;

static void init_bayer(void)
{
    static const uint8_t b4[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };
    static const uint8_t b2[2][2] = {{0, 2}, {3, 1}};
    int x, y;

    for (y = 0; y < 8; ++y)
        for (x = 0; x < 8; ++x)
            bayer8[y][x] = (uint8_t)(b4[y & 3][x & 3] * 4 + b2[y >> 2][x >> 2]);
    bayer_ready = 1;
}

static void put_pixel(uint8_t *frame, int x, int y, int grey)
{
    uint8_t *p = frame + y * ROW_BYTES + (x >> 3);
    uint8_t mask = (uint8_t)(0x80 >> (x & 7));

    if (grey > bayer8[y & 7][x & 7] * 4 + 2)
        *p |= mask;
    else
        *p &= (uint8_t)~mask;
}

// Draws cover i at (fractional) distance d from the selected one.
static void draw_cover(uint8_t *frame, int i, float d)
{
    const uint8_t *cover = wad_cover[i];
    float a = fabsf(d), sgn = d < 0 ? -1.0f : 1.0f;
    float turn = (a < 1.0f ? d : sgn) * -FLOW_ANGLE;
    float sinT = sinf(turn), cosT = cosf(turn);
    float cx = (a < 1.0f ? d * FLOW_GAP : sgn * (FLOW_GAP + (a - 1.0f) * FLOW_STEP));
    float back = (a < 1.0f ? a : 1.0f) * FLOW_BACK;
    float depth0 = FLOW_D + back; // distance to the cover's centre
    float shade = 1.0f - 0.45f * (a < 1.0f ? a : 1.0f);
    float xa = FLOW_D * (cx - FLOW_W * 0.5f * cosT) / (depth0 - FLOW_W * 0.5f * sinT);
    float xb = FLOW_D * (cx + FLOW_W * 0.5f * cosT) / (depth0 + FLOW_W * 0.5f * sinT);
    int x0, x1, sx;

    x0 = (int)floorf((xa < xb ? xa : xb) + SCREEN_W / 2);
    x1 = (int)ceilf((xa < xb ? xb : xa) + SCREEN_W / 2);
    if (x0 < 0)
        x0 = 0;
    if (x1 > SCREEN_W - 1)
        x1 = SCREEN_W - 1;

    for (sx = x0; sx <= x1; ++sx)
    {
        // Undo the perspective projection: which point along the cover is
        // seen through this screen column?
        float s = (sx + 0.5f - SCREEN_W / 2) / FLOW_D;
        float u = (cx - s * depth0) / (s * sinT - cosT);
        float half, ytop, vstep, v;
        int col, y, ya, yb, refl;

        if (u < -FLOW_W * 0.5f || u >= FLOW_W * 0.5f)
            continue;

        half = FLOW_H * 0.5f * FLOW_D / (depth0 + u * sinT);
        col = (int)((u / FLOW_W + 0.5f) * COVER_W);
        if (col > COVER_W - 1)
            col = COVER_W - 1;
        ytop = FLOW_YC - half;
        vstep = COVER_H / (2.0f * half);

        ya = (int)ceilf(ytop);
        yb = (int)floorf(FLOW_YC + half);
        refl = (int)half; // reflection is half a cover tall

        // Cover, then its mirror image fading out below.
        v = (ya - ytop) * vstep;
        for (y = ya; y < yb + refl && y < SCREEN_H; ++y)
        {
            int row, g;

            if (y < 0)
            {
                v += vstep;
                continue;
            }
            if (y < yb)
            {
                row = (int)v;
                if (row > COVER_H - 1)
                    row = COVER_H - 1;
                g = cover ? cover[row * COVER_W + col] : 60 + (((col ^ row) >> 3) & 1) * 30;
                g = (int)(g * shade);
            }
            else
            {
                float fade = 1.0f - (float)(y - yb) / refl;

                row = COVER_H - 1 - (int)((y - yb) * vstep);
                if (row < 0)
                    row = 0;
                g = cover ? cover[row * COVER_W + col] : 60 + (((col ^ row) >> 3) & 1) * 30;
                g = (int)(g * shade * FLOW_MIRROR * fade * fade);
            }
            // Dark seam at the edges so overlapping covers stay distinguishable.
            put_pixel(frame, sx, y, sx == x0 || sx == x1 ? 0 : g);
            v += vstep;
        }
    }
}

static const char *wad_title(int idx, char *buf, size_t buflen)
{
    size_t i, len;

    for (i = 0; i < sizeof(wad_titles) / sizeof(wad_titles[0]); ++i)
        if (strcasecmp(wad_names[idx], wad_titles[i].file) == 0)
            return wad_titles[i].title;

    snprintf(buf, buflen, "%s", wad_names[idx]);
    len = strlen(buf);
    if (len > 4)
        buf[len - 4] = '\0'; // drop ".wad"
    return buf;
}

static void draw_flow(void)
{
    PlaydateAPI *pd = pd_glue_api();
    int order[2 * FLOW_VISIBLE + 2], n = 0, i, j;
    char title[WAD_NAME_LEN], counter[16];
    const char *name = wad_title(wad_selected, title, sizeof(title));
    uint8_t *frame;

    pd->graphics->clear(kColorBlack);
    frame = pd->graphics->getFrame();

    // Farthest from the carousel's position first, so nearer covers overlap
    // their neighbours.
    for (i = 0; i < wad_count; ++i)
        if (fabsf(i - flow_pos) < FLOW_VISIBLE + 0.99f)
            order[n++] = i;
    for (i = 1; i < n; ++i)
    {
        int cur = order[i];

        for (j = i - 1; j >= 0 && fabsf(order[j] - flow_pos) < fabsf(cur - flow_pos); --j)
            order[j + 1] = order[j];
        order[j + 1] = cur;
    }
    for (i = 0; i < n; ++i)
        draw_cover(frame, order[i], order[i] - flow_pos);

    pd->graphics->markUpdatedRows(0, SCREEN_H - 1);

    snprintf(counter, sizeof(counter), "%d/%d", wad_selected + 1, wad_count);
    draw_text_line("Select WAD", 2, 0);
    pd->graphics->drawText(counter, strlen(counter), kASCIIEncoding,
                           SCREEN_W - (int)strlen(counter) * 9 - 10, 4);
    draw_text_line(name, 196, 0);
    draw_text_line(wad_names[wad_selected], 216, 0);
}

// ---------------------------------------------------------------------------
// Input + per-frame update
// ---------------------------------------------------------------------------

// Returns how many covers the player moved (negative: left), and whether A
// was pressed in *chosen.
static int read_step(int *chosen)
{
    PlaydateAPI *pd = pd_glue_api();
    PDButtons cur, pushed, released;
    int dir = 0, step = 0;

    pd->system->getButtonState(&cur, &pushed, &released);
    *chosen = (pushed & kButtonA) != 0;

    if (pushed & (kButtonLeft | kButtonUp))
        step = -1;
    else if (pushed & (kButtonRight | kButtonDown))
        step = 1;

    if (step != 0)
    {
        held_dir = step;
        held_frames = 0;
    }
    else
    {
        if (cur & (kButtonLeft | kButtonUp))
            dir = -1;
        else if (cur & (kButtonRight | kButtonDown))
            dir = 1;

        if (dir != 0 && dir == held_dir)
        {
            if (++held_frames >= REPEAT_DELAY && (held_frames - REPEAT_DELAY) % REPEAT_RATE == 0)
                step = dir;
        }
        else
        {
            held_dir = dir;
            held_frames = 0;
        }
    }

    crank_accum += pd->system->getCrankChange();
    while (crank_accum >= CRANK_STEP)
    {
        crank_accum -= CRANK_STEP;
        step++;
    }
    while (crank_accum <= -CRANK_STEP)
    {
        crank_accum += CRANK_STEP;
        step--;
    }
    return step;
}

int dgpd_wad_Update(void)
{
    int step, target, chosen;
    float diff;

    if (wad_count == 0)
    {
        dgpd_wad_DrawMessage("No WAD found in wad folder");
        return 0;
    }
    if (wad_count == 1)
        return 1;

    if (!bayer_ready)
        init_bayer();

    step = read_step(&chosen);
    target = wad_selected + step;
    if (target < 0)
        target = 0;
    if (target >= wad_count)
        target = wad_count - 1;
    if (target != wad_selected)
    {
        wad_selected = target;
        flow_dirty = 1;
    }

    diff = wad_selected - flow_pos;
    if (diff != 0.0f)
    {
        flow_pos = fabsf(diff) < 0.02f ? (float)wad_selected : flow_pos + diff * 0.35f;
        flow_dirty = 1;
    }
    if (load_next_cover())
        flow_dirty = 1;

    if (chosen)
    {
        free_covers();
        return 1;
    }

    if (flow_dirty)
    {
        draw_flow();
        flow_dirty = 0;
    }
    return 0;
}

const char *dgpd_wad_Path(void)
{
    snprintf(wad_path, sizeof(wad_path), WAD_DIR "/%s", wad_names[wad_selected]);
    return wad_path;
}
