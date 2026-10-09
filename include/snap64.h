#ifndef __SNAP64_H__
#define __SNAP64_H__

#include "modding.h"

// What Snap64 Recomp itself offers a mod, beyond the functions every N64
// recompilation provides (recompconfig.h, recompdata.h, recomp_printf). A mod
// that imports these needs the port at 1.1.3 or later: say so with
// minimum_recomp_version in mod.toml, or an older port refuses the mod and
// names the import.

// The computer's local clock, as the operating system has it (zone and
// daylight saving included): hour << 16 | minute << 8 | second, the hour
// 0 to 23. Read fresh on every call.
RECOMP_IMPORT("*", unsigned long snap64_local_time(void));

// The computer's local date: year << 16 | month << 8 | day.
RECOMP_IMPORT("*", unsigned long snap64_local_date(void));

// A line of the mod's own on its details page (Options > Mods), shown after
// the description: what the mod is doing now, in words a player can check.
// mod_id is the id in mod.toml. A "%T" in the line is filled with the
// computer's clock ("6:41 PM") as the page is read, so a clock stays right
// while the page is up; an empty line takes the line away. Up to 240
// characters, wrapped to the page's lines.
RECOMP_IMPORT("*", void snap64_set_status(const char* mod_id, const char* line));

// A short line over a course for five seconds, at the top left in the
// pages' help face over a dark panel, from the moment the HUD is up: what
// the mod just did, in words a player can read without a menu. A new one
// replaces the old; none outlives the scene it was sent in. Up to about
// forty characters show; longer lines end in dots.
RECOMP_IMPORT("*", void snap64_notice(const char* mod_id, const char* line));

// The computer's clock zone: minutes east of UTC, daylight saving included,
// negative to the west (New York in October: -240).
RECOMP_IMPORT("*", long snap64_utc_offset(void));

#endif
