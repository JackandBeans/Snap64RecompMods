// Unlock Everything, a mod for Snap64 Recomp.
//
// The game keeps its progress in the save: the highest course opened (0 the
// Beach to 6 Rainbow Cloud), which course buttons the lab has shown, and a
// flag for each item and for the Beach tutorial (more_funcs/5BF20.c). Each
// time a scene is set up, this mod sets all of them to the finished state,
// through the game's own setters, which is what the port's
// tools/unlock_save.py does to a save file offline. The game writes the
// save when it next saves, so the change stays; the Pokemon Report and the
// album are not touched.

#include "modding.h"
#include "recomputils.h"
#include "common.h"

#define LAST_COURSE 6   // Rainbow Cloud

s32 func_800C0290_5D130(void);        // the highest course opened
void func_800C02A0_5D140(s32 course); // set it
s32 func_800BFC5C_5CAFC(void);        // the course whose button the lab last presented
void func_800BFC70_5CB10(s32 course); // set it
extern void* D_800C21B0_5F050;        // the save in memory, set up at boot

static s32 give(s32 pfid) {
    if (checkPlayerFlag(pfid) == 0) {
        setPlayerFlag(pfid, 1);
        return 1;
    }
    return 0;
}

RECOMP_HOOK("omSetupScene") void unlock_everything_on_scene(void* setup) {
    s32 changed = 0;

    if (D_800C21B0_5F050 == NULL) {
        return;
    }
    // The presented-button record first: raising the highest course above
    // it is how the game notices a new course, and would have the lab
    // announce one.
    if (func_800BFC5C_5CAFC() < LAST_COURSE) {
        func_800BFC70_5CB10(LAST_COURSE);
        changed = 1;
    }
    if (func_800C0290_5D130() < LAST_COURSE) {
        func_800C02A0_5D140(LAST_COURSE);
        changed = 1;
    }
    changed |= give(PFID_HAS_APPLE);
    changed |= give(PFID_HAS_PESTER_BALL);
    changed |= give(PFID_HAS_FLUTE);
    changed |= give(PFID_HAS_DASH_ENGINE);
    changed |= give(PFID_7);   // the lab's full button layout
    changed |= give(PFID_HAS_FINISHED_TUTORIAL);

    if (changed) {
        recomp_printf("[unlock everything] every course open, the Apple, the Pester Ball, the Poke Flute and the Dash Engine in hand, the tutorial done\n");
    }
}
