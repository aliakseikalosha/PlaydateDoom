// 1-bit dithering for the Playdate's display. Kept separate from the
// Playdate glue (doomgeneric_playdate.c) so the dither algorithms can be
// tuned without touching input/timing/frame-buffer code.
//
// Five modes are available, selected from Doom's own Options menu
// (ditherMode, owned by m_menu.c like detailLevel): the default 2x2 banded
// ordered dither (see dither_mask2x2 below); a random threshold dither
// that compares each colour against a cutoff jittered by noise (see
// rand_next), cut on average at the midpoint between the current
// palette's darkest and brightest colour (see build_luma) so it tracks
// Doom's palette shifts (e.g. the red damage flash) automatically, with
// the noise drawn from a PRNG separate from Doom's own (rendering must
// never consume the game's random table - that would desync demos and
// netgames); a finer 4x4 Bayer ordered dither (see bayer4x4 below); a
// 16x16 blue noise ordered dither (see blue_noise16x16 below), which has
// no repeating grid structure visible to the eye like the Bayer patterns
// do; or a Floyd-Steinberg error-diffusion threshold (see
// convert_error_diffusion below) that spreads each pixel's quantization
// error to its not-yet-visited neighbours in the same frame. Whichever mode
// is active is used everywhere - 3D view, automap, status bar, messages,
// menus.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "doomgeneric.h"
#include "i_video.h"

#include "dgpd_dither.h"

// Kept in sync with m_menu.c's ditherMode comment and Options menu labels.
enum
{
    DITHER_ORDERED_2X2,
    DITHER_RANDOM_THRESHOLD,
    DITHER_ORDERED_4X4,
    DITHER_BLUE_NOISE,
    DITHER_ERROR_DIFFUSION,
};

extern int ditherMode; // Options menu: which of the modes above is active

// 2x2 ordered-dither mask per intensity band (0-19%, 20-39%, 40-59%,
// 60-88%, 89-100%). Bits are corners TL,TR,BL,BR (MSB to LSB), e.g.
// 0x8 = "10/00" (only the top-left corner lit). A pixel's colour is decided
// by whether its own screen position, taken mod 2 in x and y, lands on a lit
// corner of its colour's mask.
static const uint8_t dither_mask2x2[5] = {0x0, 0x8, 0x9, 0x7, 0xF};

// Standard 4x4 Bayer threshold matrix (values 0-15). A pixel is white iff
// its colour's 0-15 intensity level (see level16 below) is greater than
// the matrix entry at its position, taken mod 4 in x and y.
static const uint8_t bayer4x4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5},
};

// 16x16 blue noise threshold matrix (values 0-255, each used exactly once),
// generated offline via Ulichney's void-and-cluster method (toroidal
// Gaussian energy field, sigma 1.5) - see gen_blue_noise.py. Unlike the
// Bayer matrices above, blue noise has no low-frequency structure, so its
// tiling doesn't show up as a repeating grid. A pixel is white iff its
// colour's 0-255 intensity (see level256 below) is greater than the matrix
// entry at its position, taken mod 16 in x and y.
static const uint8_t blue_noise16x16[16][16] = {
    {143, 221,  41, 239,  94,  51, 216,  99,  74, 161, 248,  24, 190, 135,   2, 249},
    { 27, 125,  75,   4, 160, 191,  30, 124, 224,  49, 138,  90, 219,  48, 172,  65},
    { 98, 171, 215, 140, 253,  67, 205, 152,   3, 179, 203,  14, 126, 105, 207, 231},
    { 11, 192,  57, 104,  23, 117,  87, 237,  60, 110,  81, 229, 165,  29,  83, 150},
    {119, 245,  34, 201, 149, 183,  40, 132, 167, 244,  37, 145,  64, 252, 181,  46},
    { 72, 159,  95, 234,  63, 223,   7, 214,  97,  17, 189, 116, 202,   5, 133, 220},
    { 19, 137, 177,  15, 122, 170,  84,  55, 195, 158,  76, 225,  53,  91, 108, 198},
    {235,  54, 217,  78,  43, 241, 141, 112, 251,  44, 129,  26, 169, 238, 154,  39},
    { 88, 128, 193, 107, 156, 196,  28, 209,  12, 146, 185, 103, 204,  16,  69, 180},
    {  1, 163,  31, 254,   6,  92,  62, 173,  80, 228,  59, 243,  82, 148, 121, 247},
    { 96, 227,  70, 120, 212, 134, 232, 118, 155, 100,  32, 136,  22, 222,  47, 206},
    { 36, 139, 182,  50, 164,  21, 188,  42,   0, 218, 197, 166, 113, 187,  66, 153},
    {106, 210,   9,  85, 240, 102,  77, 250, 175, 127,  71,  45, 255,  86,  13, 174},
    { 73, 246, 123, 199, 147,  35, 208, 142,  56,  93, 230,   8, 144, 200, 130, 233},
    { 18, 157,  58,  25, 226,  68, 168, 111,  20, 211, 151, 178, 101,  33, 213,  52},
    {194,  89, 184, 114, 176, 131,  10, 242, 186,  38, 115,  61, 236,  79, 162, 109},
};

