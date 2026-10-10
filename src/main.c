// Playdate entry point for Doom (doomgeneric).
#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "dgpd.h"
#include "dgpd_wadselect.h"
#include "pd_glue.h"

// doomgeneric.h pulls in Doom types; declare just what is needed here.
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);
void I_AtExit(void (*func)(void), int run_on_error); // i_system.c
extern int show_endoom;                              // d_main.c

static PlaydateAPI *pd;

PlaydateAPI *pd_glue_api(void)
{
    return pd;
}

static PDMenuItem *automap_item;
static int automap_shown; // title currently reflects an open automap

// 0: pick a WAD, 1: show splash, 2: load Doom, 3: running, 4: leaving Doom
enum
{
    BOOT_SELECT,
    BOOT_SPLASH,
    BOOT_LOAD,
    BOOT_RUN,
    BOOT_EXIT
};
static int boot_stage = BOOT_SELECT;
static int quit_requested; // Doom finished (its menu's "Quit Game")
static int restarted;      // restartGame() has been called

// Launch argument that tells the freshly restarted game which WAD it just left.
#define LAUNCH_WAD_ARG "wad="

// Runs last of Doom's exit functions (I_Quit calls them newest first, and this
// is registered before Doom registers its own). In this port I_Quit returns
// instead of exiting the process, so this is where the game learns it was quit.
// (The one exception is D_Endoom, which calls exit(0) after the ENDOOM screen;
// that is switched off after Doom has started, see below.)
static void doom_quit(void)
{
    quit_requested = 1;
}

// Doom keeps all of its state in globals and can't be started twice in one
// process, so going back to the picker means restarting the whole game. The
// restart is handled by the system once update() returns.
static void return_to_picker(void)
{
    static char args[64]; // outlives the call, in case the system reads it later

    snprintf(args, sizeof(args), LAUNCH_WAD_ARG "%s", dgpd_wad_Name());
    pd->system->restartGame(args);
    restarted = 1;
}

static int update(void *userdata)
{
    (void)userdata;

    if (boot_stage == BOOT_SELECT)
    {
        if (dgpd_wad_Update())
            boot_stage = BOOT_SPLASH;
        return 1;
    }

    if (boot_stage == BOOT_SPLASH)
    {
        dgpd_wad_DrawMessage("Loading Game...");
        boot_stage = BOOT_LOAD;
        return 1;
    }

    if (boot_stage == BOOT_LOAD)
    {
        // static: Doom keeps myargv and reads it for the whole session (e.g. at demo start)
        static char *argv[] = {"doom", "-iwad", NULL, "-nogui", NULL};

        argv[2] = (char *)dgpd_wad_Path();

        I_AtExit(doom_quit, 0);
        doomgeneric_Create(4, argv);
        show_endoom = 0; // no ENDOOM screen here, and its exit(0) would end the session
        dgpd_StartAudio(); // only now: a mixer running through the long load makes a tone
        boot_stage = BOOT_RUN;
        return 1;
    }

    if (boot_stage == BOOT_EXIT)
    {
        if (!restarted)
            return_to_picker();
        return 1;
    }

    dgpd_PollInput();
    doomgeneric_Tick();

    if (quit_requested)
    {
        // Keep the picture up while the system restarts the game.
        dgpd_wad_DrawMessage("Returning to WAD selection...");
        boot_stage = BOOT_EXIT;
        return 1;
    }

    if (dgpd_AutomapActive() != automap_shown)
    {
        automap_shown = dgpd_AutomapActive();
        pd->system->setMenuItemTitle(automap_item, automap_shown ? "Game View" : "Automap");
    }
    return 1;
}

static void menu_doom(void *ud)
{
    (void)ud;
    if (boot_stage == BOOT_RUN)
        dgpd_OpenMenu();
}

static void menu_automap(void *ud)
{
    (void)ud;
    if (boot_stage == BOOT_RUN)
        dgpd_ToggleAutomap();
}

int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg)
{
    const char *args;

    (void)arg;

    if (event == kEventInit)
    {
        pd = playdate;
        pd->display->setRefreshRate(35);
        pd->system->setUpdateCallback(update, NULL);
        dgpd_wad_Scan();

        // Back from a game: open the picker on the WAD that was just played.
        args = pd->system->getLaunchArgs(NULL);
        if (args != NULL && strncmp(args, LAUNCH_WAD_ARG, strlen(LAUNCH_WAD_ARG)) == 0)
            dgpd_wad_Return(args + strlen(LAUNCH_WAD_ARG));

        pd->system->addMenuItem("Game menu", menu_doom, NULL);
        automap_item = pd->system->addMenuItem("Automap", menu_automap, NULL);
        // Music on/off lives in Doom's own Sound Volume options (music volume 0).
        // Always-run stays permanently on (dgpd_SetAlwaysRun's default); it's
        // still reachable via dgpd_SetAlwaysRun() if a menu slot ever frees up.
    }

    return 0;
}
