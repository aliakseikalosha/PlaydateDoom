// doomgeneric platform layer for the Playdate (400x240, 1-bit, D-pad + A/B + crank).
//
// Video:   Doom's 320x240 8-bit frame is converted to 1 bit and centred in
//          the 400x240 display (full height; only left/right bars remain)
//          by dgpd_dither_ConvertFrame (see dgpd_dither.c for the dither
//          algorithms themselves).
// Input:   D-pad moves/turns, A fires, B uses. The crank turns. With the crank
//          extended the D-pad's left/right strafe instead of turning, and the
//          weapon auto-fires while a monster is in front of the player (Options
//          menu toggle). Hold B and tap left/right to cycle weapons. In menus/intermissions A is
//          Enter and B is Back (or Yes/No on confirmation prompts).
#include <stdint.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_player.h"
#include "m_controls.h"
#include "p_local.h"
#include "p_mobj.h"

#include "dgpd.h"
#include "dgpd_dither.h"
#include "pd_glue.h"

#define LCD_ROWBYTES 52
#define VIEW_X_BYTES ((LCD_COLUMNS / 8 - DOOMGENERIC_RESX / 8) / 2)
#define VIEW_Y ((LCD_ROWS - DOOMGENERIC_RESY) / 2)

// Doom's turning is mouse-driven when analog; one crank degree maps to this
// many mouse counts (about 1.75 degrees of turn per crank degree).
#define CRANK_TURN_SCALE 40.0f

extern boolean menuactive;
extern boolean automapactive;
extern int autoFire; // Options menu toggle (m_menu.c)
extern int messageToPrint;
extern boolean messageNeedsInput;

// ---------------------------------------------------------------- video

void DG_Init(void)
{
    PlaydateAPI *pd = pd_glue_api();

    pd->graphics->clear(kColorBlack);
}

void DG_DrawFrame(void)
{
    PlaydateAPI *pd = pd_glue_api();
    uint8_t *frame = pd->graphics->getFrame();

    dgpd_dither_ConvertFrame(frame + VIEW_Y * LCD_ROWBYTES + VIEW_X_BYTES, LCD_ROWBYTES);

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
static int autofire_held;                 // KEY_FIRE held by crank-out auto fire

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

// Uses Doom's own next/previous-weapon keys, which walk weapon_order_table
// and so handle the chainsaw and super shotgun (sharing slots 1 and 3 with
// the fist/shotgun) correctly; number keys can only toggle within a slot.
static void cycle_weapon(int dir)
{
    unsigned char key = dir > 0 ? KEY_END : KEY_HOME;

    key_prevweapon = KEY_HOME;
    key_nextweapon = KEY_END;
    queue_key(1, key);
    queue_key(0, key);
}

// True when a live monster is in front of the player, probing the same three
// angles P_BulletSlope uses so auto fire triggers exactly when a shot would
// be auto-aimed at something. Fist/chainsaw only count targets in melee reach.
static int enemy_in_front(void)
{
    player_t *player = &players[consoleplayer];
    mobj_t *mo = player->mo;
    fixed_t range;
    angle_t an;
    int i;

    if (!mo || player->playerstate != PST_LIVE)
        return 0;

    range = (player->readyweapon == wp_fist || player->readyweapon == wp_chainsaw)
            ? MELEERANGE : 16 * 64 * FRACUNIT;

    an = mo->angle;
    for (i = 0; i < 3; i++)
    {
        P_AimLineAttack(mo, an, range);
        if (linetarget && (linetarget->flags & MF_COUNTKILL) && linetarget->health > 0)
            return 1;
        an += (i == 0) ? 1 << 26 : -(2 << 26);
    }
    return 0;
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
    // key that was pressed under the previous mapping. The release edge alone
    // isn't trusted: a tap shorter than one (slow) update reports pushed and
    // released together, and a release while the system menu is open is never
    // reported at all. Either would leave the key down forever, so anything
    // no longer in the current state is released too. A same-update tap is
    // pushed below and released here on the next poll, so Doom still sees it
    // for one frame.
    for (i = 0; i < BTN_COUNT; i++)
    {
        if (held_key[i] && ((released & button_bit[i]) || !(cur & button_bit[i])))
        {
            queue_key(0, held_key[i]);
            held_key[i] = 0;

            // Letting go of A must not cancel auto fire that is still on.
            if (i == BTN_A && autofire_held)
                queue_key(1, KEY_FIRE);
        }
    }

    if (b_down && ((released & kButtonB) || !(cur & kButtonB)))
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

    // Crank-out auto fire: hold fire while an enemy is in front. A is still
    // free to fire manually; the hold is only released when neither wants it.
    if (gameplay && autoFire && crank_out && !b_down && enemy_in_front())
    {
        if (!autofire_held)
        {
            queue_key(1, KEY_FIRE);
            autofire_held = 1;
        }
    }
    else if (autofire_held)
    {
        autofire_held = 0;
        if (!held_key[BTN_A])
            queue_key(0, KEY_FIRE);
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