static uint8_t level[256];    // intensity band (0-4) per palette colour, for DITHER_ORDERED_2X2
static uint8_t level16[256];  // intensity level (0-15) per palette colour, for DITHER_ORDERED_4X4
static uint8_t level256[256]; // intensity (0-255) per palette colour, for DITHER_BLUE_NOISE
static uint8_t pct[256];      // luma (0-100) per palette colour, for the threshold modes
static int base_threshold;    // threshold modes' average cutoff (DITHER_RANDOM_THRESHOLD
                               // jitters it with noise; DITHER_ERROR_DIFFUSION diffuses the
                               // rounding error to neighbouring pixels instead)

// +/- spread (percentage points) of the noise added to base_threshold per
// pixel in DITHER_RANDOM_THRESHOLD, so flat areas get grain instead of a
// hard edge.
#define THRESHOLD_NOISE 16

// Error-diffusion scratch rows (with a 1-pixel guard on each side) holding
// the quantization error pushed onto the current and next row, in 1/16ths of
// a luma percentage point. Rebuilt from zero every frame, so no error ever
// carries from one frame to the next.
static int32_t diffusion_err[2][DOOMGENERIC_RESX + 2];

// Small PRNG for rendering noise only, independent of Doom's own (M_Random
// et al feed demo/netgame determinism and must not be touched here).
static uint32_t rand_state = 0x9e3779b9u;

static inline uint32_t rand_next(void)
{
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;
    return rand_state;
}

static void build_luma(void)
{
    int i;
    int min_pct = 100, max_pct = 0;

    for (i = 0; i < 256; i++)
    {
        int y = (colors[i].r * 77 + colors[i].g * 150 + colors[i].b * 29) >> 8;

        // Doom's lighting is dark for a 1-bit panel; lift the mid-tones.
        y = (y + (int) sqrtf((float) (y * 255))) >> 1;

        pct[i] = (uint8_t) (y * 100 / 255);
        level[i] = (uint8_t) (pct[i] < 20 ? 0 : pct[i] < 40 ? 1 : pct[i] < 60 ? 2 : pct[i] < 89 ? 3 : 4);
        level16[i] = (uint8_t) (pct[i] * 16 / 100 > 15 ? 15 : pct[i] * 16 / 100);
        level256[i] = (uint8_t) y;

        if (pct[i] < min_pct)
            min_pct = pct[i];
        if (pct[i] > max_pct)
            max_pct = pct[i];
    }

    // Threshold mode's cutoff follows the current palette instead of a fixed
    // value: the midpoint between its darkest and brightest colour, so the
    // black/white split stays balanced across Doom's palette shifts (e.g.
    // the red damage flash).
    base_threshold = (min_pct + max_pct) / 2;
}

static void convert_ordered_2x2(uint8_t *dst, int dst_stride, const uint8_t *src)
{
    int y, bx;

    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;
        // A pixel is white iff (x%2, y%2) lands on a lit corner of its
        // colour's mask: row y%2 picks TL/TR (y even) or BL/BR (y odd),
        // column x%2 then picks the left (even x) or right (odd x) bit.
        int fine_y = y & 1;
        uint8_t bitEvenX = fine_y ? 0x2 : 0x8; // BL : TL
        uint8_t bitOddX  = fine_y ? 0x1 : 0x4; // BR : TR

        for (bx = 0; bx < DOOMGENERIC_RESX / 8; bx++)
        {
            row[bx] = (uint8_t) ((dither_mask2x2[level[src[0]]] & bitEvenX ? 0x80 : 0) |
                                 (dither_mask2x2[level[src[1]]] & bitOddX  ? 0x40 : 0) |
                                 (dither_mask2x2[level[src[2]]] & bitEvenX ? 0x20 : 0) |
                                 (dither_mask2x2[level[src[3]]] & bitOddX  ? 0x10 : 0) |
                                 (dither_mask2x2[level[src[4]]] & bitEvenX ? 0x08 : 0) |
                                 (dither_mask2x2[level[src[5]]] & bitOddX  ? 0x04 : 0) |
                                 (dither_mask2x2[level[src[6]]] & bitEvenX ? 0x02 : 0) |
                                 (dither_mask2x2[level[src[7]]] & bitOddX  ? 0x01 : 0));
            src += 8;
        }
    }
}

