// 1-bit dithering for the Playdate's display. Kept separate from the
// Playdate glue (doomgeneric_playdate.c) so the dither algorithms can be
// tuned without touching input/timing/frame-buffer code.
//
// Six modes are available, selected from Doom's own Options menu
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
// error to its not-yet-visited neighbours in the same frame; or the 2x2
// ordered dither again, but with the error left over from snapping each
// pixel to one of its five intensity bands carried to its neighbours the same
// way (see convert_ordered_2x2_carry below), so the bands blend instead of
// showing hard edges. Whichever mode is active is used everywhere - 3D view,
// automap, status bar, messages, menus.
//
// These loops run over 76,800 pixels every frame on an in-order Cortex-M7, so
// they are written around what that core is slow at rather than around the
// shortest source: the 8-bit frame is read a word (4 pixels) at a time; per-
// colour decisions come from lookup tables rebuilt only when the palette or
// mode changes (see build_luma and build_mode_tables) instead of being
// recomputed per pixel; each output byte is assembled in a register and
// stored once; and the error-diffusion carries live in registers rather than
// being read-modify-written in the scratch rows (see fs_spread).
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
    DITHER_ORDERED_2X2_CARRY,
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
static int16_t pct16[256];    // the same in 1/16ths of a percentage point, for the diffusion modes
static int base_threshold;    // threshold modes' average cutoff (DITHER_RANDOM_THRESHOLD
                               // jitters it with noise; DITHER_ERROR_DIFFUSION diffuses the
                               // rounding error to neighbouring pixels instead)

// Tables derived from the above for one specific mode, so that mode's inner
// loop does a single lookup per pixel instead of a chain of them. Only the
// active mode's tables are built (see build_mode_tables), and they are
// rebuilt when the palette or the mode changes.
//
// lit_tab[y & 3][x & 3] points at a 256-entry table answering "is this colour
// white at this position of the repeating dither pattern?" (0 or 1), for the
// ordered modes whose pattern repeats every 2 or 4 pixels. A 2x2 pattern
// points several cells at the same table (lit_store[0-3]); 4x4 has its own
// table for each of the 16 cells (lit_store[0-15]).
static uint8_t lit_store[16][256];
static const uint8_t *lit_tab[4][4];

// DITHER_RANDOM_THRESHOLD: a colour is white iff its entry here is greater
// than a random byte, i.e. with probability random_thr / 256 (0-256, so it
// can be "never" and "always").
static uint16_t random_thr[256];

// DITHER_ORDERED_2X2_CARRY: cell_lit[k][a >> 3] is 1 if corner k (TL,TR,BL,BR)
// of a 2x2 cell is lit when its pixel's adjusted luma is a (0-1600, in 1/16ths
// of a percentage point), i.e. when a falls in a band whose mask lights that
// corner - see build_mode_tables. The 25%-wide bands start on multiples of 8,
// so a >> 3 loses nothing.
static uint8_t cell_lit[4][100 * 16 / 8 + 1];

static int tables_for;       // ditherMode the tables above were built for
static int tables_valid;     // 0 after a palette change, until they are rebuilt

// +/- spread (percentage points) of the noise base_threshold is jittered by
// per pixel in DITHER_RANDOM_THRESHOLD, so flat areas get grain instead of a
// hard edge.
#define THRESHOLD_NOISE 16

// Error-diffusion scratch rows (with a 1-pixel guard on each side) holding
// the quantization error pushed onto the current and next row, in 1/16ths of
// a luma percentage point. Shared by DITHER_ERROR_DIFFUSION and
// DITHER_ORDERED_2X2_CARRY. Only the first row is cleared each frame (every
// other row is written in full by the row before it), so no error ever
// carries from one frame to the next.
static int32_t diffusion_err[2][DOOMGENERIC_RESX + 2];

// Small PRNG for rendering noise only, independent of Doom's own (M_Random
// et al feed demo/netgame determinism and must not be touched here).
static uint32_t rand_state = 0x9e3779b9u;

