// Island at Night, a mod for Snap64 Recomp.
//
// The courses under an open sky -- the Beach, the River, the Volcano and the
// Valley -- after dark. (The Tunnel and the Cave have no sky to turn.)
//
// These courses are drawn the way most of the game is: every surface is a
// texture multiplied by a shade, and the shade is either a color painted on
// the vertex or the scene's one light falling on it. The fog, the color
// behind everything and the sky belong to the course's own set-up. So night
// is made here by changing those few things as a course is set up, and
// nothing else: the game then draws its own night, on the ride and again
// whenever it redraws one of the course's photos for Professor Oak or the
// album.
//
//   - the course's set-up record (fog, background) is changed before the
//     game reads it;
//   - the scene's light is dimmed toward the night's own light while the
//     course's world exists, and put back when it is taken down;
//   - the colors painted on vertices are multiplied by the same light, once
//     per model, as each model is first drawn;
//   - the sky's small picture is recolored in place: the same clouds, by
//     night;
//   - the sky's list is made to call one more list, the mod's own: stars
//     between the clouds and, where the night has one, a moon
//     (night_data.c, written by tools/gen_night.py). The stars twinkle by
//     the course's own clock, and a photo keeps the twinkle of its instant.
//
// Everything is done to the course's data as it sits in memory. That data
// is read from the cartridge again each time a course is loaded, so the
// change never outlives the course.

#include "modding.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "snap64.h"
#include "common.h"
#include "world/world.h"
#include "night_data.h"

// The scene light every course shares (app_render, always in memory) and
// the four display-list cursors.
#define COURSE_LIGHT  ((volatile u8*) 0x800AEBD0)
#define MAIN_GFX_POS  ((Gfx**) 0x8004A890)
#define SKY_TEXELS    (64 * 32)

// What the cartridge loads together with a course. Only vertices and
// commands inside these spans are ever touched, because only these are read
// fresh from the cartridge with the course:
//   LOW   on the ride and for a photo: the course's own data and the
//         Pokémon data loaded beside it (Magikarp, Pikachu, Zubat,
//         Bulbasaur);
//   RIDE  on the ride only: the ride's own models of those four;
//   LEVEL on the ride only: the data of the level code every course shares
//         (the Zero-One and its rider, the things thrown);
//   PHOTO for a photo only: the close-up models of those four, which sit
//         where the level code's memory ends.
#define LOW_LO    0x800F5D90u
#define LOW_HI    0x801D6840u
#define RIDE_LO   0x8033F6C0u
#define RIDE_HI   0x80350200u
#define LEVEL_LO  0x80366BA0u
#define LEVEL_HI  0x803B5000u
#define PHOTO_LO  0x803A71B0u
#define LOW_UNITS   ((LOW_HI - LOW_LO) >> 3)
#define RIDE_UNITS  ((RIDE_HI - RIDE_LO) >> 3)
#define LEVEL_UNITS ((LEVEL_HI - LEVEL_LO) >> 3)
#define MAP_BYTES   ((LOW_UNITS + RIDE_UNITS + LEVEL_UNITS) / 8 + 1)

// A photo's world is built with this function of the photo code as its
// "a Pokemon enters" callback; the ride's world with the course's own.
#define PHOTO_WORLD_CALLBACK 0x8009D0B4u

// The scene light by day, as the cartridge has it.
#define DAY_AMBIENT 100
#define DAY_DIFFUSE 180
// At night the ambient share rises and the diffuse falls, as moonlight
// models softly; the two still add up to the day's.
#define NIGHT_AMBIENT 125
#define NIGHT_DIFFUSE 155

#define G_LIGHTING_BIT 0x00020000u

// Written into the empty last byte of a course's set-up record once its
// load has been made night.
#define DONE_MARK 0xA5

// One course's night. Colors are red, green, blue; lights are parts of 256.
typedef struct Night {
    u32 setup;              // the course's set-up record
    const char* option;     // the mod's setting for this course
    const char* weather;    // its setting for this course's weather, or none
    u32 spare;              // the pipe sync in the sky's list that gives its place to our call
    u32 pics[2];            // the sky's pictures, 64x32
    const u8* rows[2];      // each picture row's height: 0 at the horizon, 255 overhead
    u32 sky_vtx[2];         // the sky box's vertex arrays
    u8 sky_vtx_n[2];
    u8 r_lo, r_scale;       // cloud = (a texel's red, 5 bits, minus r_lo) * r_scale, of 256
    u32 world_lo, world_hi; // where the course's ground and scenery vertices lie
    u8 world[3];            // the light on the scenery
    u8 body[3];             // the light on Pokémon and objects, and the scene light
    u8 flow[3];             // the light on what flows: the water, or the Volcano's lava
    u8 pic_sky[3], pic_cloud[3];            // the night sky's picture, kept bright: open sky, and cloud
    u8 tint_lo[3], tint_hi[3];              // the box's corners: the tint at the horizon, and at the top
    u16 fog_min, fog_max;
    Gfx* sky;               // the mod's list for this sky
    Gfx (*twinkle)[2];      // its brightness commands
    u8 twinkles;
    s8 key[3];              // where the night's key light comes from: the moon, or the lava below
    u8 glint;               // the moon's glint on the water: the courses that have water
    u8 embers;              // the Volcano: embers over the lava, its sky pulsing with it, ash for weather
    Gfx* const* fireflies;  // fireflies over the water, where there are any
    s8 sun[3];              // where the sunset's sun stands
    Gfx* sun_list;          // the sun and its haze, for the sky at sunset
    s8 dawn_sun[3];         // where the dawn's sun stands
    Gfx* dawn_list;         // the dawn's sun, for the sky at dawn
    Vtx* band;              // the glow band at the horizon toward the sunset's sun (night_data.h)
    Vtx* dawn_band;         // and toward the dawn's
} Night;

#define NIGHT_TUNNEL_SETUP 0x8011E6CCu  // tunnel_WorldSetup: no sky box; a flat sky color where it opens

static const Night nights[] = {
    {   // The Beach: a clear night, a full moon over the sea.
        NIGHT_BEACH_SETUP, "beach", "beach_weather", NIGHT_BEACH_SPARE, NIGHT_BEACH_PICS, NIGHT_BEACH_ROWS,
        NIGHT_BEACH_SKY_VTX, NIGHT_BEACH_SKY_VTX_N, NIGHT_BEACH_R_LO, NIGHT_BEACH_R_SCALE,
        0x8011B050u, 0x80135BD4u, { 92, 123, 205 }, { 108, 141, 220 }, { 80, 112, 190 },
        { 104, 120, 184 }, { 196, 200, 212 }, { 48, 56, 88 }, { 16, 20, 48 }, 990, 1000,
        night_beach_sky, night_beach_twinkle, NIGHT_BEACH_TWINKLES, NIGHT_BEACH_KEY, 1, 0, NULL, NIGHT_BEACH_SUN, night_beach_sun, NIGHT_BEACH_DAWN, night_beach_dawn, night_beach_band, night_beach_dawn_band },
    {   // The River: broken cloud, a mist over the water.
        NIGHT_RIVER_SETUP, "river", "river_weather", NIGHT_RIVER_SPARE, NIGHT_RIVER_PICS, NIGHT_RIVER_ROWS,
        NIGHT_RIVER_SKY_VTX, NIGHT_RIVER_SKY_VTX_N, NIGHT_RIVER_R_LO, NIGHT_RIVER_R_SCALE,
        0x8012A0C0u, 0x801416F0u, { 86, 124, 196 }, { 104, 142, 214 }, { 66, 104, 156 },
        { 104, 120, 176 }, { 188, 192, 208 }, { 48, 56, 80 }, { 16, 20, 44 }, 988, 1000,
        night_river_sky, night_river_twinkle, NIGHT_RIVER_TWINKLES, NIGHT_RIVER_KEY, 1, 0, night_firefly_groups, NIGHT_RIVER_SUN, night_river_sun, NIGHT_RIVER_DAWN, night_river_dawn, night_river_band, night_river_dawn_band },
    {   // The Volcano: no moon. The lava keeps its own light and lights the smoke from below.
        NIGHT_VOLCANO_SETUP, "volcano", "volcano_weather", NIGHT_VOLCANO_SPARE, NIGHT_VOLCANO_PICS, NIGHT_VOLCANO_ROWS,
        NIGHT_VOLCANO_SKY_VTX, NIGHT_VOLCANO_SKY_VTX_N, NIGHT_VOLCANO_R_LO, NIGHT_VOLCANO_R_SCALE,
        0x800FFF90u, 0x80112260u, { 132, 96, 92 }, { 172, 132, 122 }, { 255, 240, 228 },
        { 72, 56, 64 }, { 255, 210, 150 }, { 108, 60, 44 }, { 24, 20, 40 }, 986, 1000,
        night_volcano_sky, night_volcano_twinkle, NIGHT_VOLCANO_TWINKLES, { 0, -127, 0 }, 0, 1, NULL, NIGHT_VOLCANO_SUN, night_volcano_sun, NIGHT_VOLCANO_DAWN, night_volcano_dawn, night_volcano_band, night_volcano_dawn_band },
    {   // The Valley: a high, clear mountain night.
        NIGHT_VALLEY_SETUP, "valley", "valley_weather", NIGHT_VALLEY_SPARE, NIGHT_VALLEY_PICS, NIGHT_VALLEY_ROWS,
        NIGHT_VALLEY_SKY_VTX, NIGHT_VALLEY_SKY_VTX_N, NIGHT_VALLEY_R_LO, NIGHT_VALLEY_R_SCALE,
        0x800FFF90u, 0x80116A70u, { 84, 112, 190 }, { 104, 136, 220 }, { 46, 72, 126 },
        { 104, 120, 184 }, { 196, 200, 212 }, { 48, 56, 88 }, { 12, 16, 44 }, 992, 1000,
        night_valley_sky, night_valley_twinkle, NIGHT_VALLEY_TWINKLES, NIGHT_VALLEY_KEY, 1, 0, NULL, NIGHT_VALLEY_SUN, night_valley_sun, NIGHT_VALLEY_DAWN, night_valley_dawn, night_valley_band, night_valley_dawn_band },
    {   // The Tunnel: no sky of its own, a flat color where it opens to the air; the hour's
        // light and sky color, no weather (its roof keeps the weather out), no stars.
        NIGHT_TUNNEL_SETUP, "tunnel", NULL, 0, { 0, 0 }, { 0, 0 },
        { 0, 0 }, { 0, 0 }, 0, 0,
        0x8011CB50u, 0x8018BC30u, { 118, 130, 178 }, { 126, 140, 196 }, { 100, 120, 170 },
        { 196, 200, 212 }, { 196, 200, 212 }, { 48, 56, 88 }, { 16, 20, 48 }, 996, 1000,
        NULL, NULL, 0, { 0, 127, 0 }, 0, 0, NULL, { 0, 13, -127 }, NULL, { 0, 13, 127 }, NULL, NULL, NULL },
};

extern f32 GlobalTimer;         // the world's clock: it advances by the set-up's sky speed each tick

static const Night* night;       // the course whose world exists and is at night, or none
static u8 on_the_ride;           // that world is the ride's, not a photo's
static u8 raining;               // that night is a rainy one
static u8 storming;              // and the rain has lightning in it
static u8 snowing;               // or that night is a snowy one
static u8 ashing;                // or, on the Volcano, ash is falling
static u8 scoring;               // the game is measuring a photo, not showing anything
static u8 fx_lit = 0;            // the settings: effects take the night's light (off by default),
static u8 glows_on = 1;          // the water glows are drawn,
static u8 moonlight_on = 1;      // the key light leans toward the moon
static u8 night_sounds_on = 1;   // the night's voices are heard
static u8 sunset;                // the time chosen is the sunset, not the night
static u8 dawn;                  // and that sunset is a dawn (the sun across the sky)
static u8 dusk;                  // or that the sun sets during the ride
static u8 cycle;                 // or that the player's clock drives the hour, round the whole day
static f32 nightness = 1.0f;     // how far from the cartridge's day the scene is: 0 the day itself, 1 the night's own colors
static f32 ride_minutes = 1380.0f;   // the clock's minute the ride took, kept for its photos' worlds
static f32 cycle_minute = -1.0f;     // the minute last applied
static u16 steep_shade[3] = { 110, 118, 160 };   // the steep faces' shade, 256 by day
static u16 sky_orig_pic[2][64 * 32];  // the sky pictures as the cartridge has them (the day end of the blend)
static u32 sky_orig_cn[2][64];        // the sky box's corner colors as the cartridge has them
static u8 sky_orig_cn_kept[2];
static u8 day_fog[3], day_bg[3];      // the course's own fog and background colors,
static u16 day_fog_min, day_fog_max;  // its own fog distances,
static u8 day_ambient[3], day_diffuse[3];    // its own scene light,
static u8 live_weather[8];            // the weather drawn for each course this session (Live, or Random)
static u8 live_weather_drawn[8];
static u8 tint_away[3];          // the corners' tint at the horizon away from the sun (tint_lo's twin by night)
static u8 scene_ambient[3], scene_diffuse[3];   // the scene light's colors in force
static u8 land_diff[3];          // what the sun (or the moon) adds to a painted face that faces it
static f32 relight_dir[3];       // toward the sun, or the moon, in the world's axes
static u16 fog_near, fog_far;    // the night's fog, to come back to after a flash
static Mtx storm_mtx[2][2];      // the flash's and the bolt's matrices, this frame's pair and the last's
static Mtx streak_mtx[2];        // the shooting star's, two sets
static u8 streak_turn;
static u32 streak_logged;        // the last star the log named
static f32 view_fwd[2];          // where the view faces, level, a unit; from the game's light each frame
static u8 light_world[3];        // the lights in force: on the scenery,
static u8 light_body[3];         // on Pokémon and objects (and the scene light),
static u8 light_flow[3];         // and on what flows
static u8 sky_horizon[3], sky_top[3];    // the sky's colors in force: at the horizon (the fog's) and at the top (the background's)
static u8 pic_sky[3], pic_cloud[3];      // the sky picture's colors in force
static u8 tint_lo[3], tint_hi[3];        // the box's corner tints in force
static f32 sky_sin_lo, sky_sin_hi;       // how low and how high the box's corners reach (sines of elevation)
static u8 cloud_top_fade;                // how far the clouds thin toward the drum's lid: full at sunset, none by night
static u8 sky_mask[SKY_TEXELS];          // the picture's luminance, then its softened cloud mask
static u8 sky_blur[SKY_TEXELS];
static Mtx rain_mtx[2][NIGHT_RAIN_SHEETS * 2];   // each sheet's two matrices, this frame's set and the last's
static u8 rain_turn;             // which of the two sets this frame writes
static Mtx glow_mtx[2][1 + NIGHT_FIREFLY_GROUPS];   // the moon path's and the fireflies' matrices, two sets
static u8 glow_turn;
static f32 flow_y_sum;           // the water's height: the mean, the lowest and the highest of the vertices that flow
static f32 flow_y_min, flow_y_max;
static u32 flow_y_n;
static u32 water_model;          // the model whose lists hold the water (its first list), once walked
static u8 drawing_water;         // that model is being drawn this frame

// The moon's glint on the water. The water's own triangles are copied as the
// course is first read -- each vertex load and each triangle of the lists
// that draw with the flowing material, node by node -- and drawn again right
// after the water every frame, with an adding blend and the soft dot as the
// moon's reflection. Each corner's place in the dot (its s and t) is set from
// how far the half-way direction between the eye and the moon tilts off the
// water's upright, split along the moon's bearing and across it, so the
// highlight lands where the reflection would, stretched toward the moon as
// the glitter path on real water is, and the dot is read per pixel between
// the corners, so even the Beach's wide sea triangles carry a compact light.
// The copies stand under the game's own matrices, read off the commands it
// wrote for the water's nodes this frame.
#define GLINT_VTX  12288
#define GLINT_OPS  24576
#define GLINT_SEGS 128
#define GLINT_LOAD 0x80000000u   // an op that loads copies: start << 12 | count << 6 | first slot
#define GLINT_USED 0x40000000u   // a load a triangle of the water draws from
#define GLINT_FLOW 0x20000000u   // a triangle of the water (else of the land)
#define GLINT_LAND 0x10000000u   // a load a triangle of the land draws from
typedef struct GlintSeg {
    u32 lists[4];           // the node's lists, as the game calls them
    u32 model;              // the model it belongs to (its first list)
    u16 op0, op_n;          // its loads and triangles
    u8 lists_n;
    u8 flow;                // whether any of its triangles are the water's
} GlintSeg;
static Vtx glint_vtx[2][GLINT_VTX];    // the copies, two sets: this frame's and the last's
static u32 glint_op[GLINT_OPS];
static GlintSeg glint_seg[GLINT_SEGS];
static Mtx glint_mtx[2][GLINT_SEGS];
static u16 glint_vtx_n, glint_op_n, glint_tris, glint_land;
static u32 glint_load_src[GLINT_OPS];    // where a load's vertices lie in the game's memory (the dusk relights them)
static u32 glint_orig_cn[GLINT_VTX];     // each copy's day colors, as the course has them
static u16 glint_face[GLINT_VTX];        // each copy's lighting: bit 15 lit, 14 steep, 13 water, low nine bits toward
static u8 sky_mask_pic[2][SKY_TEXELS];   // each sky picture's cloud mask, kept for the dusk
static u8 pal_a[11][3], pal_b[11][3];    // the sunset's palette and the night's,
static u8 pal_d[11][3];                  // and the dawn's: the cycle's three ends (the fourth, the day, is the cartridge)
static f32 dir_a[3], dir_b[3], dir_d[3];
static u8 star_fade = 255;               // how far the stars have come up
static u32 dusk_last;
static u8 glint_seg_n, glint_recording, glint_turn, glint_said, glint_said_drawn;
static u8 drawing_glint;              // the model being drawn has segments of the glint
static u32 glint_model;               // the model being walked
static u8 glint_slot_ok[64];           // the walker's slots that hold copies,
static u16 glint_slot_op[64];          // and the load that filled each
static u32 glint_top[4];               // the lists a node calls, while it is walked
static s32 glint_top_n;
static u8 glint_drawn[GLINT_SEGS];
static u8 ember_n;                      // the embers seeded so far (the Volcano)
static u8 ember_seeded[GLINT_SEGS];     // the parts whose embers are seeded
static void glint_reset(void) {
    glint_vtx_n = glint_op_n = glint_tris = glint_land = 0;
    glint_seg_n = 0;
    ember_n = 0;
    {
        s32 k;

        for (k = 0; k < GLINT_SEGS; k++) {
            ember_seeded[k] = 0;
        }
    }
    glint_said = 0;
    glint_said_drawn = 0;
}
static f32 walk_node_y;          // the height in the world of the node being walked (its blocks are placed)
#ifdef NIGHT_MAP
#define MAP_N    40
#define MAP_CELL 500.0f
static u8 map_kind[MAP_N * MAP_N];        // 1 land, 2 water, 4 the camera passed
static f32 map_water_y[MAP_N * MAP_N];
static u16 map_water_n[MAP_N * MAP_N];
static f32 walk_node_x, walk_node_z;
static u32 map_eye_tick;
static s32 map_cell(f32 x, f32 z) {
    s32 cx = (s32) ((x + 10000.0f) / MAP_CELL);
    s32 cz = (s32) ((z + 10000.0f) / MAP_CELL);
    if (cx < 0 || cz < 0 || cx >= MAP_N || cz >= MAP_N) {
        return -1;
    }
    return cz * MAP_N + cx;
}
static void map_print(void) {
    char row[MAP_N + 1];
    s32 cx, cz;

    recomp_printf("[night map] columns x = -10000 .. 10000 by 500; rows z\n");
    for (cz = 0; cz < MAP_N; cz++) {
        for (cx = 0; cx < MAP_N; cx++) {
            u8 k = map_kind[cz * MAP_N + cx];
            row[cx] = (k & 4) ? '@' : (k & 2) ? ((k & 1) ? '%' : '~') : (k & 1) ? '#' : '.';
        }
        row[MAP_N] = 0;
        recomp_printf("[night map] z=%6d |%s|\n", -10000 + cz * 500 + 250, row);
    }
    for (cz = 0; cz < MAP_N; cz++) {
        for (cx = 0; cx < MAP_N; cx++) {
            s32 c = cz * MAP_N + cx;
            if (map_water_n[c] != 0) {
                recomp_printf("[night water] x=%d z=%d y=%d n=%d\n", -10000 + cx * 500 + 250, -10000 + cz * 500 + 250,
                              (s32) (map_water_y[c] / (f32) map_water_n[c]), (s32) map_water_n[c]);
            }
            map_kind[c] = 0;
            map_water_y[c] = 0.0f;
            map_water_n[c] = 0;
        }
    }
    recomp_printf("[night map] end\n");
}
#endif
static u8 lit;                   // the walker's reading of G_LIGHTING
static u8 flowing;               // the walker's reading of the material: the one that flows
static u32 slots[64];            // the walker's vertex buffer: where each painted vertex came from
static f32 photo_clock;          // the world's clock as a photo recorded it
static u8 tinted[MAP_BYTES];     // one bit per 8 bytes: this vertex or command is done
static u8 walked[MAP_BYTES];     // one bit per 8 bytes: the model that starts here is done
static Gfx* entry_pos[4];        // where the four command lists stood as a model began
static u32 tex_addr;             // the walker's reading of the picture set to load (G_SETTIMG),
static u32 tex_wide;             // the bytes a unit of its load counts (4 for 32-bit pictures),
static u32 tex_bytes;            // the size of the last block load,
static u32 tlut_addr;            // the palette last loaded and its entries,
static u32 tlut_n;
static u8 tile_fmt, tile_siz;    // and the drawing tile's picture format

