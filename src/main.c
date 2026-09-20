// Playdate entry point for Doom (doomgeneric).
#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "dgpd.h"
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

static int boot_stage; // 0: show splash, 1: load Doom, 2: running

static int update(void *userdata)
{
    (void)userdata;

    if (boot_stage == 0)
    {
        const char *msg = "Loading DOOM...";

        pd->graphics->clear(kColorBlack);
        pd->graphics->setDrawMode(kDrawModeFillWhite);
        pd->graphics->drawText(msg, strlen(msg), kASCIIEncoding, 140, 112);
        boot_stage = 1;
        return 1;
    }

    if (boot_stage == 1)
    {
        static char *argv[] = {"doom", "-iwad", "doom1.wad", "-nogui", NULL};

        doomgeneric_Create(4, argv);
        boot_stage = 2;
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
    dgpd_OpenMenu();
}

static void menu_automap(void *ud)
{
    (void)ud;
    dgpd_ToggleAutomap();
}

static void menu_run(void *ud)
{
    PDMenuItem *item = ud;

    dgpd_SetAlwaysRun(pd->system->getMenuItemValue(item));
}

int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg)
{
    (void)arg;

    if (event == kEventInit)
    {
        PDMenuItem *run;

        pd = playdate;
        pd->display->setRefreshRate(35);
        pd->system->setUpdateCallback(update, NULL);

        pd->system->addMenuItem("Doom menu", menu_doom, NULL);
        automap_item = pd->system->addMenuItem("Automap", menu_automap, NULL);
        run = pd->system->addCheckmarkMenuItem("Always run", 1, menu_run, NULL);
        pd->system->setMenuItemUserdata(run, run);
    }

    return 0;
}