static inline uint32_t rand_step(uint32_t s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

#define ROW_BYTES (DOOMGENERIC_RESX / 8)

// Several of the helpers below are written as functions so each step of an
// algorithm reads as one thing, but must compile to straight-line code with
// their state in registers; the compiler's own call-or-not heuristic isn't
// trusted with that.
#define FORCE_INLINE static inline __attribute__((always_inline))

// Reads 4 pixels as one word instead of four byte loads; PIX(w, i) is the
// i-th (0-3) of them in screen order.
FORCE_INLINE uint32_t load4(const uint8_t *p)
{
    uint32_t w;

    memcpy(&w, p, sizeof w);
    return w;
}

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PIX(w, i) (((w) >> (24 - 8 * (i))) & 0xFF)
#else
#define PIX(w, i) (((w) >> (8 * (i))) & 0xFF)
#endif

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
        pct16[i] = (int16_t) (pct[i] * 16);
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

static void build_mode_tables(int mode)
{
    int i, c;

    switch (mode)
    {
        case DITHER_RANDOM_THRESHOLD:
            // A pixel is white iff pct >= base_threshold + noise, with noise
            // uniform over -16..16 (33 values), i.e. with probability
            // (pct - base + 17) / 33, saturating at never / always. A random
            // byte is compared against that probability scaled to 0-256.
            for (c = 0; c < 256; c++)
            {
                int d = pct[c] - base_threshold;

                random_thr[c] = (uint16_t) (d < -THRESHOLD_NOISE ? 0
                                          : d >= THRESHOLD_NOISE ? 256
                                          : ((d + THRESHOLD_NOISE + 1) * 256 + THRESHOLD_NOISE) / (2 * THRESHOLD_NOISE + 1));
            }
            break;

        case DITHER_ORDERED_4X4:
            // White iff the colour's 0-15 level is greater than the Bayer
            // matrix entry at the cell.
            for (i = 0; i < 16; i++)
            {
                for (c = 0; c < 256; c++)
                    lit_store[i][c] = level16[c] > bayer4x4[i >> 2][i & 3];

                lit_tab[i >> 2][i & 3] = lit_store[i];
            }
            break;

        case DITHER_ORDERED_2X2_CARRY:
            // Palette independent, but cheap enough to just derive here from
            // dither_mask2x2 (corner i is bit 3 - i of a mask) rather than
            // keep a second copy of it in sync by hand. A pixel snaps to the
            // nearest of the five bands, (a + 200) / 400 for a in 1/16ths of a
            // percentage point.
            for (i = 0; i < 4; i++)
                for (c = 0; c <= 100 * 16 / 8; c++)
                    cell_lit[i][c] = (uint8_t) ((dither_mask2x2[(c * 8 + 25 * 16 / 2) / (25 * 16)] >> (3 - i)) & 1);
            break;

        case DITHER_BLUE_NOISE:
        case DITHER_ERROR_DIFFUSION:
            // Work straight from level256 / pct16.
            break;

        default:
            // 2x2 ordered: a pixel is white iff (x%2, y%2) lands on a lit
            // corner of its colour's mask - cell i = y%2 * 2 + x%2 is bit
            // 3 - i, the same order as the mask's TL,TR,BL,BR. Only four
            // distinct cells, shared across the 4x4 grid.
            for (i = 0; i < 4; i++)
                for (c = 0; c < 256; c++)
                    lit_store[i][c] = (uint8_t) ((dither_mask2x2[level[c]] >> (3 - i)) & 1);

            for (i = 0; i < 16; i++)
                lit_tab[i >> 2][i & 3] = lit_store[((i >> 2) & 1) * 2 + (i & 1)];
            break;
    }
}

// DITHER_ORDERED_2X2 and DITHER_ORDERED_4X4: the pattern repeats every 2 or 4
// pixels, so a pixel's result is a single lookup in the table for its
// position (see lit_tab). Column k of every byte sits at x mod 4 == k mod 4
// because 8 is a multiple of 4, so the row's four tables are picked once.
static void convert_ordered(uint8_t *restrict dst, int dst_stride, const uint8_t *restrict src)
{
    int y, bx;

    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;
        const uint8_t *t0 = lit_tab[y & 3][0];
        const uint8_t *t1 = lit_tab[y & 3][1];
        const uint8_t *t2 = lit_tab[y & 3][2];
        const uint8_t *t3 = lit_tab[y & 3][3];

        for (bx = 0; bx < ROW_BYTES; bx++)
        {
            uint32_t a = load4(src), b = load4(src + 4);

            row[bx] = (uint8_t) ((t0[PIX(a, 0)] << 7) | (t1[PIX(a, 1)] << 6) |
                                 (t2[PIX(a, 2)] << 5) | (t3[PIX(a, 3)] << 4) |
                                 (t0[PIX(b, 0)] << 3) | (t1[PIX(b, 1)] << 2) |
                                 (t2[PIX(b, 2)] << 1) |  t3[PIX(b, 3)]);
            src += 8;
        }
    }
}