static s32 unit_of(u32 addr) {
    if (addr >= LOW_LO && addr < LOW_HI) {
        return (s32) ((addr - LOW_LO) >> 3);
    }
    if (on_the_ride) {
        if (addr >= RIDE_LO && addr < RIDE_HI) {
            return (s32) (LOW_UNITS + ((addr - RIDE_LO) >> 3));
        }
        if (addr >= LEVEL_LO && addr < LEVEL_HI) {
            return (s32) (LOW_UNITS + RIDE_UNITS + ((addr - LEVEL_LO) >> 3));
        }
    } else if (addr >= PHOTO_LO && addr < LEVEL_HI) {
        return (s32) (LOW_UNITS + RIDE_UNITS + ((addr - LEVEL_LO) >> 3));
    }
    return -1;
}

// Marks the address in a map. 1 when it was not marked before; 0 when it
// was, or when the address is outside what a course loads.
static s32 mark(u8* map, u32 addr) {
    s32 unit = unit_of(addr);
    u8 bit;

    if (unit < 0) {
        return 0;
    }
    bit = (u8) (1 << (unit & 7));
    if (map[unit >> 3] & bit) {
        return 0;
    }
    map[unit >> 3] |= bit;
    return 1;
}

static s32 marked(u8* map, u32 addr) {
    s32 unit = unit_of(addr);

    return unit >= 0 && (map[unit >> 3] & (1 << (unit & 7))) != 0;
}

static void clear_maps(void) {
    volatile u8* a = tinted;
    volatile u8* b = walked;
    u32 i;

    for (i = 0; i < MAP_BYTES; i++) {
        a[i] = 0;
        b[i] = 0;
    }
}

static u32 part(u32 value, u32 of256) {
    return (value * of256) >> 8;
}

// A light color as the display list carries it: red, green, blue in the
// top three bytes.
// A light of 255 is the whole: 256 parts, so a day color comes back exact.
static u32 whole(u32 light) {
    return light >= 255 ? 256 : light;
}

static u32 night_word(u32 word) {
    return (part(word >> 24, whole(light_body[0])) << 24) | (part((word >> 16) & 0xFF, whole(light_body[1])) << 16) |
           (part((word >> 8) & 0xFF, whole(light_body[2])) << 8) | (word & 0xFF);
}

// The night's light falls on one painted vertex, once: the scenery's, a
// body's, or -- under the material that flows -- the water's or the lava's.
static u32 snow_channel(u32 channel, u32 white, u32 bright);

#define SNOW_R 236
#define SNOW_G 240
#define SNOW_B 252

static void light_vertex(u32 at, s32 steep, s32 toward) {
    if (at != 0 && mark(tinted, at)) {
        volatile u8* color = (volatile u8*) (at + 12);
        const u8* light = flowing ? light_flow
                        : (at >= night->world_lo && at < night->world_hi) ? light_world : light_body;

        // The water's height in the world: the vertex's, plus its block's place.
        // Only the course's own water counts: the Zero-One has parts of the
        // same two-picture material.
        if (flowing && at >= night->world_lo && at < night->world_hi) {
            f32 y = (f32) ((volatile s16*) at)[1] + walk_node_y;

            flow_y_sum += y;
            if (flow_y_n == 0 || y > flow_y_max) {
                flow_y_max = y;
            }
            if (flow_y_n == 0 || y < flow_y_min) {
                flow_y_min = y;
            }
            flow_y_n++;
        }
#ifdef NIGHT_MAP
        if (at >= night->world_lo && at < night->world_hi) {
            f32 wx = (f32) ((volatile s16*) at)[0] + walk_node_x;
            f32 wz = (f32) ((volatile s16*) at)[2] + walk_node_z;
            s32 c = map_cell(wx, wz);

            if (c >= 0) {
                if (flowing) {
                    map_kind[c] |= 2;
                    map_water_y[c] += (f32) ((volatile s16*) at)[1] + walk_node_y;
                    map_water_n[c]++;
                } else {
                    map_kind[c] |= 1;
                }
            }
        }
#endif

        // Under snow, the land's painted colors (much of the land's color is
        // painted, over gray pictures) go to snow first, except where the
        // face stands steep: there the rock shows, darker and colder.
        if (snowing && !flowing && !steep) {
            u32 bright = 159 + (97 * ((color[0] * 77 + color[1] * 151 + color[2] * 28) >> 8)) / 255;

            color[0] = (u8) snow_channel(color[0], SNOW_R, bright);
            color[1] = (u8) snow_channel(color[1], SNOW_G, bright);
            color[2] = (u8) snow_channel(color[2], SNOW_B, bright);
        }
        {
            s32 k;

            for (k = 0; k < 3; k++) {
                u32 lit = light[k] + (flowing ? 0 : ((land_diff[k] * (u32) toward) >> 8));

                color[k] = (u8) part(color[k], whole(lit));
            }
        }
        if (steep) {
            color[0] = (u8) part(color[0], steep_shade[0]);
            color[1] = (u8) part(color[1], steep_shade[1]);
            color[2] = (u8) part(color[2], steep_shade[2]);
        }
    }
}

// Snow lies on the land. Under snow, the land's own pictures are painted
// over white where they lie, once each, as the first triangle of the land
// that shows them is met: a picture drawn through a palette has the palette
// painted; one of plain colors, or of intensities, has its texels painted.
// The Pokémon and the things that move are lit by the scene's light, not
// painted, so their pictures are never touched; nor is the water's, nor
// the sky's.

// A color under snow: mostly snow, brighter where the color was bright, with
// a little of the color itself left in it.
static u32 snow_channel(u32 channel, u32 white, u32 bright) {
    u32 value = (38 * channel + 218 * ((white * bright) >> 8)) >> 8;

    return value > 255 ? 255 : value;
}

static void snow_colors(volatile u16* texel, u32 count) {
    u32 i;

    for (i = 0; i < count; i++) {
        u32 t = texel[i];
        u32 r = (((t >> 11) & 31) * 255) / 31;
        u32 g = (((t >> 6) & 31) * 255) / 31;
        u32 b = (((t >> 1) & 31) * 255) / 31;
        u32 bright = 159 + (97 * ((r * 77 + g * 151 + b * 28) >> 8)) / 255;

        r = snow_channel(r, SNOW_R, bright);
        g = snow_channel(g, SNOW_G, bright);
        b = snow_channel(b, SNOW_B, bright);
        texel[i] = (u16) (((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (t & 1));
    }
}

static void snow_intensities(volatile u8* value, u32 count, u32 step) {
    u32 i;

    for (i = 0; i < count; i++) {
        u32 bright = 159 + (97 * value[i * step]) / 255;

        value[i * step] = (u8) snow_channel(value[i * step], SNOW_R, bright);
    }
}

static void snow_picture(void) {
    if (tex_addr == night->pics[0] || tex_addr == night->pics[1]) {
        return;
    }
    switch (tile_fmt) {
        case 2:     // color index: the palette
            if (tlut_addr != 0 && mark(tinted, tlut_addr)) {
                snow_colors((volatile u16*) tlut_addr, tlut_n);
            }
            break;
        case 0:     // red, green, blue, alpha
            if (tile_siz == 2 && mark(tinted, tex_addr)) {
                snow_colors((volatile u16*) tex_addr, tex_bytes / 2);
            }
            break;
        case 3:     // intensity and alpha
            if (tile_siz == 2 && mark(tinted, tex_addr)) {
                snow_intensities((volatile u8*) tex_addr, tex_bytes / 2, 2);
            }
            break;
        case 4:     // intensity
            if (tile_siz == 1 && mark(tinted, tex_addr)) {
                snow_intensities((volatile u8*) tex_addr, tex_bytes, 1);
            }
            break;
        default:
            break;
    }
}

// A face that stands steep holds no snow: its corners keep a darker, colder
// shade. 1 when less than a third of the face's normal stands upright.
static s32 steep_face(u32 a, u32 b, u32 c) {
    const volatile s16* A = (const volatile s16*) a;
    const volatile s16* B = (const volatile s16*) b;
    const volatile s16* C = (const volatile s16*) c;
    f32 ux = (f32) (B[0] - A[0]), uy = (f32) (B[1] - A[1]), uz = (f32) (B[2] - A[2]);
    f32 vx = (f32) (C[0] - A[0]), vy = (f32) (C[1] - A[1]), vz = (f32) (C[2] - A[2]);
    f32 nx = uy * vz - uz * vy;
    f32 ny = uz * vx - ux * vz;
    f32 nz = ux * vy - uy * vx;
    f32 all = nx * nx + ny * ny + nz * nz;

    return all > 1.0f && ny * ny < 0.12f * all;
}

// How much a face faces the light (the sun, or the moon), 0 to 256: the
// face's normal from its corners' winding against relight_dir.
static s32 face_toward(u32 a, u32 b, u32 c) {
    const volatile s16* A = (const volatile s16*) a;
    const volatile s16* B = (const volatile s16*) b;
    const volatile s16* C = (const volatile s16*) c;
    f32 ux = (f32) (B[0] - A[0]), uy = (f32) (B[1] - A[1]), uz = (f32) (B[2] - A[2]);
    f32 vx = (f32) (C[0] - A[0]), vy = (f32) (C[1] - A[1]), vz = (f32) (C[2] - A[2]);
    f32 nx = uy * vz - uz * vy;
    f32 ny = uz * vx - ux * vz;
    f32 nz = ux * vy - uy * vx;
    f32 len = __builtin_sqrtf(nx * nx + ny * ny + nz * nz);
    f32 d;

    if (len < 1.0f) {
        return 0;
    }
    d = (nx * relight_dir[0] + ny * relight_dir[1] + nz * relight_dir[2]) / len;
    if (d <= 0.0f) {
        return 0;
    }
    return (s32) (d * 256.0f);
}

// A corner's lighting, kept with its copy for the dusk: the first face that
// lights it decides, as light_vertex does.
static void glint_face_set(u32 slot, s32 steep, s32 toward) {
    u32 o = glint_op[glint_slot_op[slot]];
    u32 copy = ((o >> 12) & 0x1FFF) + (slot - (o & 63));

    if (copy < GLINT_VTX && glint_face[copy] == 0) {
        glint_face[copy] = (u16) (0x8000u | (steep ? 0x4000u : 0u) | (flowing ? 0x2000u : 0u) | ((u32) toward & 0x1FFu));
    }
}

// The three corners of a triangle command's word. Under snow, a triangle of
// the land also brings the picture it shows under the snow.
static void light_corners(u32 word) {
    u32 a = ((word >> 16) & 0xFF) >> 1;
    u32 b = ((word >> 8) & 0xFF) >> 1;
    u32 c = (word & 0xFF) >> 1;

    if (a < 64 && b < 64 && c < 64) {
        s32 steep = 0;
        s32 toward = 0;

        if (!flowing && slots[a] != 0 && slots[b] != 0 && slots[c] != 0) {
            if (snowing) {
                if (tex_addr != 0) {
                    snow_picture();
                }
                steep = steep_face(slots[a], slots[b], slots[c]);
            }
            toward = face_toward(slots[a], slots[b], slots[c]);
        }
        if (glint_recording && glint_slot_ok[a] && glint_slot_ok[b] && glint_slot_ok[c] && glint_op_n < GLINT_OPS) {
            u32 used = flowing ? GLINT_USED : GLINT_LAND;

            glint_op[glint_op_n++] = (a << 16) | (b << 8) | c | (flowing ? GLINT_FLOW : 0);
            glint_face_set(a, steep, toward);
            glint_face_set(b, steep, toward);
            glint_face_set(c, steep, toward);
            glint_op[glint_slot_op[a]] |= used;
            glint_op[glint_slot_op[b]] |= used;
            glint_op[glint_slot_op[c]] |= used;
            if (flowing) {
                glint_tris++;
            } else {
                glint_land++;
            }
        }
        light_vertex(slots[a], steep, toward);
        light_vertex(slots[b], steep, toward);
        light_vertex(slots[c], steep, toward);
    }
}

// Reads a display list of the course's own data and gives the night's light
// to what colors it: the vertices loaded while lighting is off (their fourth
// word is a color then, a normal otherwise), each as a triangle first uses
// it, and the light colors a list sets for itself. The material in force is
// followed by its combiner: the one that blends two pictures is the one the
// courses use for what flows. Lists reached by a call or by the near/far
// branch are read too.
static void walk(u32 list, s32 depth) {
    u32 half = 0;
    s32 budget = 20000;

    if (depth > 12 || (list & 7) || unit_of(list) < 0) {
        return;
    }
    while (budget-- > 0 && unit_of(list) >= 0) {
        volatile u32* cmd = (volatile u32*) list;
        u32 w0 = cmd[0];
        u32 w1 = cmd[1];

        switch (w0 >> 24) {
            case 0x01: {    // G_VTX: count, and one past the last slot filled
                u32 count = (w0 >> 12) & 0xFF;
                u32 first = ((w0 >> 1) & 0x7F) - count;
                u32 i;

                for (i = 0; i < count && first + i < 64; i++) {
                    slots[first + i] = (lit || (w1 & 7)) ? 0 : w1 + 16 * i;
                }
                if (glint_recording && !lit && (w1 & 7) == 0 && w1 >= night->world_lo && w1 < night->world_hi
                    && count <= 32 && glint_vtx_n + count <= GLINT_VTX && glint_op_n < GLINT_OPS) {
                    const volatile u32* src = (const volatile u32*) w1;
                    u32* dst0 = (u32*) &glint_vtx[0][glint_vtx_n];
                    u32* dst1 = (u32*) &glint_vtx[1][glint_vtx_n];

                    for (i = 0; i < count * 4; i++) {
                        dst0[i] = dst1[i] = src[i];
                    }
                    for (i = 0; i < count && first + i < 64; i++) {
                        glint_slot_ok[first + i] = 1;
                        glint_slot_op[first + i] = glint_op_n;
                    }
                    glint_load_src[glint_op_n] = w1;
                    for (i = 0; i < count; i++) {
                        glint_orig_cn[glint_vtx_n + i] = ((u32*) &glint_vtx[0][glint_vtx_n + i])[3];
                        glint_face[glint_vtx_n + i] = 0;
                    }
                    glint_op[glint_op_n++] = GLINT_LOAD | ((u32) glint_vtx_n << 12) | (count << 6) | (first & 63);
                    glint_vtx_n = (u16) (glint_vtx_n + count);
                } else {
                    for (i = 0; i < count && first + i < 64; i++) {
                        glint_slot_ok[first + i] = 0;
                    }
                }
                break;
            }
            case 0x05:      // G_TRI1
                light_corners(w0);
                break;
            case 0x06:      // G_TRI2
            case 0x07:      // G_QUAD
                light_corners(w0);
                light_corners(w1);
                break;
            case 0xFC:      // G_SETCOMBINE
                flowing = ((w0 & 0x00FFFFFF) == 0x272C04 && w1 == 0x1F0C93FF);
                break;
            case 0xFD:      // G_SETTIMG: the picture to load next
                tex_addr = w1;
                tex_wide = (((w0 >> 19) & 3) == 3) ? 4 : 2;
                break;
            case 0xF3:      // G_LOADBLOCK: the load's size, in its own units
                tex_bytes = (((w1 >> 12) & 0xFFF) + 1) * tex_wide;
                break;
            case 0xF0:      // G_LOADTLUT: a palette, and its entries
                tlut_addr = tex_addr;
                tlut_n = ((w1 >> 14) & 0x3FF) + 1;
                break;
            case 0xF5:      // G_SETTILE, the drawing tile: the picture's format
                if (((w1 >> 24) & 7) == 0) {
                    tile_fmt = (u8) ((w0 >> 21) & 7);
                    tile_siz = (u8) ((w0 >> 19) & 3);
                }
                break;
            case 0xD9:      // G_GEOMETRYMODE: clear, then set
                if (~w0 & G_LIGHTING_BIT) {
                    lit = 0;
                }
                if (w1 & G_LIGHTING_BIT) {
                    lit = 1;
                }
                break;
            case 0xDB:      // G_MOVEWORD, light color
                if (((w0 >> 16) & 0xFF) == 0x0A && mark(tinted, list)) {
                    cmd[1] = night_word(w1);
                }
                break;
            case 0xE1:      // G_RDPHALF_1: the target of the branch that follows
                half = w1;
                break;
            case 0x04:      // G_BRANCH_Z: the near or far list
                walk(half, depth + 1);
                break;
            case 0xDE:      // G_DL
                walk(w1, depth + 1);
                if ((w0 >> 16) & 0xFF) {
                    return;
                }
                break;
            case 0xDF:      // G_ENDDL
                return;
            default:
                break;
        }
        list += 8;
    }
}

static DObj* after(DObj* node) {
    if (node->firstChild != NULL) {
        return node->firstChild;
    }
    while (node != NULL && (u32) node != 1) {
        if (node->next != NULL) {
            return node->next;
        }
        node = node->parent;
    }
    return NULL;
}

// A list a node calls: remembered, so the glint can find the node's matrix
// among the commands the game writes, then read.
static void walk_top(u32 list) {
    if (glint_top_n < 4) {
        glint_top[glint_top_n++] = list;
    }
    walk(list, 0);
}

static void walk_payload(DObj* node, s32 kind) {
    void* payload = node->payload.any;
    DObj* up = node;
    s32 guard = 8;
    u16 op_before, tris_before, land_before;

    if (payload == NULL) {
        return;
    }
    walk_node_y = 0.0f;
#ifdef NIGHT_MAP
    walk_node_x = walk_node_z = 0.0f;
#endif
    while (up != NULL && (u32) up != 1 && guard-- > 0) {
        walk_node_y += up->position.v.y;
#ifdef NIGHT_MAP
        walk_node_x += up->position.v.x;
        walk_node_z += up->position.v.z;
#endif
        up = up->parent;
    }
    glint_top_n = 0;
    op_before = glint_op_n;
    tris_before = glint_tris;
    land_before = glint_land;
    {
        s32 k;

        for (k = 0; k < 64; k++) {
            glint_slot_ok[k] = 0;
        }
    }
    switch (kind) {
        case 'A':
        case 'B':
            walk_top((u32) node->payload.dlist);
            break;
        case 'C':
        case 'D': {
            DObjPayloadTypeC* entry = node->payload.typeC;
            s32 guard = 8;

            while (unit_of((u32) entry) >= 0 && entry->dlistID != 4 && guard-- > 0) {
                walk_top((u32) entry->dlist);
                entry++;
            }
            break;
        }
        case 'I': {
            DObjPayloadTypeI* pair = node->payload.typeI;

            if (unit_of((u32) pair) >= 0) {
                walk_top((u32) pair->unk_00);
                walk_top((u32) pair->unk_04);
            }
            break;
        }
        case 'J': {
            DObjPayloadTypeJ* entry = node->payload.typeJ;
            s32 guard = 8;

            while (unit_of((u32) entry) >= 0 && entry->dlistID != 4 && guard-- > 0) {
                walk_top((u32) entry->unk_04);
                walk_top((u32) entry->unk_08);
                entry++;
            }
            break;
        }
    }
    // The node drew water: its loads and triangles are a segment of the glint.
    if (glint_recording && (glint_tris > tris_before || glint_land > land_before) && glint_seg_n < GLINT_SEGS) {
        GlintSeg* seg = &glint_seg[glint_seg_n++];
        s32 k;

        for (k = 0; k < glint_top_n; k++) {
            seg->lists[k] = glint_top[k];
        }
        seg->lists_n = (u8) glint_top_n;
        seg->model = glint_model;
        seg->op0 = op_before;
        seg->op_n = (u16) (glint_op_n - op_before);
        seg->flow = (glint_tris > tris_before);
    }
}

// A model about to be drawn. The first time each model of the course comes
// by, its lists are read and given the night's light; after that the model
// is known by its first list and passed over.
static void model_enters(GObj* obj, s32 kind) {
    DObj* node;
    DObj* first = NULL;
    s32 guard = 400;
    s32 i;

    for (i = 0; i < 4; i++) {
        entry_pos[i] = MAIN_GFX_POS[i];
    }
    if (night == NULL || obj == NULL || obj->type != 1) {
        return;
    }
    for (node = obj->data.dobj; node != NULL && guard-- > 0; node = after(node)) {
        if (node->payload.any != NULL) {
            first = node;
            break;
        }
    }
    if (first == NULL || unit_of((u32) first->payload.any) < 0) {
        return;
    }
    drawing_water = (water_model != 0 && (u32) first->payload.any == water_model);
    drawing_glint = 0;
    {
        s32 k;

        for (k = 0; k < glint_seg_n; k++) {
            if (glint_seg[k].model == (u32) first->payload.any) {
                drawing_glint |= glint_seg[k].flow;
            }
        }
    }
    if (marked(walked, (u32) first->payload.any)) {
        return;
    }
    mark(walked, (u32) first->payload.any);
    lit = 1;
    flowing = 0;
    tex_addr = 0;
    tlut_addr = 0;
    tile_fmt = 0xFF;
    for (i = 0; i < 64; i++) {
        slots[i] = 0;
    }
    guard = 400;
    {
        u32 water_before = flow_y_n;
        u16 vtx_before = glint_vtx_n, op_before = glint_op_n, tris_before = glint_tris, land_before = glint_land;
        u8 seg_before = glint_seg_n;

        glint_model = (u32) first->payload.any;
        glint_recording = (glint_seg_n < GLINT_SEGS);
        for (node = first; node != NULL && guard-- > 0; node = after(node)) {
            walk_payload(node, kind);
        }
        glint_recording = 0;
        if (glint_tris == tris_before && glint_land == land_before) {
            // Nothing of the world in this model: what it loaded is dropped again.
            glint_vtx_n = vtx_before;
            glint_op_n = op_before;
            glint_seg_n = seg_before;
        } else {
            drawing_glint = (glint_tris > tris_before);
        }
        if (flow_y_n > water_before && water_model == 0) {
            water_model = (u32) first->payload.any;
            drawing_water = 1;
        }
    }
}

static void moon_glint(void);
static void embers(void);
static void cycle_tick(void);
static void fireflies(GObj* sky);
static GObj* sky_now;           // the sky being drawn, for the camera's place

// A model just drawn. A material may carry light colors of its own, which
// the game writes into the frame's command lists as it draws; those written
// for this model are given the night's light here, every frame, since the
// game writes them anew every frame.
static void model_leaves(void) {
    s32 i;

    if (night == NULL) {
        return;
    }
    for (i = 0; i < 4; i++) {
        volatile u32* cmd = (volatile u32*) entry_pos[i];
        volatile u32* end = (volatile u32*) MAIN_GFX_POS[i];
        s32 budget = 4096;

        if (cmd == NULL || end < cmd) {
            continue;
        }
        while (cmd < end && budget-- > 0) {
            if ((cmd[0] >> 16) == 0xDB0A) {
                cmd[1] = night_word(cmd[1]);
            }
            cmd += 2;
        }
    }
    if (drawing_glint && night->glint && glows_on && !sunset && !scoring && !raining && !snowing && sky_now != NULL) {
        drawing_glint = 0;
        moon_glint();
    } else if (drawing_glint && night->embers && !scoring && sky_now != NULL) {
        drawing_glint = 0;
        embers();
    }
    // The water was just drawn: what glows on it or over it is drawn now, so
    // that the water, which is drawn among the see-through things and writes
    // no depth, does not paint over it.
    if (drawing_water && glows_on && !sunset && !scoring && !raining && !snowing && sky_now != NULL) {
        drawing_water = 0;
        glow_turn ^= 1;
        if (night->fireflies != NULL) {
            fireflies(sky_now);
        }
    }
}

RECOMP_HOOK("renRenderModelTypeA") void night_model_a(GObj* obj) { model_enters(obj, 'A'); }
RECOMP_HOOK("renRenderModelTypeB") void night_model_b(GObj* obj) { model_enters(obj, 'B'); }
RECOMP_HOOK("renRenderModelTypeC") void night_model_c(GObj* obj) { model_enters(obj, 'C'); }
RECOMP_HOOK("renRenderModelTypeD") void night_model_d(GObj* obj) { model_enters(obj, 'D'); }
RECOMP_HOOK("renRenderModelTypeI") void night_model_i(GObj* obj) { model_enters(obj, 'I'); }
RECOMP_HOOK("renRenderModelTypeJ") void night_model_j(GObj* obj) { model_enters(obj, 'J'); }

RECOMP_HOOK_RETURN("renRenderModelTypeA") void night_model_a_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeB") void night_model_b_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeC") void night_model_c_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeD") void night_model_d_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeI") void night_model_i_done(void) { model_leaves(); }
RECOMP_HOOK_RETURN("renRenderModelTypeJ") void night_model_j_done(void) { model_leaves(); }

