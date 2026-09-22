// Unlimited Film, a mod for Snap64 Recomp.
//
// The game gives 60 shots a course: every photo goes into a roll of 60
// records (D_800B0598, app_render/47380.c), the counter in the corner shows
// 60 minus the photos taken, and when it reaches 0 the ride ends with
// "You're out of film!". This mod keeps the roll from ever being full: as
// the 61st shot fires, the oldest record is dropped and the others move
// down, so the new photo lands in the last slot and Oak sees the newest 60.
// The counter is told the roll is always full, so the shutter keeps its
// normal click and the ride goes on; a setting lets it count the shots
// taken instead.
//
// Both changes are replacements of two small functions of the level code
// (app_level/4FE780.c), which the shutter and the counter call; nothing is
// hooked. The shutter asks func_8035E508_4FE918 how many shots are left
// right before it stores a photo, so that is where room is made.

#include "modding.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "common.h"

#define ROLL 60

extern s32 gPhotoCount;             // photos in the roll (app_render/47380.c)
extern PhotoData D_800B0598[ROLL];  // the roll itself

// The counter in the corner (app_level/4FE780.c): three digit sprites and
// the bitmap each shows, from a table of the ten digits.
extern s32 D_80388F58_529368;       // the ones digit's bitmap
extern s32 D_803890B8_5294C8;       // the tens digit's bitmap
extern s32 D_80389218_529628;       // the hundreds digit's bitmap
extern s32 D_8038A034_52A444[];     // the ten digits
extern SObj* D_803B0A18_550E28;     // the tens sprite (hidden below 10)
extern SObj* D_803B0A1C_550E2C;     // the hundreds sprite (hidden below 100)

static u32 dropped = 0;   // photos pushed out of the roll this ride

// The shots left, as the shutter asks right before it stores a photo: if
// the roll is full, drop the oldest so the new one fits. The records move
// down one slot a word at a time through volatile pointers, so the compiler
// leaves the loop as it is instead of calling a copy routine of its own.
// Never few, so the click is the normal one.
RECOMP_PATCH s32 func_8035E508_4FE918(void) {
    if (gPhotoCount >= ROLL) {
        volatile u32* dst = (volatile u32*) &D_800B0598[0];
        const volatile u32* src = (const volatile u32*) &D_800B0598[1];
        s32 words = (s32) ((sizeof(PhotoData) * (ROLL - 1)) / sizeof(u32));
        while (words > 0) {
            *dst = *src;
            dst++;
            src++;
            words--;
        }
        gPhotoCount = ROLL - 1;
        dropped++;
        recomp_printf("[unlimited film] photo %u: the roll was full, the oldest dropped\n",
                      (unsigned) (ROLL + dropped));
    }
    return ROLL;
}

// The counter in the corner, as the game draws it when the ride starts and
// after every photo: the same three digits, showing 60 throughout or the
// shots taken; never red, and never 0, which is what would end the ride.
RECOMP_PATCH s32 func_8035E52C_4FE93C(void) {
    s32 value = ROLL;
    s32 ones, tens, hundreds;

    if (gPhotoCount == 0) {
        dropped = 0;   // a new ride
    }
    if (recomp_get_config_u32("counter") == 1) {
        value = gPhotoCount + (s32) dropped;
        if (value > 999) {
            value = 999;
        }
    }
    ones = value % 10;
    tens = (value % 100) / 10;
    hundreds = value / 100;

    D_80388F58_529368 = D_8038A034_52A444[ones];
    if (hundreds == 0) {
        if (tens == 0) {
            spSetAttribute(&D_803B0A18_550E28->sprite, SP_HIDDEN);
        } else {
            D_803890B8_5294C8 = D_8038A034_52A444[tens];
            spClearAttribute(&D_803B0A18_550E28->sprite, SP_HIDDEN);
        }
        spSetAttribute(&D_803B0A1C_550E2C->sprite, SP_HIDDEN);
    } else {
        D_803890B8_5294C8 = D_8038A034_52A444[tens];
        spClearAttribute(&D_803B0A18_550E28->sprite, SP_HIDDEN);
        D_80389218_529628 = D_8038A034_52A444[hundreds];
        spClearAttribute(&D_803B0A1C_550E2C->sprite, SP_HIDDEN);
    }
    return ROLL;
}