// Each pixel is white iff its table entry beats one random byte. One PRNG
// step feeds 4 pixels, and the PRNG state stays in a register for the frame.
#define RANDOM_BIT(w, i, r, j, bit) \
    if (random_thr[PIX(w, i)] > (((r) >> (8 * (j))) & 0xFF)) bits |= (bit)

static void convert_random_threshold(uint8_t *restrict dst, int dst_stride, const uint8_t *restrict src)
{
    uint32_t rnd = rand_state;
    int y, bx;

    // Each pixel compares its colour's luma against base_threshold jittered
    // by noise, so flat-coloured areas dither into grain instead of banding
    // at a hard edge.
    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;

        for (bx = 0; bx < ROW_BYTES; bx++)
        {
            uint32_t a = load4(src), b = load4(src + 4);
            uint32_t r0, r1;
            unsigned bits = 0;

            rnd = rand_step(rnd);
            r0 = rnd;
            rnd = rand_step(rnd);
            r1 = rnd;

            RANDOM_BIT(a, 0, r0, 0, 0x80);
            RANDOM_BIT(a, 1, r0, 1, 0x40);
            RANDOM_BIT(a, 2, r0, 2, 0x20);
            RANDOM_BIT(a, 3, r0, 3, 0x10);
            RANDOM_BIT(b, 0, r1, 0, 0x08);
            RANDOM_BIT(b, 1, r1, 1, 0x04);
            RANDOM_BIT(b, 2, r1, 2, 0x02);
            RANDOM_BIT(b, 3, r1, 3, 0x01);

            row[bx] = (uint8_t) bits;
            src += 8;
        }
    }

    rand_state = rnd;
}

// A pixel is white iff its colour's 0-255 intensity is greater than the blue
// noise matrix entry at its position mod 16 in x and y. 16 is two bytes of
// output, so the matrix row is loaded once per row into 4 words and the row
// walks the frame 16 pixels at a time.
#define BLUE_BIT(w, i, m, j, bit) \
    if (level256[PIX(w, i)] > PIX(m, j)) bits |= (bit)

static void convert_blue_noise(uint8_t *restrict dst, int dst_stride, const uint8_t *restrict src)
{
    int y, bx;

    _Static_assert(DOOMGENERIC_RESX % 16 == 0, "blue noise walks 16 pixels (2 bytes) at a time");

    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;
        const uint8_t *matrix_row = blue_noise16x16[y & 15];
        uint32_t m0 = load4(matrix_row), m1 = load4(matrix_row + 4);
        uint32_t m2 = load4(matrix_row + 8), m3 = load4(matrix_row + 12);

        for (bx = 0; bx < ROW_BYTES; bx += 2)
        {
            uint32_t a = load4(src), b = load4(src + 4), c = load4(src + 8), d = load4(src + 12);
            unsigned bits = 0;

            BLUE_BIT(a, 0, m0, 0, 0x80);
            BLUE_BIT(a, 1, m0, 1, 0x40);
            BLUE_BIT(a, 2, m0, 2, 0x20);
            BLUE_BIT(a, 3, m0, 3, 0x10);
            BLUE_BIT(b, 0, m1, 0, 0x08);
            BLUE_BIT(b, 1, m1, 1, 0x04);
            BLUE_BIT(b, 2, m1, 2, 0x02);
            BLUE_BIT(b, 3, m1, 3, 0x01);
            row[bx] = (uint8_t) bits;

            bits = 0;
            BLUE_BIT(c, 0, m2, 0, 0x80);
            BLUE_BIT(c, 1, m2, 1, 0x40);
            BLUE_BIT(c, 2, m2, 2, 0x20);
            BLUE_BIT(c, 3, m2, 3, 0x10);
            BLUE_BIT(d, 0, m3, 0, 0x08);
            BLUE_BIT(d, 1, m3, 1, 0x04);
            BLUE_BIT(d, 2, m3, 2, 0x02);
            BLUE_BIT(d, 3, m3, 3, 0x01);
            row[bx + 1] = (uint8_t) bits;

            src += 16;
        }
    }
}