// The scene light: ambient at 0 and diffuse at 8, each kept twice by the
// game's light record.
static void set_light(const Night* by) {
    volatile u8* light = COURSE_LIGHT;
    s32 i;

    for (i = 0; i < 3; i++) {
        s32 day_a = (day_ambient[i] != 0 || day_diffuse[i] != 0) ? day_ambient[i] : DAY_AMBIENT;
        s32 day_d = (day_ambient[i] != 0 || day_diffuse[i] != 0) ? day_diffuse[i] : DAY_DIFFUSE;
        s32 share = (s32) (nightness * 256.0f);
        s32 ambient = (by != NULL) ? day_a + (((s32) scene_ambient[i] - day_a) * share) / 256 : day_a;
        s32 diffuse = (by != NULL) ? day_d + (((s32) scene_diffuse[i] - day_d) * share) / 256 : day_d;

        light[i] = light[4 + i] = (u8) ambient;
        light[8 + i] = light[12 + i] = (u8) diffuse;
    }
}

// A sky picture, recolored where it lies. The night keeps the day picture's
// own light and shade -- its luminance, stretched between the darkest open
// sky and the brightest cloud and softened a little -- and paints it again
// as night sky under night cloud, both kept BRIGHT. The dark comes from the
// box's corners (sky_corners), which blend in eight bits across the dome, as
// the game's own corners carry the day's blue above and haze below. A
// picture of five bits a channel cannot hold a dark gradient without flecks;
// a bright one can.
// A sky picture painted from its cloud mask with the colors in force.
// A texel of the cartridge's picture as the day end of the blend: itself, or
// under rain, snow or ash, grayed and a little darker (an overcast day).
static u32 day_texel(s32 which, s32 i, s32 k) {
    u16 t = sky_orig_pic[which][i];
    u32 c[3];

    c[0] = ((t >> 11) & 31) * 255 / 31;
    c[1] = ((t >> 6) & 31) * 255 / 31;
    c[2] = ((t >> 1) & 31) * 255 / 31;
    if (raining || snowing || ashing) {
        u32 gray = (c[0] * 77 + c[1] * 151 + c[2] * 28) >> 8;

        return ((c[k] * 115 + gray * 141) * 218) >> 16;
    }
    return c[k];
}