static void convert_random_threshold(uint8_t *dst, int dst_stride, const uint8_t *src)
{
    int y, bx;

    // Each pixel compares its colour's luma against base_threshold jittered
    // by noise, so flat-coloured areas dither into grain instead of banding
    // at a hard edge.
    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;

        for (bx = 0; bx < DOOMGENERIC_RESX / 8; bx++)
        {
            uint8_t bits = 0;
            int b;

            for (b = 0; b < 8; b++)
            {
                int noise = (int) (rand_next() % (2 * THRESHOLD_NOISE + 1)) - THRESHOLD_NOISE;

                if (pct[src[b]] >= base_threshold + noise)
                    bits |= (uint8_t) (0x80 >> b);
            }

            row[bx] = bits;
            src += 8;
        }
    }
}

static void convert_ordered_4x4(uint8_t *dst, int dst_stride, const uint8_t *src)
{
    int y, bx;

    // A pixel is white iff its colour's 0-15 intensity level is greater
    // than the Bayer matrix entry at its position mod 4 in x and y - finer
    // grained than the 2x2 dither, at the cost of a coarser repeat pattern.
    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;
        const uint8_t *matrix_row = bayer4x4[y & 3];

        for (bx = 0; bx < DOOMGENERIC_RESX / 8; bx++)
        {
            uint8_t bits = 0;
            int c;

            for (c = 0; c < 8; c++)
                if (level16[src[c]] > matrix_row[c & 3])
                    bits |= (uint8_t) (0x80 >> c);

            row[bx] = bits;
            src += 8;
        }
    }
}

static void convert_blue_noise(uint8_t *dst, int dst_stride, const uint8_t *src)
{
    int y, bx;

    // A pixel is white iff its colour's 0-255 intensity is greater than the
    // blue noise matrix entry at its position mod 16 in x and y. Column c
    // of byte bx sits at x = bx*8+c, so x mod 16 is c offset by 8 on every
    // other byte (8 being half of 16) rather than always starting at 0.
    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;
        const uint8_t *matrix_row = blue_noise16x16[y & 15];

        for (bx = 0; bx < DOOMGENERIC_RESX / 8; bx++)
        {
            int x0 = (bx & 1) << 3; // 0 or 8
            uint8_t bits = 0;
            int c;

            for (c = 0; c < 8; c++)
                if (level256[src[c]] > matrix_row[x0 + c])
                    bits |= (uint8_t) (0x80 >> c);

            row[bx] = bits;
            src += 8;
        }
    }
}

static void convert_error_diffusion(uint8_t *dst, int dst_stride, const uint8_t *src)
{
    int y, x;
    int32_t *cur = diffusion_err[0] + 1;
    int32_t *next = diffusion_err[1] + 1;

    memset(diffusion_err, 0, sizeof(diffusion_err));

    // Floyd-Steinberg on the threshold: each pixel adds the error handed to
    // it by its neighbours to its own luma, is shown as white (100) or black
    // (0) by comparing against base_threshold, and passes the leftover
    // (what it asked for minus what it could show) on: 7/16 to the next
    // pixel in scan order, 3/16, 5/16 and 1/16 to the row below. Rows
    // alternate direction (serpentine) so the grain doesn't streak.
    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;
        int ltr = !(y & 1);
        int dir = ltr ? 1 : -1;

        memset(row, 0, DOOMGENERIC_RESX / 8);
        memset(next - 1, 0, (DOOMGENERIC_RESX + 2) * sizeof(*next));

        for (x = ltr ? 0 : DOOMGENERIC_RESX - 1; x >= 0 && x < DOOMGENERIC_RESX; x += dir)
        {
            int adjusted = pct[src[y * DOOMGENERIC_RESX + x]] * 16 + cur[x];
            int white = adjusted >= base_threshold * 16;
            int err = adjusted - (white ? 100 * 16 : 0);

            if (white)
                row[x >> 3] |= (uint8_t) (0x80 >> (x & 7));

            cur[x + dir] += err * 7 / 16;
            next[x - dir] += err * 3 / 16;
            next[x] += err * 5 / 16;
            next[x + dir] += err / 16;
        }

        {
            int32_t *tmp = cur;

            cur = next;
            next = tmp;
        }
    }
}

void dgpd_dither_ConvertFrame(uint8_t *dst, int dst_stride)
{
    const uint8_t *src = (const uint8_t *) DG_ScreenBuffer;

    if (palette_changed)
    {
        build_luma();
        palette_changed = false;
    }

    switch (ditherMode)
    {
        case DITHER_RANDOM_THRESHOLD:
            convert_random_threshold(dst, dst_stride, src);
            break;

        case DITHER_ORDERED_4X4:
            convert_ordered_4x4(dst, dst_stride, src);
            break;

        case DITHER_BLUE_NOISE:
            convert_blue_noise(dst, dst_stride, src);
            break;

        case DITHER_ERROR_DIFFUSION:
            convert_error_diffusion(dst, dst_stride, src);
            break;

        default:
            convert_ordered_2x2(dst, dst_stride, src);
            break;
    }
}