// Hands a pixel's error err to its not-yet-visited neighbours in Floyd-
// Steinberg's weights: 7/16 to the next pixel in scan order (carry), 3/16,
// 5/16 and 1/16 to the row below, at scan positions i-1, i and i+1.
//
// Rather than read-modify-writing three cells of the next row per pixel,
// each cell of it is written exactly once, as soon as the last of its three
// contributions is known. Cell i-1 is complete once pixel i has handed over
// its 3/16, so it is stored into *slot (cell i-1) here. On entry *pend holds
// what cell i-1 had so far (1/16 from pixel i-2, 5/16 from pixel i-1) and
// *prev1 the 1/16 pixel i-1 left for cell i; on exit they hold cell i's
// 1/16 + 5/16 so far and pixel i's own 1/16. Each cell ends up with exactly
// the sum the three read-modify-writes would give. The terms are err * k / 16 truncated toward zero, spelled out as
// shifts so the one thing that makes them truncate instead of rounding down -
// adding 15 first when the error is negative - is worked out once for all
// four terms, not once each.
FORCE_INLINE void fs_spread(int32_t err, int32_t *carry, int32_t *slot, int32_t *pend, int32_t *prev1)
{
    int32_t bias = (int32_t) ((uint32_t) (err >> 31) >> 28); // 15 if err < 0, else 0

    *carry = (err * 7 + bias) >> 4;
    *slot = *pend + ((err * 3 + bias) >> 4);
    *pend = *prev1 + ((err * 5 + bias) >> 4);
    *prev1 = (err + bias) >> 4;
}

// One pixel of DITHER_ERROR_DIFFUSION: adds the error handed to it by its
// neighbours (cur_x from the row above, *carry from the previous pixel) to
// its own luma, is shown as white (100) or black (0) by comparing against
// the threshold thr16 (base_threshold in 1/16ths), and passes the leftover
// (what it asked for minus what it could show) on. Returns 1 if white.
FORCE_INLINE unsigned fs_pixel(unsigned colour, int32_t cur_x, int32_t thr16,
                               int32_t *carry, int32_t *slot, int32_t *pend, int32_t *prev1)
{
    int32_t adjusted = pct16[colour] + cur_x + *carry;
    unsigned white = adjusted >= thr16;

    fs_spread(adjusted - (white ? 100 * 16 : 0), carry, slot, pend, prev1);
    return white;
}

// Pixel p (0-7, left to right) of the current byte, whose bit in it is 7 - p.
// A row invokes these in scan order, which is p = 7..0 when it runs right to
// left.
#define FS_PX(p) \
    bits |= fs_pixel(PIX((p) < 4 ? w0 : w1, (p) & 3), cur[x + (p)], thr16, \
                     &carry, &next[x + (p) - dir], &pend, &prev1) << (7 - (p))

// One row of Floyd-Steinberg. Rows alternate direction (serpentine) so the
// grain doesn't streak; rtl is a compile-time constant at both call sites, so
// each direction compiles to its own straight-line loop.
FORCE_INLINE void fs_row(uint8_t *restrict row, const uint8_t *restrict src,
                         const int32_t *restrict cur, int32_t *restrict next,
                         int rtl, int32_t thr16)
{
    const int dir = rtl ? -1 : 1;
    int32_t carry = 0, pend = 0, prev1 = 0;
    int bx;

    for (bx = 0; bx < ROW_BYTES; bx++)
    {
        int b = rtl ? ROW_BYTES - 1 - bx : bx;
        int x = b * 8; // leftmost pixel of this byte
        uint32_t w0 = load4(src + x), w1 = load4(src + x + 4);
        unsigned bits = 0;

        if (!rtl)
        {
            FS_PX(0); FS_PX(1); FS_PX(2); FS_PX(3); FS_PX(4); FS_PX(5); FS_PX(6); FS_PX(7);
        }
        else
        {
            FS_PX(7); FS_PX(6); FS_PX(5); FS_PX(4); FS_PX(3); FS_PX(2); FS_PX(1); FS_PX(0);
        }

        row[b] = (uint8_t) bits;
    }

    // The last pixel of the row has no neighbour after it to add its 3/16.
    next[rtl ? 0 : DOOMGENERIC_RESX - 1] = pend;
}