static void sky_paint(u32 pic, const u8* mask, s32 which) {
    s32 share = (s32) (nightness * 256.0f);
    volatile u16* texel = (volatile u16*) pic;
    s32 i;

    for (i = 0; i < SKY_TEXELS; i++) {
        s32 m = mask[i];
        s32 row = i >> 6;
        u32 c[3];
        s32 k;

        if (cloud_top_fade != 0 && row > 12) {
            // At sunset the clouds thin away toward the top of the drum, so
            // its flat lid (the picture's top row, one color) meets the sides
            // without a rim, and the picture's six repeats around the sky
            // crowd less where they are closest.
            s32 f = ((row - 12) * 255) / 19;

            m -= ((((m * f) / 255) * f) / 255) * cloud_top_fade / 255;
        }
        for (k = 0; k < 3; k++) {
            c[k] = (u32) (pic_sky[k] + (((s32) pic_cloud[k] - pic_sky[k]) * m) / 255);
        }
        if (share < 256) {
            for (k = 0; k < 3; k++) {
                u32 day = day_texel(which, i, k);

                c[k] = day + (((s32) c[k] - (s32) day) * share) / 256;
            }
        }
        texel[i] = (u16) (((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | 1);
    }
}

// The cartridge's picture meets itself at the drum's joins with a step, as
// the mask does (sky_at_night); the kept copy takes the same dissolve the
// port gives the picture it draws, so the day end of the blend has none.
static void dissolve_orig(s32 which) {
    u16* pic = sky_orig_pic[which];
    u32 best = 0xFFFFFFFFu;
    s32 best_o = 10, o, x, y, d, k;
    u16 keep[64];

    for (o = 10; o + 20 <= 54; o++) {
        u32 light = 0;

        for (y = 0; y < 32; y++) {
            for (x = o; x < o + 20; x++) {
                u16 t = pic[y * 64 + x];

                light += ((t >> 11) & 31) + ((t >> 6) & 31) + ((t >> 1) & 31);
            }
        }
        if (light < best) {
            best = light;
            best_o = o;
        }
    }
    for (y = 0; y < 32; y++) {
        u16* row = &pic[y * 64];

        for (x = 0; x < 64; x++) {
            keep[x] = row[x];
        }
        for (d = 0; d < 10; d++) {
            s32 wq = ((10 - d) * 256) / 10;
            s32 side;

            wq = (wq * wq * (3 * 256 - 2 * wq)) / (256 * 256);
            for (side = 0; side < 2; side++) {
                s32 at = side ? (63 - d) : d;
                s32 from = side ? (best_o + 10 - d) : (best_o + 10 + d);
                u16 t0 = keep[at], t1 = keep[from];
                s32 c0[3], c1[3], c[3];

                c0[0] = (t0 >> 11) & 31; c0[1] = (t0 >> 6) & 31; c0[2] = (t0 >> 1) & 31;
                c1[0] = (t1 >> 11) & 31; c1[1] = (t1 >> 6) & 31; c1[2] = (t1 >> 1) & 31;
                for (k = 0; k < 3; k++) {
                    c[k] = c0[k] + (((c1[k] - c0[k]) * wq + 128) >> 8);
                    c[k] = c[k] < 0 ? 0 : c[k] > 31 ? 31 : c[k];
                }
                row[at] = (u16) ((c[0] << 11) | (c[1] << 6) | (c[2] << 1) | (t0 & 1));
            }
        }
    }
}

static void sky_at_night(u32 pic, s32 which) {
    volatile u16* texel = (volatile u16*) pic;
    u32 bins[32];
    s32 lo = 0;
    s32 hi = 255;
    s32 count;
    s32 i;

    // The picture as the cartridge has it, kept for the day end of every blend.
    for (i = 0; i < SKY_TEXELS; i++) {
        sky_orig_pic[which][i] = texel[i];
    }
    dissolve_orig(which);
    for (i = 0; i < 32; i++) {
        bins[i] = 0;
    }
    for (i = 0; i < SKY_TEXELS; i++) {
        u32 t = texel[i];
        u32 r = (((t >> 11) & 31) * 255) / 31;
        u32 g = (((t >> 6) & 31) * 255) / 31;
        u32 b = (((t >> 1) & 31) * 255) / 31;
        u32 lum = (r * 77 + g * 151 + b * 28) >> 8;

        sky_mask[i] = (u8) lum;
        bins[lum >> 3]++;
    }
    // The stretch runs from the 3rd to the 97th hundredth of the texels.
    count = 0;
    for (i = 0; i < 32; i++) {
        count += bins[i];
        if (count >= SKY_TEXELS * 3 / 100) {
            lo = i * 8;
            break;
        }
    }
    count = 0;
    for (i = 31; i >= 0; i--) {
        count += bins[i];
        if (count >= SKY_TEXELS * 3 / 100) {
            hi = i * 8 + 7;
            break;
        }
    }
    if (hi - lo < 16) {
        hi = lo + 16;
    }
    for (i = 0; i < SKY_TEXELS; i++) {
        s32 v = (((s32) sky_mask[i] - lo) * 255) / (hi - lo);

        sky_blur[i] = (u8) (v < 0 ? 0 : v > 255 ? 255 : v);
    }
    // Softened: each texel with its neighbors, 1-2-1 both ways.
    for (i = 0; i < SKY_TEXELS; i++) {
        s32 x = i & 63;
        s32 y = i >> 6;
        s32 xl = x > 0 ? x - 1 : x;
        s32 xr = x < 63 ? x + 1 : x;
        s32 yu = y > 0 ? y - 1 : y;
        s32 yd = y < 31 ? y + 1 : y;
        u32 sum = 4 * sky_blur[y * 64 + x]
                + 2 * (sky_blur[y * 64 + xl] + sky_blur[y * 64 + xr] + sky_blur[yu * 64 + x] + sky_blur[yd * 64 + x])
                + sky_blur[yu * 64 + xl] + sky_blur[yu * 64 + xr] + sky_blur[yd * 64 + xl] + sky_blur[yd * 64 + xr];

        sky_mask[i] = (u8) (sum >> 4);
    }
    // The picture meets itself at each of the drum's six edges, its last
    // column against its first, and the two do not agree (a cloud begins at
    // the first). The ten columns either side of the join dissolve into the
    // mask's clearest stretch of twenty, laid across the join so the cloud
    // runs on, as the port does to the day's picture (patches/src/sky_patch.c).
    {
        u32 best = 0xFFFFFFFFu;
        s32 best_o = 10, o, x, d;
        u8 keep[64];

        for (o = 10; o + 20 <= 54; o++) {
            u32 cloud = 0;

            for (i = 0; i < 32; i++) {
                for (x = o; x < o + 20; x++) {
                    cloud += sky_mask[i * 64 + x];
                }
            }
            if (cloud < best) {
                best = cloud;
                best_o = o;
            }
        }
        for (i = 0; i < 32; i++) {
            u8* row = &sky_mask[i * 64];

            for (x = 0; x < 64; x++) {
                keep[x] = row[x];
            }
            for (d = 0; d < 10; d++) {
                s32 w = ((10 - d) * 256) / 10;

                w = (w * w * (3 * 256 - 2 * w)) / (256 * 256);
                row[d] = (u8) (keep[d] + ((((s32) keep[best_o + 10 + d] - keep[d]) * w + 128) >> 8));
                row[63 - d] = (u8) (keep[63 - d] + ((((s32) keep[best_o + 10 - d] - keep[63 - d]) * w + 128) >> 8));
            }
        }
    }
}

// How low and how high the sky box's corners reach, as sines of elevation,
// so that the lowest ring takes the horizon's tint and the highest the top's.
static void sky_extent(const Night* by) {
    s32 i, j;

    sky_sin_lo = 1.0f;
    sky_sin_hi = -1.0f;
    for (i = 0; i < 2; i++) {
        for (j = 0; j < by->sky_vtx_n[i]; j++) {
            volatile s16* pos = (volatile s16*) (by->sky_vtx[i] + 16 * j);
            f32 x = pos[0];
            f32 y = pos[1];
            f32 z = pos[2];
            f32 len = __builtin_sqrtf(x * x + y * y + z * z);

            if (len > 1.0f) {
                f32 sine = y / len;

                if (sine < sky_sin_lo) {
                    sky_sin_lo = sine;
                }
                if (sine > sky_sin_hi) {
                    sky_sin_hi = sine;
                }
            }
        }
    }
    if (sky_sin_hi - sky_sin_lo < 0.1f) {
        sky_sin_hi = sky_sin_lo + 0.1f;
    }
}

// The sky box's corners carry the night: each corner's color is the tint at
// the horizon blended toward the tint at the top by the corner's height, as
// the game's own corners carry the day's blue above and haze below. The
// walker is told these are done, so the scenery's light does not fall on
// the sky.
static void sky_corners(u32 addr, s32 count, s32 which) {
    s32 i;

    for (i = 0; i < count; i++) {
        volatile s16* pos = (volatile s16*) (addr + 16 * i);
        volatile u8* color = (volatile u8*) (addr + 16 * i + 12);
        f32 x = pos[0];
        f32 y = pos[1];
        f32 z = pos[2];
        f32 len = __builtin_sqrtf(x * x + y * y + z * z);
        f32 up = len > 1.0f ? (y / len - sky_sin_lo) / (sky_sin_hi - sky_sin_lo) : 0.0f;
        f32 flat = __builtin_sqrtf(x * x + z * z);
        f32 facing = flat > 1.0f ? (x * relight_dir[0] + z * relight_dir[2]) / flat : 0.0f;
        s32 share;
        s32 toward;
        s32 k;

        if (up < 0.0f) {
            up = 0.0f;
        }
        if (up > 1.0f) {
            up = 1.0f;
        }
        share = (s32) (up * 256.0f);        // the height's share, horizon to top
        toward = (s32) ((0.5f + 0.5f * facing) * 256.0f);
        if (!sky_orig_cn_kept[which] && i < 64) {
            sky_orig_cn[which][i] = ((u32) color[0] << 16) | ((u32) color[1] << 8) | color[2];
        }
        for (k = 0; k < 3; k++) {
            s32 low = tint_away[k] + (((s32) tint_lo[k] - tint_away[k]) * toward) / 256;
            s32 tint = low + (((s32) tint_hi[k] - low) * share) / 256;
            s32 day = (i < 64) ? (s32) ((sky_orig_cn[which][i] >> (16 - 8 * k)) & 0xFF) : tint;

            color[k] = (u8) (day + ((tint - day) * (s32) (nightness * 256.0f)) / 256);
        }
        mark(tinted, addr + 16 * i);
    }
}

// The course's time in ticks (sixtieths of a second: the game takes two
// steps for each frame it draws): the world's own clock
// on the ride, the clock a photo recorded when it is a photo's world. The
// clock advances by the set-up's sky speed each tick.
static u32 ticks_of(const Night* by) {
    WorldSetup* setup = (WorldSetup*) by->setup;
    f32 clock = on_the_ride ? GlobalTimer : photo_clock;
    f32 speed = setup->unk_10;

    if (speed < 0.05f) {
        speed = 1.0f;
    }
    return (u32) (clock / speed);
}

// The stars twinkle. Each group's brightness is its base, plus two slow
// waves of different lengths so that it never quite repeats, plus -- for
// some -- a brief swell now and then. Most groups are calm, some breathe, a
// few shimmer; none flashes. The clock is the world's own (a tick is a
// sixtieth of a second) on the ride, and the clock a photo recorded when
// it is a photo's sky, so a photo keeps the twinkle of its instant.
static void twinkle(const Night* by) {
    u32 ticks = ticks_of(by);
    s32 i;

    for (i = 0; i < by->twinkles && i < NIGHT_GROUPS_MAX; i++) {
        const NightTwinkle* group = &night_twinkles[i];
        s32 one = night_sine[((group->phase1 + ticks * group->rate1) >> 8) & 0xFF];
        s32 two = night_sine[((group->phase2 + ticks * group->rate2) >> 8) & 0xFF];
        s32 value = group->base + (group->swing * (3 * one + 2 * two)) / (5 * 127);

        if (group->swell != 0) {
            s32 at = (s32) ((group->phase3 + ticks * group->rate3) & 0xFFFF);

            if (at < 0x0C00) {
                value += (group->swell * (at < 0x0600 ? at : 0x0C00 - at)) / 0x0600;
            }
        }
        if (value < 40) {
            value = 40;
        }
        if (value > 255) {
            value = 255;
        }
        value = (value * (s32) star_fade) / 255;
        by->twinkle[i][0].words.w1 = 0xFFFFFF00u | (u32) value;
    }
    // The bright stars' steady hearts, one command after the groups'.
    by->twinkle[by->twinkles][0].words.w1 = 0xFFFFFF00u | (u32) ((235 * (s32) star_fade) / 255);
}

// The mod's stars and moon live in the mod's own memory, above the
// console's. The renderer reads an address up there only between these two
// commands of its extended set (rt64_extended_gbi.h: opcode 0x64,
// G_EX_SETRDRAMEXTENDED_V1 = 0x2C), so they go around the sky's drawing.
static void extended_addresses(s32 on) {
    Gfx* cmd = MAIN_GFX_POS[0]++;

    cmd->words.w0 = (0x64u << 24) | 0x00002Cu;
    cmd->words.w1 = on ? 1u : 0u;
}

static void command(Gfx** list, u32 w0, u32 w1) {
    Gfx* cmd = (*list)++;

    cmd->words.w0 = w0;
    cmd->words.w1 = w1;
}

// A matrix that only moves a thing to a place: the identity, with the place
// in its last row. The record holds sixteen whole parts, then their sixteen
// fractions.
static void place(Mtx* mtx, f32 x, f32 y, f32 z) {
    volatile u16* half = (volatile u16*) mtx;
    s32 at[3];
    s32 i;

    at[0] = (s32) (x * 65536.0f);
    at[1] = (s32) (y * 65536.0f);
    at[2] = (s32) (z * 65536.0f);
    for (i = 0; i < 32; i++) {
        half[i] = 0;
    }
    half[0] = half[5] = half[10] = half[15] = 1;
    for (i = 0; i < 3; i++) {
        half[12 + i] = (u16) (at[i] >> 16);
        half[28 + i] = (u16) (at[i] & 0xFFFF);
    }
}

// The rain: sheets of drops around the camera, each sheet drawn twice, one
// copy above the other, and moved down by a matrix as the clock runs. A copy
// that has fallen a sheet's height is gone and a new one has come in at the
// top, so every copy only ever falls; each is named to the renderer
// (rt64_extended_gbi.h, a matrix group: opcode 0x64, command 0x0C, closed by
// 0x0D) so that it can place the copy between two ticks when the frame rate
// is raised. The camera's place is the sky's own: the sky box follows the
// camera on the ride and is set to a photo's camera for a photo. The lists
// go among the see-through things, which are drawn after the solid world.
static void rain(GObj* sky) {
    Gfx** list = &MAIN_GFX_POS[1];
    DObj* eye = sky->data.dobj;
    u32 ticks = ticks_of(night);
    Mtx* set;
    u32 sheet;
    u32 copy;

    if (eye == NULL || *list == NULL) {
        return;
    }
    rain_turn ^= 1;
    set = rain_mtx[rain_turn];
    command(list, (0x64u << 24) | 0x00002Cu, 1);
    gSPDisplayList((*list)++, night_rain_begin);
    // The wind: the sheets drift sideways on two slow waves, a gust every six
    // seconds or so with a quicker shiver on it, each sheet a little apart.
    for (sheet = 0; sheet < NIGHT_RAIN_SHEETS; sheet++) {
        u32 fallen = ticks * night_rain_speed[sheet];
        u32 turn = fallen / NIGHT_RAIN_HEIGHT;
        f32 gust = (f32) night_sine[((ticks * 23) >> 5) + 11 * sheet & 0xFF] * (90.0f / 127.0f)
                 + (f32) night_sine[(((ticks * 61) >> 5) + 70 + 29 * sheet) & 0xFF] * (36.0f / 127.0f);

        for (copy = 0; copy < 2; copy++) {
            u32 which = turn + copy;
            s32 below = (s32) fallen - (s32) (which * NIGHT_RAIN_HEIGHT);     // from -HEIGHT up to HEIGHT
            Mtx* mtx = &set[sheet * 2 + copy];

            place(mtx, eye->position.v.x + NIGHT_RAIN_SLANT_X * (f32) below + gust * 0.9f,
                  eye->position.v.y - (f32) (NIGHT_RAIN_HEIGHT / 2) - (f32) below,
                  eye->position.v.z + NIGHT_RAIN_SLANT_Z * (f32) below + gust * 0.4f);
            // The group: this id, pushed; place, turn, perspective and vertices followed between ticks.
            command(list, (0x64u << 24) | 0x00000Cu, 0x52410000u | (sheet << 8) | (which & 0xFF));
            command(list, 0x00002829u, 0);
            gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
            gSPDisplayList((*list)++, night_rain_sheets[sheet]);
            gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
            command(list, (0x64u << 24) | 0x00000Du, 1);
        }
    }
    gSPDisplayList((*list)++, night_rain_end);
    command(list, (0x64u << 24) | 0x00002Cu, 0);
}

// The snow: made and moved as the rain is, but slowly (its paces are in
// eighths of a unit a tick), and each sheet sways from side to side on a
// slow wave of its own as it falls.
static void snow(GObj* sky) {
    Gfx** list = &MAIN_GFX_POS[1];
    DObj* eye = sky->data.dobj;
    u32 ticks = ticks_of(night);
    Mtx* set;
    u32 sheet;
    u32 copy;

    if (eye == NULL || *list == NULL) {
        return;
    }
    rain_turn ^= 1;
    set = rain_mtx[rain_turn];
    command(list, (0x64u << 24) | 0x00002Cu, 1);
    gSPDisplayList((*list)++, night_rain_begin);
    gSPDisplayList((*list)++, night_snow_alpha);
    if (ashing) {
        // Ash: the flakes take the primitive's gray (the snow's material colors
        // them from their corners alone, which are white), a little see-through.
        gDPSetCombineLERP((*list)++, SHADE, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0,
                          SHADE, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0);
        gDPSetPrimColor((*list)++, 0, 0, 112, 104, 100, 220);
    }
    for (sheet = 0; sheet < NIGHT_SNOW_SHEETS; sheet++) {
        u32 fallen = (ticks * night_snow_speed[sheet]) >> (ashing ? 4 : 3);
        u32 turn = fallen / NIGHT_SNOW_HEIGHT;
        f32 sway = (f32) (night_sine[(((ticks * (90 + 17 * sheet)) >> 8) + 37 * sheet) & 0xFF] * 40) / 127.0f;
        f32 sway_across = (f32) (night_sine[(((ticks * (70 + 13 * sheet)) >> 8) + 91 * sheet) & 0xFF] * 28) / 127.0f;

        for (copy = 0; copy < 2; copy++) {
            u32 which = turn + copy;
            s32 below = (s32) fallen - (s32) (which * NIGHT_SNOW_HEIGHT);
            Mtx* mtx = &set[sheet * 2 + copy];

            place(mtx, eye->position.v.x + sway, eye->position.v.y - (f32) (NIGHT_SNOW_HEIGHT / 2) - (f32) below,
                  eye->position.v.z + sway_across);
            command(list, (0x64u << 24) | 0x00000Cu, 0x534E0000u | (sheet << 8) | (which & 0xFF));
            command(list, 0x00002829u, 0);
            gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
            gSPDisplayList((*list)++, night_snow_sheets[sheet]);
            gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
            command(list, (0x64u << 24) | 0x00000Du, 1);
        }
    }
    gSPDisplayList((*list)++, night_rain_end);
    command(list, (0x64u << 24) | 0x00002Cu, 0);
}

// The moon's path on the sea: a strip of soft light from near the camera
// toward the moon, a hair above the water (whose height is the mean of the
// vertices that flow, gathered as the walker meets them), moving with the
// camera as a real one does, and adding to the water rather than covering
// it. Where the land stands before the water it hides the path.
// The game's matrix at an address, as floats; its product with another; a
// float matrix packed as the game's.
static void read_mtx(f32 m[4][4], u32 addr) {
    const volatile s16* hi = (const volatile s16*) addr;
    const volatile u16* lo = (const volatile u16*) (addr + 32);
    s32 r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            m[r][c] = (f32) hi[r * 4 + c] + (f32) lo[r * 4 + c] / 65536.0f;
        }
    }
}

static void mul_mtx(f32 out[4][4], f32 a[4][4], f32 b[4][4]) {
    s32 r, c, k;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            f32 sum = 0.0f;

            for (k = 0; k < 4; k++) {
                sum += a[r][k] * b[k][c];
            }
            out[r][c] = sum;
        }
    }
}

static void copy_mtx(f32 out[4][4], f32 in[4][4]) {
    s32 r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            out[r][c] = in[r][c];
        }
    }
}

static void pack_mtx(Mtx* mtx, f32 m[4][4]) {
    volatile u16* half = (volatile u16*) mtx;
    s32 r, c;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            s32 v = (s32) (m[r][c] * 65536.0f);

            half[r * 4 + c] = (u16) (v >> 16);
            half[16 + r * 4 + c] = (u16) (v & 0xFFFF);
        }
    }
}

// One corner of the water, in the world through its node's matrix: its place
// in the dot from the tilt of the half-way direction between the eye and the
// moon, along the moon's bearing (gently) and across it (sharply), and its
// color, the moon's, fading into the far fog.
#define GLINT_ALONG 480.0f
#define GLINT_ACROSS 1200.0f

static void glint_corner(Vtx* v, f32 m[4][4], const f32* eye, const f32* la, const f32* breath) {
    f32 x = v->v.ob[0], y = v->v.ob[1], z = v->v.ob[2];
    f32 px = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
    f32 py = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
    f32 pz = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
    f32 vx = eye[0] - px, vy = eye[1] - py, vz = eye[2] - pz;
    f32 d = __builtin_sqrtf(vx * vx + vy * vy + vz * vz);
    f32 hx, hy, hz, hl, along, across, k;
    s32 st;

    if (d < 1.0f) {
        v->v.tc[0] = v->v.tc[1] = 8000;
        v->v.cn[0] = v->v.cn[1] = v->v.cn[2] = 0;
        return;
    }
    hx = vx / d + relight_dir[0];
    hy = vy / d + relight_dir[1];
    hz = vz / d + relight_dir[2];
    hl = __builtin_sqrtf(hx * hx + hy * hy + hz * hz);
    if (hl < 0.01f) {
        hl = 0.01f;
    }
    hx /= hl;
    hz /= hl;
    along = hx * la[0] + hz * la[1] + breath[0];
    across = -hx * la[1] + hz * la[0] + breath[1];
    st = 256 + (s32) (along * GLINT_ALONG);
    v->v.tc[0] = (s16) (st < -8000 ? -8000 : st > 8000 ? 8000 : st);
    st = 256 + (s32) (across * GLINT_ACROSS);
    v->v.tc[1] = (s16) (st < -8000 ? -8000 : st > 8000 ? 8000 : st);
    k = (d < 3500.0f) ? 1.0f : (7000.0f - d) / 3500.0f;
    if (k < 0.0f) {
        k = 0.0f;
    }
    v->v.cn[0] = (u8) (150.0f * k);
    v->v.cn[1] = (u8) (172.0f * k);
    v->v.cn[2] = (u8) (235.0f * k);
    v->v.cn[3] = 255;
}

// A segment of the water under the matrix the game drew it with: its corners
// set for this frame, then its loads and triangles, in a matrix group of its
// own so the port moves it with the water between the game's frames.
static void glint_segment(s32 k, f32 m[4][4], const f32* eye, const f32* la, const f32* breath, Gfx** list,
                          s32* opened) {
    const GlintSeg* seg = &glint_seg[k];
    Vtx* set = glint_vtx[glint_turn];
    Mtx* mtx = &glint_mtx[glint_turn][k];
    u32 j, o, pending = 0xFFFFFFFFu;

    if (!seg->flow) {
        return;
    }
    if (!*opened) {
        *opened = 1;
        command(list, (0x64u << 24) | 0x00002Cu, 1);
        gSPDisplayList((*list)++, night_glow_begin);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, 140);
    }
    for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
        o = glint_op[j];
        if ((o & GLINT_LOAD) && (o & GLINT_USED)) {
            u32 start = (o >> 12) & 0x1FFF, count = (o >> 6) & 0x3F, v;

            for (v = 0; v < count; v++) {
                glint_corner(&set[start + v], m, eye, la, breath);
            }
        }
    }
    {
        // Two units above the water: a surface that writes depth refuses a draw
        // on its own plane, and the lift keeps the glint under the land still.
        f32 lifted[4][4];

        copy_mtx(lifted, m);
        lifted[3][1] += 2.0f;
        pack_mtx(mtx, lifted);
    }
    command(list, (0x64u << 24) | 0x00000Cu, 0x474C0000u | (u32) k);
    command(list, 0x00002829u, 0);
    gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
        o = glint_op[j];
        if (o & GLINT_LOAD) {
            if (pending != 0xFFFFFFFFu) {
                gSP1Triangle((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0);
                pending = 0xFFFFFFFFu;
            }
            if (o & GLINT_USED) {
                gSPVertex((*list)++, &set[(o >> 12) & 0x1FFF], (o >> 6) & 0x3F, o & 63);
            }
        } else if (!(o & GLINT_FLOW)) {
            continue;       // the land's
        } else if (pending == 0xFFFFFFFFu) {
            pending = o;
        } else {
            gSP2Triangles((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0,
                          (o >> 16) & 0xFF, (o >> 8) & 0xFF, o & 0xFF, 0);
            pending = 0xFFFFFFFFu;
        }
    }
    if (pending != 0xFFFFFFFFu) {
        gSP1Triangle((*list)++, (pending >> 16) & 0xFF, (pending >> 8) & 0xFF, pending & 0xFF, 0);
    }
    gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
    command(list, (0x64u << 24) | 0x00000Du, 1);
}

// Embers over the lava. The lava is the Volcano's "water" to the glint's
// record (it draws with the flowing material), so when a part of it is first
// drawn, a few dozen points on its triangles become the embers' homes. Every
// frame each ember stands above its home by its age -- rising, swaying,
// cooling from yellow to a dull red, fading -- as a small soft dot with the
// adding blend, facing the view, under the matrix the game drew that part
// with. Their pace is the world's clock, so a photo keeps its instant's.
#define EMBERS 48
#define EMBER_HALF 7.0f
typedef struct Ember {
    f32 home[3];            // on the lava, in its part's own space
    u16 period, phase;      // the ticks of one rise, and where in it this one began
    u8 seg, sway;           // the part it belongs to, and its sway's phase
} Ember;
static Ember ember[EMBERS];
static Vtx ember_vtx[2][EMBERS * 4];   // this frame's corners and the last's
static u8 ember_turn;
static u32 ember_drawn;                 // the quads written so far this frame

