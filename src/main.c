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

static PlaydateAPI *pd;

PlaydateAPI *pd_glue_api(void)
{
    return pd;
}

static PDMenuItem *automap_item;
static int automap_shown; // title currently reflects an open automap

// 0: pick a WAD, 1: show splash, 2: load Doom, 3: running
enum
{
    BOOT_SELECT,
    BOOT_SPLASH,
    BOOT_LOAD,
    BOOT_RUN
};
static int boot_stage = BOOT_SELECT;

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

        doomgeneric_Create(4, argv);
        dgpd_StartAudio(); // only now: a mixer running through the long load makes a tone
        boot_stage = BOOT_RUN;
        return 1;
    }

    dgpd_PollInput();
    doomgeneric_Tick();

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
    (void)arg;

    if (event == kEventInit)
    {
        pd = playdate;
        pd->display->setRefreshRate(35);
        pd->system->setUpdateCallback(update, NULL);
        dgpd_wad_Scan();

        pd->system->addMenuItem("Game menu", menu_doom, NULL);
        automap_item = pd->system->addMenuItem("Automap", menu_automap, NULL);
        // Music on/off lives in Doom's own Sound Volume options (music volume 0).
        // Always-run stays permanently on (dgpd_SetAlwaysRun's default); it's
        // still reachable via dgpd_SetAlwaysRun() if a menu slot ever frees up.
    }

    return 0;
}