static void convert_error_diffusion(uint8_t *restrict dst, int dst_stride, const uint8_t *restrict src)
{
    int y;
    int32_t *cur = diffusion_err[0] + 1;
    int32_t *next = diffusion_err[1] + 1;
    int32_t thr16 = base_threshold * 16;
    int32_t *tmp;

    memset(cur, 0, DOOMGENERIC_RESX * sizeof(*cur));

    // Floyd-Steinberg on the threshold (see fs_pixel, fs_spread, fs_row).
    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *row = dst + y * dst_stride;

        if (y & 1)
            fs_row(row, src, cur, next, 1, thr16);
        else
            fs_row(row, src, cur, next, 0, thr16);

        src += DOOMGENERIC_RESX;
        tmp = cur;
        cur = next;
        next = tmp;
    }
}

// Clamps a 2x2 cell pixel's adjusted luma (1/16ths of a percentage point) to
// 0-100%, so error that can't be shown anyway (pushing past pure black or
// white) doesn't leak into flat areas.
FORCE_INLINE int32_t cc_clamp(int32_t a)
{
    a &= ~(a >> 31); // negative -> 0
    return a > 100 * 16 ? 100 * 16 : a;
}

// One corner of a 2x2 cell of DITHER_ORDERED_2X2_CARRY: the pixel's colour
// plus the error `in` handed to its cell. Returns the pixel's own error - what
// it asked for minus what it lit - and ors its bit (0 or 1, shifted) into *bits.
// Each pixel is finished before the next starts so only the running error
// sum is kept live, not every pixel's intermediate values.
FORCE_INLINE int32_t cc_pixel(unsigned colour, int32_t in, const uint8_t *lit, int shift, unsigned *bits)
{
    int32_t a = cc_clamp(pct16[colour] + in);
    unsigned l = lit[a >> 3];

    *bits |= l << shift;
    return a - 100 * 16 * (int32_t) l;
}

// One 2x2 cell of DITHER_ORDERED_2X2_CARRY, given its four colours in the
// order TL,TR,BL,BR and the error cur_c handed to the cell by the row above
// (*carry is the one from the previous cell). Its lit corners are or-ed into
// top/bot (the cell's two rows of output bits) at bit `shift` and the one
// above it.
FORCE_INLINE void cc_cell(unsigned c0, unsigned c1, unsigned c2, unsigned c3, int32_t cur_c, int shift,
                          int32_t *carry, int32_t *slot, int32_t *pend, int32_t *prev1,
                          unsigned *top, unsigned *bot)
{
    int32_t in = cur_c + *carry;
    int32_t err = cc_pixel(c0, in, cell_lit[0], shift + 1, top);

    err += cc_pixel(c1, in, cell_lit[1], shift, top);
    err += cc_pixel(c2, in, cell_lit[2], shift + 1, bot);
    err += cc_pixel(c3, in, cell_lit[3], shift, bot);

    // The cell's error is what its pixels asked for minus what they actually
    // lit, averaged.
    fs_spread(err / 4, carry, slot, pend, prev1);
}

// Cell c (0-3, left to right) of byte b: a cell is two pixels wide, so it
// takes pixels 2c and 2c+1 of each of the two source rows. Plain byte loads
// rather than a word per 4 pixels as elsewhere: a cell already keeps most
// registers busy, and this came out a little shorter with fewer spills.
#define CC_CELL(c) \
    cc_cell(src0[x + 2 * (c)], src0[x + 2 * (c) + 1], src1[x + 2 * (c)], src1[x + 2 * (c) + 1], \
            cur[b * 4 + (c)], 6 - 2 * (c), &carry, &next[b * 4 + (c) - dir], &pend, &prev1, \
            &top, &bot)