static void ember_seed(s32 k) {
    const GlintSeg* seg = &glint_seg[k];
    u16 slot_copy[64];
    u32 j, o, tris = 0, want, made = 0;

    ember_seeded[k] = 1;
    for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
        if ((glint_op[j] & (GLINT_LOAD | GLINT_FLOW)) == GLINT_FLOW) {
            tris++;
        }
    }
    if (tris == 0) {
        return;
    }
    want = EMBERS / (glint_seg_n > 0 ? glint_seg_n : 1);
    if (want < 8) {
        want = 8;
    }
    while (ember_n < EMBERS && made < want) {
        Ember* e = &ember[ember_n];
        u32 mix = ((u32) ember_n * 2654435761u) ^ 0x5A17E000u;
        u32 pick, t = 0;
        f32 u, v;

        mix ^= mix >> 13;
        mix *= 0x85EBCA77u;
        mix ^= mix >> 16;
        pick = (mix >> 8) % tris;
        for (j = seg->op0; j < (u32) seg->op0 + seg->op_n; j++) {
            o = glint_op[j];
            if (o & GLINT_LOAD) {
                u32 start = (o >> 12) & 0x1FFF, count = (o >> 6) & 0x3F, first = o & 63, i;

                for (i = 0; i < count && first + i < 64; i++) {
                    slot_copy[first + i] = (u16) (start + i);
                }
            } else if (!(o & GLINT_FLOW)) {
                continue;
            } else if (t++ == pick) {
                const Vtx* A = &glint_vtx[0][slot_copy[(o >> 16) & 63]];
                const Vtx* B = &glint_vtx[0][slot_copy[(o >> 8) & 63]];
                const Vtx* D = &glint_vtx[0][slot_copy[o & 63]];
                s32 c;

                u = (f32) (mix & 0xFF) / 255.0f;
                v = (f32) ((mix >> 16) & 0xFF) / 255.0f;
                if (u + v > 1.0f) {
                    u = 1.0f - u;
                    v = 1.0f - v;
                }
                for (c = 0; c < 3; c++) {
                    e->home[c] = (f32) A->v.ob[c] + u * (f32) (B->v.ob[c] - A->v.ob[c]) + v * (f32) (D->v.ob[c] - A->v.ob[c]);
                }
                break;
            }
        }
        e->period = (u16) (200 + ((mix >> 24) % 120));
        e->phase = (u16) ((mix >> 4) % e->period);
        e->seg = (u8) k;
        e->sway = (u8) (mix & 0xFF);
        ember_n++;
        made++;
    }
}

static void ember_segment(s32 k, f32 m[4][4], Gfx** list, s32* opened) {
    u32 ticks = ticks_of(night);
    Vtx* set = ember_vtx[ember_turn];
    f32 rx = view_fwd[1], rz = -view_fwd[0];      // level, across the view
    u32 first_quad = ember_drawn;
    s32 e;

    if (!ember_seeded[k]) {
        ember_seed(k);
    }
    if (rx == 0.0f && rz == 0.0f) {
        rx = 1.0f;
    }
    for (e = 0; e < ember_n && ember_drawn < EMBERS; e++) {
        const Ember* em = &ember[e];
        u32 age;
        f32 a, x, y, z, half, bright, r, g, b;
        Vtx* q;
        s32 c;

        if (em->seg != k) {
            continue;
        }
        age = (ticks + em->phase) % em->period;
        a = (f32) age / (f32) em->period;
        x = em->home[0] * m[0][0] + em->home[1] * m[1][0] + em->home[2] * m[2][0] + m[3][0];
        y = em->home[0] * m[0][1] + em->home[1] * m[1][1] + em->home[2] * m[2][1] + m[3][1];
        z = em->home[0] * m[0][2] + em->home[1] * m[1][2] + em->home[2] * m[2][2] + m[3][2];
        y += a * (110.0f + 0.5f * (f32) em->period);
        x += (f32) night_sine[((ticks * 2) + em->sway * 4) & 0xFF] * (14.0f / 127.0f) * a;
        z += (f32) night_sine[((ticks * 3) + em->sway * 7 + 85) & 0xFF] * (10.0f / 127.0f) * a;
        bright = (a < 0.15f) ? a / 0.15f : 1.0f - (a - 0.15f) / 0.85f;
        r = (255.0f + (170.0f - 255.0f) * a) * bright;
        g = (180.0f + (40.0f - 180.0f) * a) * bright;
        b = (80.0f + (10.0f - 80.0f) * a) * bright;
        half = EMBER_HALF * (0.7f + 0.5f * a);
        q = &set[ember_drawn * 4];
        for (c = 0; c < 4; c++) {
            f32 sx = (c == 0 || c == 3) ? -1.0f : 1.0f;
            f32 sy = (c < 2) ? -1.0f : 1.0f;

            q[c].v.ob[0] = (s16) (x + sx * rx * half);
            q[c].v.ob[1] = (s16) (y + sy * half);
            q[c].v.ob[2] = (s16) (z + sx * rz * half);
            q[c].v.flag = 0;
            q[c].v.tc[0] = (c == 0 || c == 3) ? 0 : 512;
            q[c].v.tc[1] = (c < 2) ? 512 : 0;
            q[c].v.cn[0] = (u8) r;
            q[c].v.cn[1] = (u8) g;
            q[c].v.cn[2] = (u8) b;
            q[c].v.cn[3] = 255;
        }
        ember_drawn++;
    }
    if (ember_drawn == first_quad) {
        return;
    }
    if (!*opened) {
        *opened = 1;
        command(list, (0x64u << 24) | 0x00002Cu, 1);
        gSPDisplayList((*list)++, night_glow_begin);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, 255);
    }
    // In the world's own space, under the view: eight quads a load.
    {
        u32 qd = first_quad;

        while (qd < ember_drawn) {
            u32 n = ember_drawn - qd;
            u32 i;

            if (n > 8) {
                n = 8;
            }
            gSPVertex((*list)++, &set[qd * 4], n * 4, 0);
            for (i = 0; i < n; i++) {
                gSP2Triangles((*list)++, i * 4, i * 4 + 1, i * 4 + 2, 0, i * 4, i * 4 + 2, i * 4 + 3, 0);
            }
            qd += n;
        }
    }
}

// The water was just drawn: its commands, from where this model began, hold
// the matrices its nodes were drawn with and the calls of their lists. Each
// call that is a segment's is answered with that segment, under the product
// of the matrices in force there.
static void glint_scan(s32 mode) {
    DObj* eye = sky_now->data.dobj;
    f32 stack[8][4][4];
    f32 at[3], la[2], breath[2], len;
    u32 ticks;
    s32 i, k;

    if (eye == NULL || glint_seg_n == 0) {
        return;
    }
    at[0] = eye->position.v.x;
    at[1] = eye->position.v.y;
    at[2] = eye->position.v.z;
    len = __builtin_sqrtf(relight_dir[0] * relight_dir[0] + relight_dir[2] * relight_dir[2]);
    if (mode == 0 && len < 0.01f) {
        return;         // the moon overhead: no bearing to stretch along
    }
    la[0] = (len > 0.01f) ? relight_dir[0] / len : 1.0f;
    la[1] = (len > 0.01f) ? relight_dir[2] / len : 0.0f;
    // The lobe breathes: a slow sway along the bearing and a quicker, smaller one across.
    ticks = ticks_of(night);
    breath[0] = (f32) night_sine[(ticks * 2) & 0xFF] * (0.012f / 127.0f);
    breath[1] = (f32) night_sine[((ticks * 3) + 80) & 0xFF] * (0.006f / 127.0f);
    for (k = 0; k < GLINT_SEGS; k++) {
        glint_drawn[k] = 0;
    }
    if (!glint_said) {
        glint_said = 1;
        recomp_printf("[island skies] the record so far: %d corners, %d triangles of water, %d of land, %d parts\n",
                      (s32) glint_vtx_n, (s32) glint_tris, (s32) glint_land, (s32) glint_seg_n);
    }
    for (i = 0; i < 4; i++) {
        volatile u32* cmd = (volatile u32*) entry_pos[i];
        volatile u32* end = (volatile u32*) MAIN_GFX_POS[i];
        Gfx** list = &MAIN_GFX_POS[i];
        s32 sp = 0, budget = 4096, opened = 0, r, c;

        if (cmd == NULL || end < cmd) {
            continue;
        }
        for (r = 0; r < 4; r++) {
            for (c = 0; c < 4; c++) {
                stack[0][r][c] = (r == c) ? 1.0f : 0.0f;
            }
        }
        while (cmd < end && budget-- > 0) {
            u32 w0 = cmd[0], w1 = cmd[1];

            switch (w0 >> 24) {
                case 0xDA:      // a matrix: pushed unless the low bit says not, loaded or multiplied
                    if (!(w0 & 4) && w1 >= 0x80000000u) {
                        f32 m[4][4];

                        read_mtx(m, w1);
                        if ((w0 & 1) == 0 && sp < 7) {
                            copy_mtx(stack[sp + 1], stack[sp]);
                            sp++;
                        }
                        if (w0 & 2) {
                            copy_mtx(stack[sp], m);
                        } else {
                            f32 out[4][4];

                            mul_mtx(out, m, stack[sp]);
                            copy_mtx(stack[sp], out);
                        }
                    }
                    break;
                case 0xD8: {    // matrices popped
                    s32 n = (s32) (w1 >> 6);

                    while (n-- > 0 && sp > 0) {
                        sp--;
                    }
                    break;
                }
                case 0xDE:      // a list called: a segment's?
                    for (k = 0; k < glint_seg_n; k++) {
                        s32 j;

                        if (glint_drawn[k]) {
                            continue;
                        }
                        for (j = 0; j < glint_seg[k].lists_n; j++) {
                            if (glint_seg[k].lists[j] == w1) {
                                break;
                            }
                        }
                        if (j < glint_seg[k].lists_n) {
                            glint_drawn[k] = 1;
                            if (mode == 0) {
                                glint_segment(k, stack[sp], at, la, breath, list, &opened);
                            } else {
                                ember_segment(k, stack[sp], list, &opened);
                            }
                            break;
                        }
                    }
                    break;
                default:
                    break;
            }
            cmd += 2;
        }
        if (opened) {
            gSPDisplayList((*list)++, night_rain_end);
            command(list, (0x64u << 24) | 0x00002Cu, 0);
        }
    }
    if (!glint_said_drawn) {
        s32 drawn = 0;

        glint_said_drawn = 1;
        for (k = 0; k < glint_seg_n; k++) {
            drawn += glint_drawn[k];
        }
        recomp_printf("[island skies] the glint: %d of %d parts of the water found among the frame's commands\n",
                      drawn, (s32) glint_seg_n);
    }
}

static void moon_glint(void) {
    glint_scan(0);
}

static void embers(void) {
    ember_drawn = 0;
    glint_scan(1);
}

// Fireflies: four groups of small dots around the camera, each drifting on
// slow waves of its own and pulsing -- lit for half its cycle, dark for the
// other half -- out of step with the others. A photo keeps the pulse of its
// instant, as it keeps the twinkle.
static void fireflies(GObj* sky) {
    Gfx** list = &MAIN_GFX_POS[1];
    DObj* eye = sky->data.dobj;
    u32 ticks = ticks_of(night);
    Mtx* set = glow_mtx[glow_turn];
    u32 g;

    if (eye == NULL || *list == NULL) {
        return;
    }
    command(list, (0x64u << 24) | 0x00002Cu, 1);
    gSPDisplayList((*list)++, night_glow_begin);
    for (g = 0; g < NIGHT_FIREFLY_GROUPS; g++) {
        s32 pulse = night_sine[(((ticks * (23 + 3 * g)) >> 4) + 64 * g) & 0xFF];
        f32 dx = (f32) (night_sine[(((ticks * 3) >> 3) + 40 * g) & 0xFF] * 24) / 127.0f;
        f32 dy = (f32) (night_sine[(((ticks * 2) >> 3) + 90 * g) & 0xFF] * 12) / 127.0f;
        f32 dz = (f32) (night_sine[(((ticks * 5) >> 4) + 20 * g) & 0xFF] * 20) / 127.0f;
        Mtx* mtx = &set[1 + g];

        if (pulse <= 4) {
            continue;
        }
        place(mtx, eye->position.v.x + dx, eye->position.v.y + dy, eye->position.v.z + dz);
        command(list, (0x64u << 24) | 0x00000Cu, 0x46460000u | g);
        command(list, 0x00002829u, 0);
        gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, (u32) pulse * 2);
        gSPDisplayList((*list)++, night->fireflies[g]);
        gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
        command(list, (0x64u << 24) | 0x00000Du, 1);
    }
    gSPDisplayList((*list)++, night_rain_end);
    command(list, (0x64u << 24) | 0x00002Cu, 0);
}

// Lightning. Time is cut into spans of a few seconds; whether a span has a
// strike, when in the span, where around the sky and in which shape all
// come from the span's number alone, so the same instant always shows the
// same thing -- on the ride, and when a photo of that instant is redrawn.
#define STRIKE_SPAN 420         // ticks: seven seconds
#define STRIKE_ODDS 115         // of 256: how many spans have a strike
#define FLASH_TICKS 18          // three tenths of a second

void setFogColor(u8 r, u8 g, u8 b);
void setFogDistance(u16 min, u16 max);
s32 auPlaySoundWithParams(u32 sound, s32 volume, s32 pan, f32 pitch, s32 reverb);
void auStopSound(u32 handle);
void auSetSoundVolume(s32 handle, u32 volume);

// The weather's sounds are the cartridge's own. The rain is the rush of
// water the Valley plays for its whirlpool, looping. The thunder is two
// booms the cartridge holds: a deep one it never plays, as the strike, and
// the one that sounds under an evolution, slowed, as the body; the body
// again, lower and softer, is the echo. (A long low roll that snores, and
// Electrode's burst, were tried and heard first.)
#define RAIN_SOUND     33
#define RAIN_VOLUME    0x3000
#define THUNDER_STRIKE 102
#define THUNDER_BODY   395

static u32 strike_flash;        // this frame: how strong the flash is, of 255
static u32 strike_bolt;         // how strong the bolt is, of 255
static u32 strike_turn;         // where around the sky it stands, of 256
static u32 strike_shape;        // and which shape it has
static u32 strike_span;         // the span it belongs to, and that span's number mixed, for the thunder
static u32 strike_mix;
static s32 rain_handle = -1;    // the rain's voice while it plays
static u32 thunder_span;        // the span whose strike was heard last
static u32 thunder_at;          // when the thunder is due, in ticks; 0 when none is
static u32 body_at, echo_at;    // when its body and its echo are due; 0 when none is

static void strike_at(u32 ticks) {
    // A flash flickers: full, half, nearly full again, then dies away. The bolt is
    // there for the first four ticks only.
    static const u8 flash[FLASH_TICKS / 2] = { 255, 120, 220, 110, 70, 46, 30, 18, 8 };
    static const u8 bolt[4] = { 255, 90, 200, 60 };
    u32 span = ticks / STRIKE_SPAN;
    u32 mix = span * 0x9E3779B1u;
    u32 when;

    mix ^= mix >> 15;
    mix *= 0x85EBCA77u;
    mix ^= mix >> 13;
    strike_flash = 0;
    strike_bolt = 0;
    if (span == 0 || (mix & 0xFF) >= STRIKE_ODDS) {
        return;     // no strike in the ride's first seconds, nor in most spans
    }
    when = span * STRIKE_SPAN + ((mix >> 8) % (STRIKE_SPAN - FLASH_TICKS));
    if (ticks >= when && ticks < when + FLASH_TICKS) {
        u32 step = (ticks - when) / 2;      // each value holds for two ticks

        strike_flash = flash[step];
        if (step < 4) {
            strike_bolt = bolt[step];
        }
        strike_turn = (mix >> 16) & 0xFF;
        strike_shape = (mix >> 24) % NIGHT_BOLTS;
        strike_span = span;
        strike_mix = mix;
    }
}

// The sound the game has in each of its voices (sys/audio.c, auPlayingSound:
// a pointer to the table), -1 when the voice is free.
#define VOICE_SOUND (*(s32* volatile*) 0x800968BCu)

// Thunder follows a strike after a moment of the strike's own, a sixth of a
// second to a little over one: the strike, its body a tenth of a second
// after, and its echo half a second on. A strike's own number varies the
// pitch a little, so no two are quite alike. On the ride only: a photo's
// instant is silent.
// The side the bolt stood on, as a pan for its thunder: the game sets the
// scene's light behind and above the camera every frame, so the view runs
// the other way from the light, and the bolt's direction on the view's
// right is how far the sound sits to the right (64 is the middle).
static s32 thunder_side = 64;

static s32 thunder_pan(void) {
    volatile s8* dir = (volatile s8*) (COURSE_LIGHT + 16);
    f32 fx = -(f32) dir[0];
    f32 fz = -(f32) dir[2];
    f32 len = __builtin_sqrtf(fx * fx + fz * fz);
    f32 bx = (f32) night_sine[(strike_turn + 64) & 0xFF];
    f32 bz = -(f32) night_sine[strike_turn & 0xFF];
    f32 side;

    if (len < 8.0f) {
        return 64;
    }
    side = (-fz * bx + fx * bz) / (len * 127.0f);
    return 64 + (s32) (side * 52.0f);
}

static void thunder(u32 ticks) {
    if (strike_flash != 0 && strike_span != thunder_span) {
        thunder_span = strike_span;
        thunder_at = ticks + 10 + ((strike_mix >> 4) & 0x3F);
        body_at = echo_at = 0;
    }
    if (thunder_at != 0 && ticks >= thunder_at) {
        f32 pitch = 0.88f + (f32) ((strike_mix >> 12) & 3) * 0.04f;

        thunder_side = thunder_pan();
        auPlaySoundWithParams(THUNDER_STRIKE, 0x7000, thunder_side, pitch, 60);
        body_at = ticks + 6;
        echo_at = ticks + 26 + ((strike_mix >> 20) & 0xF);
        recomp_printf("[island skies] thunder at tick %u, pan %d\n", ticks, thunder_side);
        thunder_at = 0;
    }
    if (body_at != 0 && ticks >= body_at) {
        auPlaySoundWithParams(THUNDER_BODY, 0x6000, thunder_side, 0.8f, 80);
        body_at = 0;
    }
    if (echo_at != 0 && ticks >= echo_at) {
        auPlaySoundWithParams(THUNDER_BODY, 0x3400, 64, 0.6f, 110);
        echo_at = 0;
    }
}

// The rain's sound: one voice, looping, started as the ride's sky is first
// drawn, started again should the game have taken the voice for something
// of its own, and stopped as the world is taken down.
static void rain_sound(void) {
    if (rain_handle >= 0 && VOICE_SOUND != NULL && VOICE_SOUND[rain_handle] != RAIN_SOUND) {
        rain_handle = -1;
    }
    if (rain_handle < 0) {
        rain_handle = auPlaySoundWithParams(RAIN_SOUND, RAIN_VOLUME, 64, 1.1f, 20);
        recomp_printf("[island skies] the rain's voice: %d\n", rain_handle);
    }
}

// The night's voices: three insects in the dark, each with its own pitch and
// its own side, chirping at its own uneven pace, from one short sound of the
// cartridge's bank, played low under the music. The picks are by ear (the
// playtest folder's "night sounds" holds the candidates); the pace comes
// from the world's clock, so a photo's instant is quiet or not as the ride
// was. Not in rain, snow or a storm, which have their own voices, nor at
// sunset, when the day's sounds still stand.
#define CHIRP_SOUND 309
#define CHIRP_VOICES 3
static u32 chirp_next[CHIRP_VOICES];      // the tick of each insect's next chirp
static u8 chirp_said;

