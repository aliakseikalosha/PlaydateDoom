// doomgeneric platform layer for the Playdate (400x240, 1-bit, D-pad + A/B + crank).
//
// Video:   Doom's 320x240 8-bit frame is converted to 1 bit and centred in
//          the 400x240 display (full height; only left/right bars remain).
//          Two conversion modes are available, selected from Doom's own
//          Options menu (ditherMode, owned by m_menu.c like detailLevel):
//          the default 2x2 banded ordered dither (see dither_mask below),
//          or a random threshold dither that compares each colour against a
//          cutoff jittered by noise (see rand_next), cut on average at the
//          midpoint between the current palette's darkest and brightest
//          colour (see build_luma) so it tracks Doom's palette shifts (e.g.
//          the red damage flash) automatically. The noise is drawn from a
//          PRNG separate from Doom's own (rendering must never consume the
//          game's random table - that would desync demos and netgames).
//          Whichever mode is active is used everywhere - 3D view, automap,
//          status bar, messages, menus.
// Input:   D-pad moves/turns, A fires, B uses. The crank turns. With the crank
//          extended the D-pad's left/right strafe instead of turning. Hold B
//          and tap left/right to cycle weapons. In menus/intermissions A is
//          Enter and B is Back (or Yes/No on confirmation prompts).
#include <math.h>
#include <stdint.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_player.h"
#include "i_video.h"

#include "dgpd.h"
#include "pd_glue.h"

#define LCD_ROWBYTES 52
#define VIEW_X_BYTES ((LCD_COLUMNS / 8 - DOOMGENERIC_RESX / 8) / 2)
#define VIEW_Y ((LCD_ROWS - DOOMGENERIC_RESY) / 2)

// Doom's turning is mouse-driven when analog; one crank degree maps to this
// many mouse counts (about 1.75 degrees of turn per crank degree).
#define CRANK_TURN_SCALE 40.0f

extern boolean menuactive;
extern boolean automapactive;
extern int messageToPrint;
extern boolean messageNeedsInput;
extern int ditherMode; // Options menu: 0 = ordered dither (default), 1 = flat threshold

// ---------------------------------------------------------------- video

// 2x2 ordered-dither mask per intensity band (0-19%, 20-39%, 40-59%,
// 60-88%, 89-100%). Bits are corners TL,TR,BL,BR (MSB to LSB), e.g.
// 0x8 = "10/00" (only the top-left corner lit). A pixel's colour is decided
// by whether its own screen position, taken mod 2 in x and y, lands on a lit
// corner of its colour's mask.
static const uint8_t dither_mask[5] = {0x0, 0x8, 0x9, 0x7, 0xF};

static uint8_t level[256];  // intensity band (0-4) for each palette colour, for ditherMode == 0
static uint8_t pct[256];    // luma (0-100) for each palette colour, for ditherMode == 1
static int base_threshold;  // ditherMode == 1's average cutoff, before per-pixel noise

// +/- spread (percentage points) of the noise added to base_threshold per
// pixel in ditherMode == 1, so flat areas get grain instead of a hard edge.
#define THRESHOLD_NOISE 16

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

void DG_Init(void)
{
    PlaydateAPI *pd = pd_glue_api();

    pd->graphics->clear(kColorBlack);
}