// One pair of pixel rows (one row of cells) of DITHER_ORDERED_2X2_CARRY; see
// fs_row for why rtl is a constant.
FORCE_INLINE void cc_row(uint8_t *restrict top_row, uint8_t *restrict bot_row,
                         const uint8_t *restrict src0, const uint8_t *restrict src1,
                         const int32_t *restrict cur, int32_t *restrict next, int rtl)
{
    const int dir = rtl ? -1 : 1;
    int32_t carry = 0, pend = 0, prev1 = 0;
    int bx;

    for (bx = 0; bx < ROW_BYTES; bx++)
    {
        int b = rtl ? ROW_BYTES - 1 - bx : bx; // 4 cells per output byte
        int x = b * 8;
        unsigned top = 0, bot = 0;

        if (!rtl)
        {
            CC_CELL(0); CC_CELL(1); CC_CELL(2); CC_CELL(3);
        }
        else
        {
            CC_CELL(3); CC_CELL(2); CC_CELL(1); CC_CELL(0);
        }

        top_row[b] = (uint8_t) top;
        bot_row[b] = (uint8_t) bot;
    }

    next[rtl ? 0 : DOOMGENERIC_RESX / 2 - 1] = pend;
}

static void convert_ordered_2x2_carry(uint8_t *restrict dst, int dst_stride, const uint8_t *restrict src)
{
    int cy;
    int32_t *cur = diffusion_err[0] + 1;
    int32_t *next = diffusion_err[1] + 1;
    int32_t *tmp;

    memset(cur, 0, DOOMGENERIC_RESX / 2 * sizeof(*cur));

    // convert_ordered's banding with the leftover carried on, one 2x2 cell
    // at a time. Every pixel of a cell adds the error handed to the cell by
    // its neighbours to its own luma, snaps to the nearest of the five
    // coverages the 2x2 masks can show (0, 25, 50, 75, 100%) and is then lit
    // or not by that band's mask at its own screen position, exactly as in
    // the plain 2x2 mode. The cell's error - what its pixels asked for minus
    // what they actually lit, averaged - goes to the neighbouring cells with
    // Floyd-Steinberg weights, in serpentine order (see fs_row). Diffusing per
    // cell rather than per pixel keeps the error honest: a pixel's output
    // depends on its mask corner as well as its band, so an error measured
    // against the band's nominal coverage would be wrong pixel by pixel and
    // the mistakes would feed back into streaks. Each adjusted luma is
    // clamped to 0-100% first (see cc_clamp).
    for (cy = 0; cy < DOOMGENERIC_RESY / 2; cy++)
    {
        uint8_t *top_row = dst + cy * 2 * dst_stride;
        const uint8_t *src0 = src + cy * 2 * DOOMGENERIC_RESX;

        if (cy & 1)
            cc_row(top_row, top_row + dst_stride, src0, src0 + DOOMGENERIC_RESX, cur, next, 1);
        else
            cc_row(top_row, top_row + dst_stride, src0, src0 + DOOMGENERIC_RESX, cur, next, 0);

        tmp = cur;
        cur = next;
        next = tmp;
    }
}

void dgpd_dither_ConvertFrame(uint8_t *dst, int dst_stride)
{
    const uint8_t *src = (const uint8_t *) DG_ScreenBuffer;

    if (palette_changed)
    {
        build_luma();
        palette_changed = false;
        tables_valid = 0;
    }

    if (!tables_valid || tables_for != ditherMode)
    {
        build_mode_tables(ditherMode);
        tables_for = ditherMode;
        tables_valid = 1;
    }

    switch (ditherMode)
    {
        case DITHER_RANDOM_THRESHOLD:
            convert_random_threshold(dst, dst_stride, src);
            break;

        case DITHER_ORDERED_4X4:
            convert_ordered(dst, dst_stride, src);
            break;

        case DITHER_BLUE_NOISE:
            convert_blue_noise(dst, dst_stride, src);
            break;

        case DITHER_ERROR_DIFFUSION:
            convert_error_diffusion(dst, dst_stride, src);
            break;

        case DITHER_ORDERED_2X2_CARRY:
            convert_ordered_2x2_carry(dst, dst_stride, src);
            break;

        default:
            convert_ordered(dst, dst_stride, src);
            break;
    }
}