static void night_voices(void) {
    static const u8 chirp_pan[CHIRP_VOICES] = { 26, 64, 102 };
    static const f32 chirp_pitch[CHIRP_VOICES] = { 1.0f, 1.12f, 0.91f };
    static const u16 chirp_vol[CHIRP_VOICES] = { 0x1A00, 0x1400, 0x1700 };
    u32 ticks = ticks_of(night);
    s32 k;

    if (!on_the_ride || !night_sounds_on || sunset || raining || snowing || storming || scoring || night->embers
        || night->weather == NULL) {
        return;
    }
    for (k = 0; k < CHIRP_VOICES; k++) {
        if (ticks >= chirp_next[k] && ticks < chirp_next[k] + 30) {
            u32 mix = (ticks + 977u * (u32) k) * 0x9E3779B1u;
            f32 pitch = chirp_pitch[k] * (0.97f + (f32) ((mix >> 8) & 0xF) * 0.004f);
            s32 pan = (s32) chirp_pan[k] + (s32) ((mix >> 16) & 0xF) - 8;

            auPlaySoundWithParams(CHIRP_SOUND, chirp_vol[k], pan, pitch, 40);
            chirp_next[k] = ticks + 45 + ((mix >> 20) % 150) + 30 * (u32) k;
            if (!chirp_said) {
                chirp_said = 1;
                recomp_printf("[island skies] the first chirp at tick %u\n", ticks);
            }
        } else if (ticks >= chirp_next[k] + 30) {
            chirp_next[k] = ticks + 20 + 40 * (u32) k;      // the clock jumped (a new ride): start again soon
        }
    }
}

// What a flash does to the whole scene. The colors painted on the land
// cannot be relit for an instant, but the fog can: it jumps toward the
// flash's color and draws nearer, a veil of lit rain over everything; the
// scene light jumps with it, so the Pokémon are lit too. With no flash
// this writes the night's own values back.
static void storm_light(void) {
    static const u8 lit[3] = { 132, 148, 204 };
    volatile u8* light = COURSE_LIGHT;
    u8 fog[3];
    s32 i;

    for (i = 0; i < 3; i++) {
        u32 ambient = scene_ambient[i];
        u32 diffuse = scene_diffuse[i];

        fog[i] = (u8) (sky_horizon[i] + (((s32) lit[i] - sky_horizon[i]) * (s32) strike_flash) / 255);
        ambient += ((230 - ambient) * strike_flash) / 255;
        diffuse += ((255 - diffuse) * strike_flash) / 255;
        light[i] = light[4 + i] = (u8) ambient;
        light[8 + i] = light[12 + i] = (u8) diffuse;
    }
    setFogColor(fog[0], fog[1], fog[2]);
    setFogDistance((u16) (fog_near - (38 * strike_flash) / 255), fog_far);
}

// A matrix that turns a thing about the upright axis and moves it to a
// place: for the bolt, which is made standing to one side.
static void place_turned(Mtx* mtx, f32 x, f32 y, f32 z, u32 turn) {
    volatile u16* half = (volatile u16*) mtx;
    s32 cosine = night_sine[(turn + 64) & 0xFF] * 516;     // 16.16 fixed point: 65536 / 127
    s32 sine = night_sine[turn & 0xFF] * 516;

    place(mtx, x, y, z);
    half[0] = (u16) (cosine >> 16);
    half[16] = (u16) (cosine & 0xFFFF);
    half[2] = (u16) ((-sine) >> 16);
    half[18] = (u16) ((-sine) & 0xFFFF);
    half[8] = (u16) (sine >> 16);
    half[24] = (u16) (sine & 0xFFFF);
    half[10] = (u16) (cosine >> 16);
    half[26] = (u16) (cosine & 0xFFFF);
}

// A matrix from a place and three axes: the model's x, y and z run along them.
static void place_frame(Mtx* mtx, f32 x, f32 y, f32 z, const f32* ax, const f32* ay, const f32* az) {
    volatile u16* half = (volatile u16*) mtx;
    const f32* axes[3];
    s32 r, c;

    place(mtx, x, y, z);
    axes[0] = ax;
    axes[1] = ay;
    axes[2] = az;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            s32 v = (s32) (axes[r][c] * 65536.0f);

            half[r * 4 + c] = (u16) (v >> 16);
            half[16 + r * 4 + c] = (u16) (v & 0xFFFF);
        }
    }
}

// Sine and cosine off the mod's table, for angles that need no better.
static f32 tsin(f32 rad) {
    return (f32) night_sine[(u32) (rad * (256.0f / 6.2831853f) + 0.5f) & 0xFF] / 127.0f;
}

static f32 tcos(f32 rad) {
    return tsin(rad + 1.5707963f);
}