void DG_DrawFrame(void)
{
    PlaydateAPI *pd = pd_glue_api();
    uint8_t *frame = pd->graphics->getFrame();
    const uint8_t *src = (const uint8_t *) DG_ScreenBuffer;
    int y, bx;

    if (palette_changed)
    {
        build_luma();
        palette_changed = false;
    }

    if (ditherMode)
    {
        // Random threshold: each pixel compares its colour's luma against
        // base_threshold jittered by noise, so flat-coloured areas dither
        // into grain instead of banding at a hard edge.
        for (y = 0; y < DOOMGENERIC_RESY; y++)
        {
            uint8_t *dst = frame + (VIEW_Y + y) * LCD_ROWBYTES + VIEW_X_BYTES;

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

                dst[bx] = bits;
                src += 8;
            }
        }
        pd->graphics->markUpdatedRows(VIEW_Y, VIEW_Y + DOOMGENERIC_RESY - 1);
        return;
    }

    for (y = 0; y < DOOMGENERIC_RESY; y++)
    {
        uint8_t *dst = frame + (VIEW_Y + y) * LCD_ROWBYTES + VIEW_X_BYTES;
        // A pixel is white iff (x%2, y%2) lands on a lit corner of its
        // colour's mask: row y%2 picks TL/TR (y even) or BL/BR (y odd),
        // column x%2 then picks the left (even x) or right (odd x) bit.
        int fine_y = y & 1;
        uint8_t bitEvenX = fine_y ? 0x2 : 0x8; // BL : TL
        uint8_t bitOddX  = fine_y ? 0x1 : 0x4; // BR : TR

        for (bx = 0; bx < DOOMGENERIC_RESX / 8; bx++)
        {
            dst[bx] = (uint8_t) ((dither_mask[level[src[0]]] & bitEvenX ? 0x80 : 0) |
                                 (dither_mask[level[src[1]]] & bitOddX  ? 0x40 : 0) |
                                 (dither_mask[level[src[2]]] & bitEvenX ? 0x20 : 0) |
                                 (dither_mask[level[src[3]]] & bitOddX  ? 0x10 : 0) |
                                 (dither_mask[level[src[4]]] & bitEvenX ? 0x08 : 0) |
                                 (dither_mask[level[src[5]]] & bitOddX  ? 0x04 : 0) |
                                 (dither_mask[level[src[6]]] & bitEvenX ? 0x02 : 0) |
                                 (dither_mask[level[src[7]]] & bitOddX  ? 0x01 : 0));
            src += 8;
        }
    }

    pd->graphics->markUpdatedRows(VIEW_Y, VIEW_Y + DOOMGENERIC_RESY - 1);
}

// ---------------------------------------------------------------- timing

void DG_SleepMs(uint32_t ms)
{
    // Doom calls this while waiting for the next tic. Spin: the Playdate has
    // no sleep, and the wait is at most one tic (~28 ms).
    uint32_t start = pd_glue_api()->system->getCurrentTimeMilliseconds();

    while (pd_glue_api()->system->getCurrentTimeMilliseconds() - start < ms)
    {
    }
}

uint32_t DG_GetTicksMs(void)
{
    return pd_glue_api()->system->getCurrentTimeMilliseconds();
}

void DG_SetWindowTitle(const char *title)
{
    (void) title;
}

// ---------------------------------------------------------------- input

#define KEYQUEUE_SIZE 32

static unsigned short key_queue[KEYQUEUE_SIZE];
static unsigned int key_write, key_read;

static void queue_key(int pressed, unsigned char key)
{
    key_queue[key_write] = (unsigned short) ((pressed << 8) | key);
    key_write = (key_write + 1) % KEYQUEUE_SIZE;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    unsigned short data;

    if (key_read == key_write)
        return 0;

    data = key_queue[key_read];
    key_read = (key_read + 1) % KEYQUEUE_SIZE;
    *pressed = data >> 8;
    *key = data & 0xff;
    return 1;
}

// Physical buttons in the order they are tracked below.
enum { BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_B, BTN_A, BTN_COUNT };

static const PDButtons button_bit[BTN_COUNT] = {
    kButtonLeft, kButtonRight, kButtonUp, kButtonDown, kButtonB, kButtonA,
};

static unsigned char held_key[BTN_COUNT]; // key sent on press, released with the button
static int b_down;                        // B is held in gameplay (weapon chord modifier)
static int b_chorded;                     // a chord happened while B was held
static int use_release_pending;           // release KEY_USE on the next frame
static int run_held;                      // KEY_RSHIFT currently held for always-run
static int always_run = 1;
static unsigned char pending_menu_key;    // from the system menu

static int in_gameplay(void)
{
    return gamestate == GS_LEVEL && !menuactive && !demoplayback;
}

// The intermission/stats screen isn't a menu: it advances only when
// WI_checkForAccelerate() sees BT_ATTACK/BT_USE in the tic command, which
// requires the gameplay button mapping (A = fire, B = use) rather than
// KEY_ENTER/KEY_BACKSPACE.
static int accelerates_on_fire(void)
{
    return gamestate == GS_INTERMISSION && !menuactive && !demoplayback;
}

// Doom number key ('1'..'7') for the weapon after/before the one in hand
// that the player owns.
static void cycle_weapon(int dir)
{
    static const int order[7] = {wp_fist, wp_pistol, wp_shotgun, wp_chaingun,
                                 wp_missile, wp_plasma, wp_bfg};
    player_t *p = &players[consoleplayer];
    int cur = 0, i;

    switch (p->readyweapon)
    {
        case wp_chainsaw:      cur = 0; break;
        case wp_supershotgun:  cur = 2; break;
        default:
            for (i = 0; i < 7; i++)
                if (order[i] == (int) p->readyweapon)
                    cur = i;
    }

    for (i = 1; i < 7; i++)
    {
        int idx = (cur + dir * i + 7 * 7) % 7;
        int w = order[idx];
        boolean owned = p->weaponowned[w];

        if (idx == 0)
            owned = owned || p->weaponowned[wp_chainsaw];
        if (idx == 2)
            owned = owned || p->weaponowned[wp_supershotgun];
        if (owned)
        {
            queue_key(1, (unsigned char) ('1' + idx));
            queue_key(0, (unsigned char) ('1' + idx));
            return;
        }
    }
}