// The flash over the sky and the bolt, drawn right after the sky's own
// box and so behind all the land. Neither is to be placed between ticks:
// each is named to the renderer as a group it must leave alone.
static void storm_sky(void) {
    Gfx** list = &MAIN_GFX_POS[0];
    DObj* eye;
    Mtx* pair = storm_mtx[rain_turn & 1];

    if (strike_flash == 0 || sky_now == NULL || sky_now->data.dobj == NULL) {
        return;
    }
    eye = sky_now->data.dobj;
    place(&pair[0], eye->position.v.x, eye->position.v.y, eye->position.v.z);
    command(list, (0x64u << 24) | 0x00000Cu, 0);
    command(list, 0x00000001u, 0);
    gSPMatrix((*list)++, &pair[0], G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    gDPSetPrimColor((*list)++, 0, 0, 150, 170, 235, (120 * strike_flash) / 255);
    gSPDisplayList((*list)++, night_flash_dome);
    gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
    if (strike_bolt != 0) {
        place_turned(&pair[1], eye->position.v.x, eye->position.v.y, eye->position.v.z, strike_turn);
        gSPMatrix((*list)++, &pair[1], G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
        gSPDisplayList((*list)++, night_bolt_begin);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, (110 * strike_bolt) / 255);
        gSPDisplayList((*list)++, night_bolt_glows[strike_shape]);
        gDPSetPrimColor((*list)++, 0, 0, 255, 255, 255, strike_bolt);
        gSPDisplayList((*list)++, night_bolt_cores[strike_shape]);
        gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
    }
    gSPDisplayList((*list)++, night_storm_end);
    command(list, (0x64u << 24) | 0x00000Du, 1);
}

// The key light. By day the game aims its one light from over the viewer's
// shoulder, every frame, from the camera (app_level/player.c,
// func_80357FD4; the photo code does the same for a photo's camera). At
// night the moon is the key: the light leans toward it, the viewer's side
// keeping a share so that a Pokémon between the camera and the moon is not
// a silhouette; under cloud it comes from above; while lightning flashes,
// from the bolt; on the Volcano, from the lava below. Directions are in the
// world's own axes, as the game's are.
static void key_light(void) {
    volatile s8* dir = (volatile s8*) (COURSE_LIGHT + 16);
    f32 gx = dir[0], gy = dir[1], gz = dir[2];      // the game's own, this frame
    f32 kx, ky, kz, w, len;

    if (storming && strike_flash != 0) {
        kx = night_sine[(strike_turn + 64) & 0xFF];
        ky = 60.0f;
        kz = -(f32) night_sine[strike_turn & 0xFF];
        w = 1.0f;
    } else if (raining || snowing) {
        kx = 0.0f;
        ky = 127.0f;
        kz = 0.0f;
        w = 0.7f;
    } else {
        kx = relight_dir[0] * 127.0f;
        ky = relight_dir[1] * 127.0f;
        kz = relight_dir[2] * 127.0f;
        w = sunset ? 0.85f : 0.75f;
    }
    w *= nightness;         // by day the game's own light stands
    len = __builtin_sqrtf(kx * kx + ky * ky + kz * kz);
    if (len < 1.0f) {
        return;
    }
    kx /= len;
    ky /= len;
    kz /= len;
    len = __builtin_sqrtf(gx * gx + gy * gy + gz * gz);
    if (len < 1.0f) {
        gx = 0.0f;
        gy = 1.0f;
        gz = 0.0f;
    } else {
        gx /= len;
        gy /= len;
        gz /= len;
    }
    kx = kx * w + gx * (1.0f - w);
    ky = ky * w + gy * (1.0f - w);
    kz = kz * w + gz * (1.0f - w);
    len = __builtin_sqrtf(kx * kx + ky * ky + kz * kz);
    if (len < 0.01f) {
        return;
    }
    dir[0] = (s8) (kx * 127.0f / len);
    dir[1] = (s8) (ky * 127.0f / len);
    dir[2] = (s8) (kz * 127.0f / len);
}

// The Volcano's sky from below: the lava's glow on the clouds' undersides, the
// box's low corners tinted anew every frame with a slow breath and a quicker
// flicker on top of the night's own tint.
static void lava_sky(void) {
    static const u8 pulse[3] = { 58, 30, 12 };
    u32 ticks = ticks_of(night);
    f32 slow = (f32) night_sine[((ticks * 182) >> 8) & 0xFF] / 127.0f;
    f32 fast = (f32) night_sine[((ticks * 900) >> 8) & 0xFF] / 127.0f;
    f32 glow = 0.5f + 0.35f * slow + 0.15f * fast;
    u8 keep[3];
    s32 k, i;

    for (k = 0; k < 3; k++) {
        s32 v = tint_lo[k] + (s32) (glow * (f32) pulse[k]);

        keep[k] = tint_lo[k];
        tint_lo[k] = (u8) (v > 255 ? 255 : v);
    }
    for (i = 0; i < 2; i++) {
        if (night->sky_vtx[i] != 0) {
            sky_corners(night->sky_vtx[i], night->sky_vtx_n[i], i);
        }
    }
    for (k = 0; k < 3; k++) {
        tint_lo[k] = keep[k];
    }
}

RECOMP_HOOK("drawSkyBox1Cycle") void night_sky_begins(GObj* obj) {
    sky_now = obj;
    glint_turn ^= 1;        // the record's two sets turn once a frame
    ember_turn ^= 1;
    {
        volatile s8* dir = (volatile s8*) (COURSE_LIGHT + 16);
        f32 fx = -(f32) dir[0], fz = -(f32) dir[2];
        f32 len = __builtin_sqrtf(fx * fx + fz * fz);

        if (len > 8.0f) {
            view_fwd[0] = fx / len;
            view_fwd[1] = fz / len;
        }
    }
#ifdef NIGHT_MAP
    if (night != NULL && obj != NULL && obj->data.dobj != NULL) {
        u32 t = ticks_of(night);

        if (t - map_eye_tick >= 60 || t < map_eye_tick) {
            DObj* eye = obj->data.dobj;
            s32 c = map_cell(eye->position.v.x, eye->position.v.z);

            map_eye_tick = t;
            if (c >= 0) {
                map_kind[c] |= 4;
            }
            recomp_printf("[night eye] t=%d x=%d y=%d z=%d\n", (s32) t, (s32) eye->position.v.x,
                          (s32) eye->position.v.y, (s32) eye->position.v.z);
        }
    }
#endif
    if (night != NULL) {
        if (night->twinkle != NULL) {
            twinkle(night);         // at every hour: the fade makes them dark by day
        }
        extended_addresses(1);
        if (scoring) {
            return;         // no weather in the pass that measures a photo
        }
        if (storming) {
            strike_at(ticks_of(night));
            storm_light();
            if (on_the_ride) {
                thunder(ticks_of(night));
            }
        }
        if (moonlight_on) {
            key_light();
        }
        if (raining && obj != NULL) {
            rain(obj);
            if (on_the_ride) {
                rain_sound();
            }
        } else if ((snowing || ashing) && obj != NULL) {
            snow(obj);
        }
        if (night->embers && !scoring) {
            lava_sky();
        }
        night_voices();
    }
}

// A shooting star: now and then, in a clear night, a streak crosses the upper
// sky in four fifths of a second -- about fourteen degrees along a great
// circle, down and across, away from the moon -- the soft dot stretched
// into a streak (gen_night.py), white at its head and dark at its tail. The
// half-minutes that have one, where it begins and which way it runs come
// from the world's clock, as the storm's strikes do, so a photo's night
// has the star its instant had. It is drawn with a matrix group of its
// own, so the port moves it smoothly between the game's frames.
#define STREAK_SPAN  1800       // ticks: thirty seconds
#define STREAK_ODDS  150        // of 256: how many spans have a star
#define STREAK_TICKS 48         // its life
#define STREAK_ARC   0.25f      // how far it travels, in radians
#define STREAK_DIST  1400.0f    // where the sky's sprites stand (gen_night.py DIST)

static void shooting_star(void) {
    Gfx** list = &MAIN_GFX_POS[0];
    DObj* eye;
    Mtx* mtx;
    u32 ticks, span, mix, when, age;
    f32 d[3], e[3], m[3], c[3], t[3], n[3];
    f32 az, el, ce, phi, th, cs, sn, len, alpha;
    s32 k;

    if (sky_now == NULL || sky_now->data.dobj == NULL || sunset || raining || snowing || storming || scoring) {
        return;
    }
    ticks = ticks_of(night);
    span = ticks / STREAK_SPAN;
    if (span == 0) {
        return;         // none in the ride's first half-minute
    }
    mix = span * 0x9E3779B1u;
    mix ^= mix >> 15;
    mix *= 0x85EBCA77u;
    mix ^= mix >> 13;
    if ((mix & 0xFF) >= STREAK_ODDS) {
        return;
    }
    when = span * STREAK_SPAN + ((mix >> 8) % (STREAK_SPAN - STREAK_TICKS));
    if (ticks < when || ticks >= when + STREAK_TICKS) {
        return;
    }
    age = ticks - when;
    if (streak_logged != when) {
        streak_logged = when;
        recomp_printf("[island skies] a shooting star at tick %u\n", when);
    }
    // Where it begins: the upper sky, 26 to 70 degrees up, away from the moon.
    az = (f32) ((mix >> 16) & 0xFF) * (6.2831853f / 256.0f);
    el = 0.45f + (f32) ((mix >> 24) & 0x3F) * 0.012f;
    ce = tcos(el);
    d[0] = ce * tcos(az);
    d[1] = tsin(el);
    d[2] = ce * tsin(az);
    if (d[0] * relight_dir[0] + d[1] * relight_dir[1] + d[2] * relight_dir[2] > 0.96f) {
        az += 1.5707963f;       // a quarter turn off the moon
        d[0] = ce * tcos(az);
        d[2] = ce * tsin(az);
    }
    // Which way: along the dome, down and across. e runs level around it, n down it.
    len = __builtin_sqrtf(d[0] * d[0] + d[2] * d[2]);
    if (len < 0.05f) {
        return;
    }
    e[0] = -d[2] / len;
    e[1] = 0.0f;
    e[2] = d[0] / len;
    n[0] = d[1] * e[2] - d[2] * e[1];
    n[1] = d[2] * e[0] - d[0] * e[2];
    n[2] = d[0] * e[1] - d[1] * e[0];
    phi = 0.35f + (f32) ((mix >> 4) & 0xFF) * (2.44f / 255.0f);     // 20 to 160 degrees from level: always downward
    cs = tcos(phi);
    sn = tsin(phi);
    for (k = 0; k < 3; k++) {
        m[k] = e[k] * cs + n[k] * sn;
    }
    // The streak's center along the great circle (the head leads it by half the
    // streak's length) and its heading there; small angles, so a short series.
    th = STREAK_ARC * ((f32) age / (f32) STREAK_TICKS) - 0.054f;
    cs = 1.0f - th * th * 0.5f + th * th * th * th / 24.0f;
    sn = th - th * th * th / 6.0f;
    for (k = 0; k < 3; k++) {
        c[k] = d[k] * cs + m[k] * sn;
        t[k] = m[k] * cs - d[k] * sn;
    }
    n[0] = c[1] * t[2] - c[2] * t[1];
    n[1] = c[2] * t[0] - c[0] * t[2];
    n[2] = c[0] * t[1] - c[1] * t[0];
    alpha = (age < 6) ? (f32) age / 6.0f : (age < 30) ? 1.0f : (f32) (STREAK_TICKS - age) / 18.0f;
    eye = sky_now->data.dobj;
    streak_turn ^= 1;
    mtx = &streak_mtx[streak_turn];
    place_frame(mtx, eye->position.v.x + c[0] * STREAK_DIST, eye->position.v.y + c[1] * STREAK_DIST,
                eye->position.v.z + c[2] * STREAK_DIST, t, n, c);
    // Its own matrix group, as the bolt's: pushed, not interpolated (a group
    // that asked the port to interpolate it stopped the sky's frames).
    command(list, (0x64u << 24) | 0x00000Cu, 0);
    command(list, 0x00000001u, 0);
    gSPMatrix((*list)++, mtx, G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    gSPDisplayList((*list)++, night_rain_begin);     // the state the stars have inside the sky's list
    gDPSetPrimColor((*list)++, 0, 0, 230, 240, 255, (u8) (210.0f * alpha));
    gSPDisplayList((*list)++, night_streak);
    gSPDisplayList((*list)++, night_rain_end);
    gSPPopMatrix((*list)++, G_MTX_MODELVIEW);
    command(list, (0x64u << 24) | 0x00000Du, 1);
}

RECOMP_HOOK_RETURN("drawSkyBox1Cycle") void night_sky_ends(void) {
    if (night != NULL) {
        if (storming && !scoring) {
            storm_sky();
        }
        if (night->sky != NULL) {
            shooting_star();
        }
        extended_addresses(0);
    }
}

// The game's effects -- splashes, dust, smoke, spores, sparks, fire -- are
// drawn by one function, fx_draw, from each particle's own color, set as a
// primitive color (for some an environment color too) in the frame's command
// lists. Under the night those colors take the light that falls on bodies, as
// the splashes, the dust the Zero-One kicks up and the River's mist do; a
// color that is a fire's -- strongly orange or red, as lava and flames are --
// is a light in itself and keeps its day color, and so does the environment
// color that follows it.
// The light is given at the source, not to the commands: each particle's
// own colors (app_render/effect.h: primColor at 0x48, envColor at 0x50) are
// lit before the frame's cameras draw and put back after they have, so the
// game's own fades go on from the colors it keeps. (Rewriting the drawn
// commands afterward made the effects glitch as the view turned:
// JackandBeans, Oct 6 2026.)
// The hooks sit on omDrawAll, which runs every camera's drawing, and NOT on
// fx_draw. A hook on a game function makes the port's mod loader recompile
// that function afresh from the cartridge, and the port's own fx_draw carries
// additions the fresh copy lacks: the name it gives each particle's
// rectangle, which is how the effects move at the display's rate instead of
// the game's. With a hook on fx_draw, whatever the hook did and whether this
// option was on or off, every effect stepped as the view turned (JackandBeans,
// Oct 6 2026). Nothing runs between omDrawAll's start and its end but the
// drawing, so no particle is born or dies while its colors are the night's.
#define FX_LISTS     ((volatile u32*) 0x800BE1A8u)   // the drawer's sixteen particle lists
#define FX_PARTICLES 512
static u32 fx_lit_at[FX_PARTICLES];       // the particles lit this frame,
static u32 fx_prim[FX_PARTICLES];         // and their colors as they were
static u32 fx_env[FX_PARTICLES];
static u32 fx_lit_n;

static s32 fire_color(u32 word) {
    u32 r = word >> 24;
    u32 g = (word >> 16) & 0xFF;
    u32 b = (word >> 8) & 0xFF;

    return r >= 232 && g <= 176 && b <= 96;
}

RECOMP_HOOK("omDrawAll") void night_cameras_begin(void) {
    s32 list;

    fx_lit_n = 0;
    cycle_tick();
    if (night == NULL || scoring || !fx_lit) {
        return;
    }
    for (list = 0; list < 16; list++) {
        u32 at = FX_LISTS[list];
        s32 guard = FX_PARTICLES;

        while (at != 0 && (at & 3) == 0 && at >= 0x80000000u && guard-- > 0 && fx_lit_n < FX_PARTICLES) {
            volatile u32* prim = (volatile u32*) (at + 0x48);
            volatile u32* env = (volatile u32*) (at + 0x50);

            if (!fire_color(*prim)) {
                fx_lit_at[fx_lit_n] = at;
                fx_prim[fx_lit_n] = *prim;
                fx_env[fx_lit_n] = *env;
                fx_lit_n++;
                *prim = night_word(*prim);
                *env = night_word(*env);
            }
            at = *(volatile u32*) at;       // next
        }
    }
}

RECOMP_HOOK_RETURN("omDrawAll") void night_cameras_end(void) {
    u32 i;

    for (i = 0; i < fx_lit_n; i++) {
        *(volatile u32*) (fx_lit_at[i] + 0x48) = fx_prim[i];
        *(volatile u32*) (fx_lit_at[i] + 0x50) = fx_env[i];
    }
    fx_lit_n = 0;
}

// The game scores a photo by redrawing its scene into small depth pictures
// and counting what shows of each Pokémon; every object's own drawing is
// called for it, the sky's too. The weather stays out of that pass.
RECOMP_HOOK("score_Render") void night_scoring_begins(GObj* camera) {
    scoring = 1;
}

RECOMP_HOOK_RETURN("score_Render") void night_scoring_ends(void) {
    scoring = 0;
}

// A photo is about to be redrawn: its record holds the world's clock at the
// instant it was taken.
RECOMP_HOOK("func_8009D65C") void night_photo_begins(PhotoData* photo) {
    if (photo != NULL) {
        photo_clock = photo->unk_04.f32;
    }
}

// The options' two faces. Simple: one Time and one Weather row for the
// island. Custom: each course's own rows and the fine switches.
// Simple 0, Live 1 (the player's clock, the weather drawn), Custom 2.
static u32 setup_of(void) {
    return recomp_get_config_u32("setup");
}

static u8 simple_setup(void) {
    return setup_of() != 2;
}

#define TIME_NIGHT 0
#define TIME_SUNSET 1
#define TIME_DAWN 2
#define TIME_DUSK 3
#define TIME_DAY 4
#define TIME_CLOCK 5


// The player's clock, read through the port, as minutes of the day with the
// seconds in them, so the cycle moves between the minutes too.
static f32 clock_minutes(void) {
    u32 t = snap64_local_time();
    u32 hour = (t >> 16) & 0xFF;
    u32 minute = (t >> 8) & 0xFF;
    u32 second = t & 0xFF;

    return (f32) (hour * 60 + minute) + (f32) second / 60.0f;
}

// The time of day the options ask for a course: in Simple set-up the one
// Time row, in Custom the course's own; a photo's world takes the ride's.
static const Night* night_of(WorldSetup* setup) {
    u32 i;

    for (i = 0; i < sizeof(nights) / sizeof(nights[0]); i++) {
        if ((u32) setup == nights[i].setup) {
            u32 setup = setup_of();
            u32 time = (setup == 1) ? TIME_CLOCK : (setup == 0) ? recomp_get_config_u32("time")
                                   : recomp_get_config_u32(nights[i].option);

            cycle = (time == TIME_CLOCK);
            sunset = (time == TIME_SUNSET || time == TIME_DAWN || time == TIME_DUSK);
            dawn = (time == TIME_DAWN);
            dusk = (time == TIME_DUSK);
            return (time == TIME_DAY) ? NULL : &nights[i];
        }
    }
    return NULL;
}

// The weather the options ask for a course. In Simple set-up the one Weather
// row: the island's own (rain on the River, clear elsewhere), or one weather
// for the island, where rain and a storm bring ash to the Volcano and snow
// lies on the Valley alone. In Custom the course's own row. The answer is
// in the course's own terms: 0 clear, 1 rain (the Volcano: ash), 2 storm,
// 3 snow.
static u32 weather_of(const Night* by) {
    u32 pick;

    if (by->weather == NULL) {
        return 0;
    }
    if (!simple_setup()) {
        return recomp_get_config_u32(by->weather);
    }
    pick = (setup_of() == 1) ? 5 : recomp_get_config_u32("weather");    // 0 the island's own, 1 clear, 2 rain, 3 storm, 4 snow, 5 drawn
    if (pick == 5) {
        // Drawn once per ride for the course (a photo's world keeps the ride's):
        // mostly clear; rain a quarter of the time, a storm one in ten, snow one
        // in ten on the Valley; ash over the Volcano one in three.
        s32 idx = (s32) (by - nights);

        if (on_the_ride || !live_weather_drawn[idx]) {
            u32 t = snap64_local_time();
            u32 roll = (t * 2654435761u + snap64_local_date() * 40503u + (u32) idx * 97u) >> 24;   // 0..255

            if (by->embers) {
                live_weather[idx] = (roll < 85) ? 1 : 0;
            } else if (roll < 140) {
                live_weather[idx] = 0;
            } else if (roll < 204) {
                live_weather[idx] = 1;
            } else if (roll < 230) {
                live_weather[idx] = 2;
            } else {
                live_weather[idx] = (by->option[0] == 'v' && by->option[1] == 'a') ? 3 : 1;
            }
            live_weather_drawn[idx] = 1;
        }
        return live_weather[idx];
    }
    if (pick == 0) {
        return (by->option[0] == 'r') ? 1 : 0;      // the River's rain
    }
    if (by->embers) {
        return (pick == 2 || pick == 3) ? 1 : 0;     // the Volcano: ash with rain or a storm
    }
    if (pick == 4) {
        return (by->option[0] == 'v' && by->option[1] == 'a') ? 3 : 0;      // the Valley's snow
    }
    return pick - 1;
}

// The lights and the sky's colors in force for the time of day and the weather
// set: the night's, the sunset's or the dawn's.
static void choose_palette(const Night* by) {
    s32 i;

    for (i = 0; i < 3; i++) {
        // An overcast sky. By rain the picture goes gray and the tints a
        // little lighter than a clear night's; by snow the same picture sits
        // under much lighter tints: the brightest of the nights.
        static const u8 wet_pic_sky[3] = { 120, 128, 144 };
        static const u8 wet_pic_cloud[3] = { 168, 176, 188 };
        static const u8 wet_tint_lo[3] = { 56, 68, 92 };
        static const u8 wet_tint_hi[3] = { 28, 36, 60 };
        static const u8 snow_tint_lo[3] = { 124, 132, 148 };
        static const u8 snow_tint_hi[3] = { 64, 72, 96 };
        // The land under snow is the brightest thing of the night, and only a
        // little blue: it has a light of its own.
        static const u8 snow_world[3] = { 168, 176, 212 };
        // By night the land facing the moon is a little lighter.
        static const u8 moon_land_diff[3] = { 28, 32, 48 };
        // The sunset. The sky's picture holds blue sky and warm cloud; the
        // corners are violet at the horizon all around and deep violet-blue
        // at the top, and the warmth toward the sun is the glow band's,
        // drawn smooth (the sky is a six-sided drum, and a tint that turned
        // with the azimuth creased at its six edges). Overcast, gray-rose;
        // snow, pink.
        static const u8 sun_pic_sky[3] = { 150, 160, 210 };
        static const u8 sun_pic_cloud[3] = { 255, 215, 190 };
        static const u8 sun_tint_away[3] = { 130, 120, 170 };
        static const u8 sun_tint_hi[3] = { 90, 80, 140 };
        static const u8 sun_wet_pic_sky[3] = { 150, 150, 160 };
        static const u8 sun_wet_pic_cloud[3] = { 200, 195, 200 };
        static const u8 sun_wet_tint_away[3] = { 130, 126, 150 };
        static const u8 sun_wet_tint_hi[3] = { 96, 96, 120 };
        static const u8 sun_snow_tint_away[3] = { 170, 165, 185 };
        static const u8 sun_snow_tint_hi[3] = { 130, 128, 150 };
        // Its lights: a cool ambient on the land and a warm sun across the
        // faces that meet it; the Pokémon under the same two.
        static const u8 sun_world[3] = { 130, 115, 135 };
        static const u8 sun_world_overcast[3] = { 150, 140, 148 };
        static const u8 sun_land_diff[3] = { 210, 150, 90 };
        static const u8 sun_body[3] = { 225, 205, 190 };
        static const u8 sun_flow[3] = { 180, 155, 165 };
        static const u8 sun_ambient[3] = { 105, 100, 135 };
        static const u8 sun_diffuse[3] = { 255, 205, 150 };
        // The dawn: the same sky from the other side, cooler and pinker.
        static const u8 dawn_pic_sky[3] = { 165, 175, 220 };
        static const u8 dawn_pic_cloud[3] = { 255, 225, 215 };
        static const u8 dawn_tint_away[3] = { 120, 135, 190 };
        static const u8 dawn_tint_hi[3] = { 85, 105, 165 };
        static const u8 dawn_world[3] = { 125, 120, 150 };
        static const u8 dawn_land_diff[3] = { 215, 175, 130 };
        static const u8 dawn_body[3] = { 230, 215, 205 };
        static const u8 dawn_flow[3] = { 175, 165, 185 };
        static const u8 dawn_ambient[3] = { 108, 110, 145 };
        static const u8 dawn_diffuse[3] = { 255, 225, 195 };

        if (sunset) {
            u32 overcast = raining || snowing;

            light_world[i] = overcast ? sun_world_overcast[i] : dawn ? dawn_world[i] : sun_world[i];
            light_body[i] = dawn ? dawn_body[i] : sun_body[i];
            light_flow[i] = dawn ? dawn_flow[i] : sun_flow[i];
            land_diff[i] = overcast ? (u8) (sun_land_diff[i] >> 2) : dawn ? dawn_land_diff[i] : sun_land_diff[i];
            pic_sky[i] = overcast ? sun_wet_pic_sky[i] : dawn ? dawn_pic_sky[i] : sun_pic_sky[i];
            pic_cloud[i] = overcast ? sun_wet_pic_cloud[i] : dawn ? dawn_pic_cloud[i] : sun_pic_cloud[i];
            tint_lo[i] = snowing ? sun_snow_tint_away[i] : raining ? sun_wet_tint_away[i] : dawn ? dawn_tint_away[i] : sun_tint_away[i];
            tint_away[i] = tint_lo[i];
            tint_hi[i] = snowing ? sun_snow_tint_hi[i] : raining ? sun_wet_tint_hi[i] : dawn ? dawn_tint_hi[i] : sun_tint_hi[i];
            scene_ambient[i] = dawn ? dawn_ambient[i] : sun_ambient[i];
            scene_diffuse[i] = overcast ? (u8) part(sun_diffuse[i], 160) : dawn ? dawn_diffuse[i] : sun_diffuse[i];
        } else {
            u32 share = snowing ? 330 : raining ? 238 : 256;
            u32 world = snowing ? snow_world[i] : part(by->world[i], share);
            u32 body = part(by->body[i], share);
            u32 flow = part(by->flow[i], share > 256 ? 256 : share);    // water is not made brighter by snow

            light_world[i] = (u8) (world > 255 ? 255 : world);
            light_body[i] = (u8) (body > 255 ? 255 : body);
            light_flow[i] = (u8) (flow > 255 ? 255 : flow);
            land_diff[i] = (raining || snowing) ? 0 : moon_land_diff[i];
            pic_sky[i] = (raining || snowing) ? wet_pic_sky[i] : by->pic_sky[i];
            pic_cloud[i] = (raining || snowing) ? wet_pic_cloud[i] : by->pic_cloud[i];
            tint_lo[i] = snowing ? snow_tint_lo[i] : raining ? wet_tint_lo[i] : by->tint_lo[i];
            tint_away[i] = tint_lo[i];
            tint_hi[i] = snowing ? snow_tint_hi[i] : raining ? wet_tint_hi[i] : by->tint_hi[i];
            scene_ambient[i] = (u8) part(NIGHT_AMBIENT, light_body[i]);
            scene_diffuse[i] = (u8) part(NIGHT_DIFFUSE, light_body[i]);
        }
        sky_horizon[i] = (u8) ((pic_sky[i] * (((u32) tint_lo[i] + tint_away[i]) / 2)) / 255);
        sky_top[i] = (u8) ((pic_sky[i] * tint_hi[i]) / 255);
    }
    // Where the light comes from, for the painted land, the sky's corners
    // and the key light: the sun at sunset, the moon by night.
    {
        const s8* d = dawn ? by->dawn_sun : sunset ? by->sun : by->key;
        f32 len = __builtin_sqrtf((f32) d[0] * d[0] + (f32) d[1] * d[1] + (f32) d[2] * d[2]);

        if (len < 1.0f) {
            relight_dir[0] = 0.0f;
            relight_dir[1] = 1.0f;
            relight_dir[2] = 0.0f;
        } else if (sunset) {
            // The disc hangs six degrees up; the light that models the land
            // and the Pokémon comes from twenty-two, as it does in every game
            // that has a sunset, or flat ground would take none of it.
            f32 flat = __builtin_sqrtf((f32) d[0] * d[0] + (f32) d[2] * d[2]);

            if (flat < 1.0f) {
                flat = 1.0f;
            }
            relight_dir[0] = (d[0] / flat) * 0.927f;
            relight_dir[1] = 0.375f;
            relight_dir[2] = (d[2] / flat) * 0.927f;
        } else {
            relight_dir[0] = d[0] / len;
            relight_dir[1] = d[1] / len;
            relight_dir[2] = d[2] / len;
        }
    }
    cloud_top_fade = sunset ? 255 : 0;
}

// The dusk. The live colors are eleven triples; both ends are kept and the
// live ones slid between them as the ride's clock runs.
static u8* const live_palette[11] = { light_world, light_body, light_flow, land_diff, pic_sky, pic_cloud,
                                      tint_lo, tint_away, tint_hi, scene_ambient, scene_diffuse };

static void pal_keep(u8 pal[11][3], f32* dir) {
    s32 n, k;

    for (n = 0; n < 11; n++) {
        for (k = 0; k < 3; k++) {
            pal[n][k] = live_palette[n][k];
        }
    }
    for (k = 0; k < 3; k++) {
        dir[k] = relight_dir[k];
    }
}

// The land, the water and the scenery lit again from the record's day
// colors with the colors in force, as light_vertex lit them the first time.
static void relight_record(void) {
    u32 j;

    for (j = 0; j < glint_op_n; j++) {
        u32 o = glint_op[j];
        u32 start, count, src, v;

        if (!(o & GLINT_LOAD)) {
            continue;
        }
        start = (o >> 12) & 0x1FFF;
        count = (o >> 6) & 0x3F;
        src = glint_load_src[j];
        if (src < night->world_lo || src >= night->world_hi) {
            continue;
        }
        for (v = 0; v < count; v++) {
            u16 f = glint_face[start + v];
            volatile u8* color = (volatile u8*) (src + 16 * v + 12);
            u32 orig = glint_orig_cn[start + v];
            u8 c[3];
            s32 steep, flow, toward, k;
            const u8* light;

            if (!(f & 0x8000)) {
                continue;       // never lit by a triangle: left alone
            }
            steep = (f >> 14) & 1;
            flow = (f >> 13) & 1;
            toward = f & 0x1FF;
            light = flow ? light_flow : light_world;
            c[0] = (u8) (orig >> 24);
            c[1] = (u8) (orig >> 16);
            c[2] = (u8) (orig >> 8);
            if (snowing && !flow && !steep) {
                u32 bright = 159 + (97 * ((c[0] * 77 + c[1] * 151 + c[2] * 28) >> 8)) / 255;

                c[0] = (u8) snow_channel(c[0], SNOW_R, bright);
                c[1] = (u8) snow_channel(c[1], SNOW_G, bright);
                c[2] = (u8) snow_channel(c[2], SNOW_B, bright);
            }
            for (k = 0; k < 3; k++) {
                u32 lit = light[k] + (flow ? 0 : ((land_diff[k] * (u32) toward) >> 8));

                c[k] = (u8) part(c[k], whole(lit));
            }
            if (steep) {
                c[0] = (u8) part(c[0], steep_shade[0]);
                c[1] = (u8) part(c[1], steep_shade[1]);
                c[2] = (u8) part(c[2], steep_shade[2]);
            }
            color[0] = c[0];
            color[1] = c[1];
            color[2] = c[2];
        }
    }
}

// ---------------------------------------------------------------------------
// The cycle. One function, moment_apply, sets the whole scene for a minute of
// the day: which palettes blend and how far, how far from the cartridge's own
// day the scene stands (nightness), where the sun is and how bright, how far
// the stars and the moon have come, which list the sky's spare slot calls.
// Every mode is a moment: Night is 23:00, Sunset 18:30, Dawn 05:45; Dusk runs
// the minutes from 18:30 to 19:15 over the ride; the player's clock runs them
// all. The hours, the same the year round:
//
//   05:00  night           -> 05:45  dawn (pink, the sun rising)
//   05:45  dawn            -> 06:45  day (the cartridge's own)
//   17:30  day             -> 18:30  sunset (orange, the sun low)
//   18:30  sunset          -> 19:15  night (the sun sinks, the stars come)
//
// The day end of every blend is the cartridge itself: its picture texels, its
// corner colors, its fog, light and background, kept as the course loads.
#define M_DAWN0  300.0f
#define M_DAWN1  345.0f
#define M_DAY0   405.0f
#define M_DAY1   1050.0f
#define M_SUN1   1110.0f
#define M_NIGHT0 1155.0f

static f32 ease(f32 w) {
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    return w * w * (3.0f - 2.0f * w);
}

// A glow band's corners: faded, and deepened toward a red violet as it goes.
static void band_state(Vtx* v, const u8 colors[4][3], f32 fade, f32 late) {
    static const u8 deep[3] = { 205, 75, 115 };
    s32 c, r, k;

    if (v == NULL) {
        return;
    }
    fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
    for (c = 0; c < NIGHT_BAND_COLS; c++) {
        for (r = 0; r < NIGHT_BAND_ROWS; r++) {
            Vtx* p = &v[c * NIGHT_BAND_ROWS + r];
            u32 a = ((u32) (NIGHT_BAND_ALPHA * fade) * night_band_az_w[c] / 255) * night_band_el_w[r] / 255;

            for (k = 0; k < 3; k++) {
                p->v.cn[k] = (u8) (colors[r][k] + ((s32) deep[k] - colors[r][k]) * late);
            }
            p->v.cn[3] = (u8) a;
        }
    }
}

// A sun's haze and disc: lifted by so many units from where the generator
// put them (below the horizon when negative), and faded. The list: its
// band's call, the sprites' states, the dot, then the haze's color and
// corners and the disc's; each checked to be what it is.
static s16 sun_y0[2][8];
static u8 sun_y0_kept[2];

static void sun_state(Gfx* sun, s32 which, f32 lift, f32 alpha) {
    s32 k;

    alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
    if (sun != NULL && (sun[3].words.w0 >> 24) == 0xFA && (sun[6].words.w0 >> 24) == 0xFA
        && (sun[4].words.w0 >> 24) == 0x01 && (sun[7].words.w0 >> 24) == 0x01) {
        Vtx* haze = (Vtx*) sun[4].words.w1;
        Vtx* disc = (Vtx*) sun[7].words.w1;

        if (!sun_y0_kept[which]) {
            sun_y0_kept[which] = 1;
            for (k = 0; k < 4; k++) {
                sun_y0[which][k] = haze[k].v.ob[1];
                sun_y0[which][4 + k] = disc[k].v.ob[1];
            }
        }
        for (k = 0; k < 4; k++) {
            haze[k].v.ob[1] = (s16) (sun_y0[which][k] + lift);
            disc[k].v.ob[1] = (s16) (sun_y0[which][4 + k] + lift);
        }
        sun[3].words.w1 = 0xFFFFFF00u | (u32) (112.0f * alpha);
        sun[6].words.w1 = 0xFFFFFF00u | (u32) (255.0f * alpha);
    }
}

// The moon's two primitive colors sit at fixed places before the sky list's
// end: the end, the states put back, the moon's triangles and vertices, its
// color (four back), the moon picture, the glow's triangles and vertices, the
// glow's color (eight back); each checked to be a color.
static void moon_state(f32 moon) {
    moon = moon < 0.0f ? 0.0f : moon > 1.0f ? 1.0f : moon;
    if (!night->embers && night->sky != NULL) {
        Gfx* sky = night->sky;
        s32 n = 0;

        while (n < 4096 && sky[n].words.w0 != 0xDF000000u) {
            n++;
        }
        if (n >= 9 && (sky[n - 4].words.w0 >> 24) == 0xFA && (sky[n - 8].words.w0 >> 24) == 0xFA) {
            sky[n - 4].words.w1 = 0xFFFFFF00u | (u32) (255.0f * moon);
            sky[n - 8].words.w1 = 0xFFFFFF00u | (u32) (112.0f * moon);
        }
    }
}

// The sky list's spare slot (a pipe sync the generator found twice over)
// calls the list given, or goes back to being a pipe sync.
static void spare_point(Gfx* list) {
    volatile u32* spare = (volatile u32*) night->spare;

    if (spare == NULL) {
        return;
    }
    if (list == NULL || raining || snowing) {
        spare[0] = 0xE7000000u;
        spare[1] = 0;
    } else {
        spare[0] = 0xDE000000u;
        spare[1] = (u32) list;
    }
}

// How far each of the eleven colors has gone at a moment of the dusk. The
// land and its lights are dark once the sun is down (the sun sinks until six
// tenths); the clouds lose the sun's warmth early; the top of the sky darkens
// before the horizon; the sky's picture and the horizon's tints take the
// whole dusk.
static f32 dusk_share(s32 n, f32 t) {
    f32 e;

    switch (n) {
        case 0: case 1: case 2: case 3: case 9: case 10: e = t / 0.55f; break;
        case 5: e = (t - 0.15f) / 0.4f; break;
        case 8: e = t / 0.8f; break;
        default: e = t; break;
    }
    return e < 0.0f ? 0.0f : e > 1.0f ? 1.0f : e;
}

// The live colors slid between two palettes, on the dusk's curves or straight.
static void palette_blend(const u8 pa[11][3], const f32* da, const u8 pb[11][3], const f32* db, f32 t, s32 curved) {
    f32 len;
    s32 n, k;

    for (n = 0; n < 11; n++) {
        f32 share = curved ? dusk_share(n, t) : (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);

        for (k = 0; k < 3; k++) {
            live_palette[n][k] = (u8) ((f32) pa[n][k] + ((f32) pb[n][k] - (f32) pa[n][k]) * share);
        }
    }
    for (k = 0; k < 3; k++) {
        relight_dir[k] = da[k] + (db[k] - da[k]) * t;
        sky_horizon[k] = (u8) ((pic_sky[k] * (((u32) tint_lo[k] + tint_away[k]) / 2)) / 255);
        sky_top[k] = (u8) ((pic_sky[k] * tint_hi[k]) / 255);
    }
    len = __builtin_sqrtf(relight_dir[0] * relight_dir[0] + relight_dir[1] * relight_dir[1] + relight_dir[2] * relight_dir[2]);
    if (len > 0.01f) {
        for (k = 0; k < 3; k++) {
            relight_dir[k] /= len;
        }
    }
}

// The scene takes the colors in force, as far from the day as nightness says:
// the light, the fog and the background, the sky's pictures and corners, the
// recorded land.
static void scene_apply(void) {
    WorldSetup* setup = (WorldSetup*) night->setup;
    s32 share = (s32) (nightness * 256.0f);
    s32 i, k;

    for (k = 0; k < 3; k++) {
        steep_shade[k] = (u16) (256 + (((s32) (k == 0 ? 110 : k == 1 ? 118 : 160) - 256) * share) / 256);
    }
    set_light(night);
    {
        u8 fog[3], bg[3];
        s32 fmin = day_fog_min + (((s32) fog_near - day_fog_min) * share) / 256;
        s32 fmax = day_fog_max + (((s32) fog_far - day_fog_max) * share) / 256;

        for (k = 0; k < 3; k++) {
            fog[k] = (u8) (day_fog[k] + (((s32) sky_horizon[k] - day_fog[k]) * share) / 256);
            bg[k] = (u8) (day_bg[k] + (((s32) sky_top[k] - day_bg[k]) * share) / 256);
        }
        setFogColor(fog[0], fog[1], fog[2]);
        setup->fogDistanceMin = (u16) fmin;
        setup->fogDistanceMax = (u16) fmax;
        setup->fogR = fog[0];
        setup->fogG = fog[1];
        setup->fogB = fog[2];
        setup->backgroundR = bg[0];
        setup->backgroundG = bg[1];
        setup->backgroundB = bg[2];
    }
    for (i = 0; i < 2; i++) {
        if (night->pics[i] != 0) {
            sky_paint(night->pics[i], sky_mask_pic[i], i);
        }
        if (night->sky_vtx[i] != 0) {
            sky_corners(night->sky_vtx[i], night->sky_vtx_n[i], i);
            sky_orig_cn_kept[i] = 1;
        }
    }
    relight_record();
}

// A minute of the day, 0 to 1440: the whole scene set for it.
static void moment_apply(f32 m) {
    f32 stars = 0.0f, moon = 0.0f;

    cycle_minute = m;
    if (m >= M_NIGHT0 || m < M_DAWN0) {
        // Night.
        palette_blend(pal_b, dir_b, pal_b, dir_b, 0.0f, 0);
        nightness = 1.0f;
        sunset = 0;
        dawn = 0;
        cloud_top_fade = 0;
        stars = 1.0f;
        moon = 1.0f;
        sun_state(night->sun_list, 0, 0.0f, 0.0f);
        sun_state(night->dawn_list, 1, 0.0f, 0.0f);
        band_state(night->band, night_band_sun_color, 0.0f, 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, 0.0f, 0.0f);
        spare_point(night->sky);
    } else if (m < M_DAWN1) {
        // Night to dawn: the stars and the moon go, the sun comes up pink.
        f32 w = (m - M_DAWN0) / (M_DAWN1 - M_DAWN0);

        palette_blend(pal_b, dir_b, pal_d, dir_d, w, 0);
        nightness = 1.0f;
        sunset = (w > 0.5f);
        dawn = 1;
        cloud_top_fade = (u8) (255.0f * w);
        stars = 1.0f - ease(w);
        moon = 1.0f - ease(w);
        sun_state(night->sun_list, 0, 0.0f, 0.0f);
        sun_state(night->dawn_list, 1, -260.0f * (1.0f - w), ease(w));
        band_state(night->band, night_band_sun_color, 0.0f, 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, ease(w), 0.0f);
        spare_point(night->dawn_list);
    } else if (m < M_DAY0) {
        // Dawn to day: the pink lifts off the cartridge's own morning.
        f32 w = (m - M_DAWN1) / (M_DAY0 - M_DAWN1);

        palette_blend(pal_d, dir_d, pal_d, dir_d, 0.0f, 0);
        nightness = 1.0f - ease(w);
        sunset = 1;
        dawn = 1;
        cloud_top_fade = (u8) (255.0f * (1.0f - w));
        sun_state(night->sun_list, 0, 0.0f, 0.0f);
        sun_state(night->dawn_list, 1, 350.0f * w, 1.0f - ease(w));
        band_state(night->band, night_band_sun_color, 0.0f, 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, 1.0f - ease(w), 0.0f);
        spare_point(w < 0.999f ? night->dawn_list : NULL);
    } else if (m < M_DAY1) {
        // Day: the cartridge, exactly.
        palette_blend(pal_a, dir_a, pal_a, dir_a, 0.0f, 0);
        nightness = 0.0f;
        sunset = 1;
        dawn = 0;
        cloud_top_fade = 255;
        sun_state(night->sun_list, 0, 350.0f, 0.0f);
        sun_state(night->dawn_list, 1, 350.0f, 0.0f);
        band_state(night->band, night_band_sun_color, 0.0f, 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, 0.0f, 0.0f);
        spare_point(NULL);
    } else if (m < M_SUN1) {
        // Day to sunset: the sky warms, the sun comes down to its evening place.
        f32 w = (m - M_DAY1) / (M_SUN1 - M_DAY1);

        palette_blend(pal_a, dir_a, pal_a, dir_a, 0.0f, 0);
        nightness = ease(w);
        sunset = 1;
        dawn = 0;
        cloud_top_fade = 255;
        sun_state(night->sun_list, 0, 350.0f * (1.0f - w), ease(w));
        sun_state(night->dawn_list, 1, 0.0f, 0.0f);
        band_state(night->band, night_band_sun_color, ease(w), 0.0f);
        band_state(night->dawn_band, night_band_dawn_color, 0.0f, 0.0f);
        spare_point(night->sun_list);
    } else {
        // Sunset to night: the dusk. The sun sinks and fades, the band
        // deepens and goes, the stars rise and the moon comes.
        f32 t = (m - M_SUN1) / (M_NIGHT0 - M_SUN1);
        f32 sun_alpha = 1.0f - (t - 0.35f) / 0.3f;
        f32 sink = (t < 0.6f) ? t / 0.6f : 1.0f;

        palette_blend(pal_a, dir_a, pal_b, dir_b, t, 1);
        nightness = 1.0f;
        sunset = (t < 0.6f);
        dawn = 0;
        cloud_top_fade = (u8) (255.0f * (1.0f - t));
        stars = (t - 0.65f) / 0.3f;
        moon = (t - 0.7f) / 0.3f;
        sun_state(night->sun_list, 0, -260.0f * sink, sun_alpha);
        sun_state(night->dawn_list, 1, 0.0f, 0.0f);
        band_state(night->band, night_band_sun_color, (t < 0.3f) ? 1.0f : (t > 0.8f) ? 0.0f : (0.8f - t) / 0.5f, t > 0.8f ? 1.0f : t / 0.8f);
        band_state(night->dawn_band, night_band_dawn_color, 0.0f, 0.0f);
        spare_point(night->sun_list);
    }
    stars = stars < 0.0f ? 0.0f : stars > 1.0f ? 1.0f : stars;
    star_fade = (u8) (255.0f * stars);
    moon_state(moon);
    scene_apply();
}

// The minute a mode stands at, or runs through.
static f32 mode_minutes(void) {
    if (cycle) {
        return on_the_ride ? clock_minutes() : ride_minutes;
    }
    if (dusk) {
        // The ride's own clock: sunset holds for forty seconds, then eighty
        // seconds to night. A photo's world takes its instant's moment.
        f32 t = ((f32) ticks_of(night) - 2400.0f) / 4800.0f;

        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        return M_SUN1 + (M_NIGHT0 - M_SUN1) * t;
    }
    return dawn ? M_DAWN1 : sunset ? M_SUN1 : 1380.0f;
}

// Every ten ticks of a frame's drawing, the moment moves on: the dusk by the
// ride's clock, the cycle by the player's. The day holds still between its
// ends, so nothing is repainted there.
static void cycle_tick(void) {
    u32 t;
    f32 m;

    if (night == NULL || scoring || !(cycle || dusk)) {
        return;
    }
    t = ticks_of(night);
    if (!(dusk_last == 0xFFFFFFFFu || t - dusk_last >= 10 || t < dusk_last)) {
        return;
    }
    dusk_last = t;
    m = mode_minutes();
    if (on_the_ride) {
        ride_minutes = m;
    }
    if (m >= M_DAY0 && m < M_DAY1 && cycle_minute >= M_DAY0 && cycle_minute < M_DAY1) {
        return;
    }
    moment_apply(m);
}

// A course's world is about to be built, for the ride or for a photo. For a
// course with a night: make its data night, unless this load of it already
// is.
RECOMP_HOOK("createWorld") void night_create_world(WorldSetup* setup, s32 sky_id, s32 block_min, s32 block_max,
                                                   s32 link, s32 dl_link, void* pokemon_enters) {
    const Night* by;
    s32 i;

    on_the_ride = ((u32) pokemon_enters != PHOTO_WORLD_CALLBACK);
    by = night_of(setup);
    night = by;
    thunder_span = 0xFFFFFFFFu;
    thunder_at = body_at = echo_at = 0;
    chirp_next[0] = 240;
    chirp_next[1] = 330;
    chirp_next[2] = 420;
    chirp_said = 0;
    if (by == NULL) {
        set_light(NULL);
        return;
    }
    // The lights and the sky in force. A rainy night is overcast and a little
    // darker; nothing of it shows a star or the moon.
    {
        u32 weather = weather_of(by);

        raining = (weather == 1 || weather == 2);
        storming = (weather == 2);
        snowing = (weather == 3);
        ashing = 0;
        if (by->embers) {           // the Volcano's weather is ash, not rain
            ashing = (weather == 1);
            raining = storming = snowing = 0;
        }
        // The fine switches are Custom's; Simple takes their defaults.
        fx_lit = simple_setup() || (recomp_get_config_u32("effects") == 1);     // "Night-lit" is the second choice
        glows_on = simple_setup() || (recomp_get_config_u32("water_glow") == 0);
        moonlight_on = simple_setup() || (recomp_get_config_u32("moonlight") == 0);
        night_sounds_on = simple_setup() || (recomp_get_config_u32("night_sounds") == 0);
    }
    // The cycle's three ends: the sunset's palette, the night's and the dawn's.
    {
        u8 was_sunset = sunset, was_dawn = dawn;

        sunset = 1;
        dawn = 0;
        choose_palette(by);
        pal_keep(pal_a, dir_a);
        sunset = 0;
        choose_palette(by);
        pal_keep(pal_b, dir_b);
        sunset = 1;
        dawn = 1;
        choose_palette(by);
        pal_keep(pal_d, dir_d);
        sunset = was_sunset;
        dawn = was_dawn;
    }
    star_fade = 255;
    dusk_last = 0xFFFFFFFFu;
    cycle_minute = -1.0f;
    fog_near = by->fog_min - (snowing ? 10 : raining ? 6 : 0);
    fog_far = by->fog_max;
    // The set-up record ends in a byte the game leaves empty; it is marked here
    // so that a load is made night once. A fresh load brings it back empty.
    if (((volatile u8*) setup)[0x1F] != DONE_MARK) {
        volatile u8* light = COURSE_LIGHT;

        ((volatile u8*) setup)[0x1F] = DONE_MARK;
        // The course as the cartridge has it, the day end of every blend.
        day_fog[0] = setup->fogR;
        day_fog[1] = setup->fogG;
        day_fog[2] = setup->fogB;
        day_bg[0] = setup->backgroundR;
        day_bg[1] = setup->backgroundG;
        day_bg[2] = setup->backgroundB;
        day_fog_min = setup->fogDistanceMin;
        day_fog_max = setup->fogDistanceMax;
        for (i = 0; i < 3; i++) {
            day_ambient[i] = light[i];
            day_diffuse[i] = light[8 + i];
        }
        sun_y0_kept[0] = sun_y0_kept[1] = 0;
        sky_orig_cn_kept[0] = sky_orig_cn_kept[1] = 0;
        clear_maps();
        flow_y_sum = 0.0f;
        flow_y_n = 0;
        water_model = 0;
        glint_reset();
        sky_extent(by);
        for (i = 0; i < 2; i++) {
            if (by->pics[i] != 0) {
                s32 k;

                sky_at_night(by->pics[i], i);
                for (k = 0; k < SKY_TEXELS; k++) {
                    sky_mask_pic[i][k] = sky_mask[k];
                }
            }
        }
        {
            f32 m = mode_minutes();

            if (on_the_ride) {
                ride_minutes = m;
            }
            moment_apply(m);
        }
        recomp_printf("[island skies] the %s was loaded: %s%s\n", by->option,
                      cycle ? "the clock's hour" : dusk ? "the sun is setting" : dawn ? "the sun rises" : sunset ? "the sun sets" : "night falls",
                      storming ? ", with a storm" : raining ? ", with rain" : snowing ? ", with snow" : ashing ? ", with ash" : "");
        if (cycle) {
            u32 t = snap64_local_time();

            recomp_printf("[island skies] your clock says %u:%u%u\n", (t >> 16) & 0xFF, ((t >> 8) & 0xFF) / 10, ((t >> 8) & 0xFF) % 10);
        }
    } else {
        // A photo's world of the same load: the moment of its instant.
        moment_apply(mode_minutes());
    }
}

// The world is taken down: the light the courses share goes back to day.
RECOMP_HOOK("destroyWorld") void night_destroy_world(void) {
    if (night != NULL && glint_seg_n != 0) {
        recomp_printf("[island skies] the record at the end: %d corners, %d triangles of water, %d of land, %d parts%s%s%s\n",
                      (s32) glint_vtx_n, (s32) glint_tris, (s32) glint_land, (s32) glint_seg_n,
                      (glint_vtx_n > GLINT_VTX - 64) ? "; the corners FULL" : "",
                      (glint_op_n > GLINT_OPS - 64) ? "; the ops FULL" : "",
                      (glint_seg_n >= GLINT_SEGS) ? "; the parts FULL" : "");
    }
#ifdef NIGHT_MAP
    if (night != NULL) {
        map_print();
    }
#endif
    night = NULL;
    set_light(NULL);
    if (rain_handle >= 0) {
        if (VOICE_SOUND != NULL && VOICE_SOUND[rain_handle] == RAIN_SOUND) {
            auStopSound((u32) rain_handle);
        }
        rain_handle = -1;
    }
    thunder_at = body_at = echo_at = 0;
    chirp_next[0] = 240;
    chirp_next[1] = 330;
    chirp_next[2] = 420;
    chirp_said = 0;
}