static unsigned char map_button(int btn, int gameplay, int crank_out)
{
    if (!gameplay)
    {
        boolean confirm = messageToPrint && messageNeedsInput;

        switch (btn)
        {
            case BTN_LEFT:  return KEY_LEFTARROW;
            case BTN_RIGHT: return KEY_RIGHTARROW;
            case BTN_UP:    return KEY_UPARROW;
            case BTN_DOWN:  return KEY_DOWNARROW;
            case BTN_A:     return confirm ? 'y' : KEY_ENTER;
            case BTN_B:     return confirm ? 'n' : KEY_BACKSPACE;
        }
        return 0;
    }

    switch (btn)
    {
        case BTN_LEFT:  return crank_out ? KEY_STRAFE_L : KEY_LEFTARROW;
        case BTN_RIGHT: return crank_out ? KEY_STRAFE_R : KEY_RIGHTARROW;
        case BTN_UP:    return KEY_UPARROW;
        case BTN_DOWN:  return KEY_DOWNARROW;
        case BTN_A:     return KEY_FIRE;
    }
    return 0; // B is handled separately (use on release, weapon chord)
}

void dgpd_PollInput(void)
{
    PlaydateAPI *pd = pd_glue_api();
    PDButtons cur, pushed, released;
    int gameplay = in_gameplay();
    int map_gameplay = gameplay || accelerates_on_fire();
    int crank_out = !pd->system->isCrankDocked();
    int i;

    if (use_release_pending)
    {
        queue_key(0, KEY_USE);
        use_release_pending = 0;
    }

    if (pending_menu_key)
    {
        queue_key(1, pending_menu_key);
        queue_key(0, pending_menu_key);
        pending_menu_key = 0;
    }

    pd->system->getButtonState(&cur, &pushed, &released);

    // Releases first, so a mode change (e.g. a menu opening) never strands a
    // key that was pressed under the previous mapping.
    for (i = 0; i < BTN_COUNT; i++)
    {
        if ((released & button_bit[i]) && held_key[i])
        {
            queue_key(0, held_key[i]);
            held_key[i] = 0;
        }
    }

    if (b_down && (released & kButtonB))
    {
        b_down = 0;
        if (!b_chorded)
        {
            queue_key(1, KEY_USE);
            use_release_pending = 1;
        }
    }

    for (i = 0; i < BTN_COUNT; i++)
    {
        unsigned char key;

        if (!(pushed & button_bit[i]))
            continue;

        if (map_gameplay && i == BTN_B)
        {
            b_down = 1;
            b_chorded = 0;
            continue;
        }

        if (gameplay && b_down && (i == BTN_LEFT || i == BTN_RIGHT))
        {
            cycle_weapon(i == BTN_RIGHT ? 1 : -1);
            b_chorded = 1;
            continue;
        }

        key = map_button(i, map_gameplay, crank_out);
        if (key)
        {
            queue_key(1, key);
            held_key[i] = key;
        }
    }

    // Always-run is a held Shift, only while actually playing.
    if (always_run && gameplay && !run_held)
    {
        queue_key(1, KEY_RSHIFT);
        run_held = 1;
    }
    else if (run_held && !(always_run && gameplay))
    {
        queue_key(0, KEY_RSHIFT);
        run_held = 0;
    }

    if (gameplay)
    {
        float crank = pd->system->getCrankChange();

        if (crank != 0.0f)
        {
            event_t ev;

            ev.type = ev_mouse;
            ev.data1 = 0;
            ev.data2 = (int) (crank * CRANK_TURN_SCALE);
            ev.data3 = 0;
            D_PostEvent(&ev);
        }
    }
    else
    {
        (void) pd->system->getCrankChange(); // discard so it doesn't jump when play resumes
    }
}

void dgpd_OpenMenu(void)
{
    pending_menu_key = KEY_ESCAPE;
}

void dgpd_ToggleAutomap(void)
{
    pending_menu_key = KEY_TAB;
}

void dgpd_SetAlwaysRun(int on)
{
    always_run = on;
}

int dgpd_AutomapActive(void)
{
    return automapactive;
}
