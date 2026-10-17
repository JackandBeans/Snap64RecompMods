// Written by tools/gen_night.py; do not edit by hand.
#ifndef NIGHT_DATA_H
#define NIGHT_DATA_H

#include "common.h"

#define NIGHT_GROUPS_MAX 54
typedef struct NightTwinkle {
    u8 base, swing, swell, pad;
    u16 rate1, rate2, rate3;
    u16 phase1, phase2, phase3;
} NightTwinkle;
extern const s8 night_sine[];
extern const NightTwinkle night_twinkles[];
extern u16 night_moon_phases[8][32 * 32];
extern Gfx night_use_moon[];      // its first command loads the moon's picture

// The glow band at the horizon toward the sun: its columns and rows, its weights and colors.
#define NIGHT_BAND_COLS 11
#define NIGHT_BAND_ROWS 4
#define NIGHT_BAND_ALPHA 170
extern const u8 night_band_az_w[NIGHT_BAND_COLS];
extern const u8 night_band_el_w[NIGHT_BAND_ROWS];
extern const u8 night_band_sun_color[NIGHT_BAND_ROWS][3];
extern const u8 night_band_dawn_color[NIGHT_BAND_ROWS][3];

// rain
#define NIGHT_RAIN_SHEETS 10
#define NIGHT_RAIN_HEIGHT 1400
#define NIGHT_RAIN_SLANT_X 0.070f
#define NIGHT_RAIN_SLANT_Z 0.030f
extern Gfx* const night_rain_sheets[];
extern Gfx* const night_rain_volume[];
#define NIGHT_VOLUME_HALF_X 1400
#define NIGHT_VOLUME_HALF_Z 2190
extern const u8 night_rain_speed[];
extern Gfx night_rain_begin[];
extern Gfx night_rain_end[];

// storm
#define NIGHT_BOLTS 8
extern Gfx night_flash_dome[];
extern Gfx night_bolt_begin[];
extern Gfx night_storm_end[];
extern Gfx* const night_bolt_glows[];
extern Gfx* const night_bolt_cores[];

// snow
#define NIGHT_SNOW_SHEETS 6
#define NIGHT_SNOW_HEIGHT 1000
extern Gfx* const night_snow_sheets[];
extern Gfx* const night_snow_volume[];
extern const u8 night_snow_speed[];
extern Gfx night_snow_alpha[];

// the moon path and the fireflies
#define NIGHT_FIREFLY_GROUPS 4
extern Gfx night_glow_begin[];
extern Gfx* const night_firefly_groups[];
extern Gfx night_streak[];

// beach
#define NIGHT_BEACH_SETUP 0x8011B914u
#define NIGHT_BEACH_SPARE 0x8011CD30u
#define NIGHT_BEACH_KEY { -108, 21, 63 }
#define NIGHT_BEACH_SUN { -109, 13, 63 }
extern Gfx night_beach_sun[];
extern Vtx night_beach_band[];
extern Vtx night_beach_dawn_band[];
#define NIGHT_BEACH_DAWN { 109, 13, -63 }
extern Gfx night_beach_dawn[];
#define NIGHT_BEACH_PICS { 0x8011B968u, 0x00000000u }
#define NIGHT_BEACH_ROWS { night_beach_rows0, 0 }
#define NIGHT_BEACH_SKY_VTX { 0x8011C968u, 0x8011CB48u }
#define NIGHT_BEACH_SKY_VTX_N { 30, 6 }
#define NIGHT_BEACH_R_LO 14
#define NIGHT_BEACH_R_SCALE 28
#define NIGHT_BEACH_TWINKLES 47
extern Gfx night_beach_sky[];
extern Gfx night_beach_twinkle[][2];
extern const u8 night_beach_rows0[32];

// river
#define NIGHT_RIVER_SETUP 0x8012AC90u
#define NIGHT_RIVER_SPARE 0x8012C1C0u
#define NIGHT_RIVER_KEY { 102, 31, 69 }
#define NIGHT_RIVER_SUN { 105, 13, 71 }
extern Gfx night_river_sun[];
extern Vtx night_river_band[];
extern Vtx night_river_dawn_band[];
#define NIGHT_RIVER_DAWN { -105, 13, -71 }
extern Gfx night_river_dawn[];
#define NIGHT_RIVER_PICS { 0x8012ACD8u, 0x00000000u }
#define NIGHT_RIVER_ROWS { night_river_rows0, 0 }
#define NIGHT_RIVER_SKY_VTX { 0x8012BCD8u, 0x8012BEB8u }
#define NIGHT_RIVER_SKY_VTX_N { 30, 24 }
#define NIGHT_RIVER_R_LO 23
#define NIGHT_RIVER_R_SCALE 64
#define NIGHT_RIVER_TWINKLES 45
extern Gfx night_river_sky[];
extern Gfx night_river_twinkle[][2];
extern const u8 night_river_rows0[32];

// volcano
#define NIGHT_VOLCANO_SETUP 0x800FFFB8u
#define NIGHT_VOLCANO_SPARE 0x801014F0u
#define NIGHT_VOLCANO_KEY { 0, 127, 0 }
#define NIGHT_VOLCANO_SUN { -119, 13, -43 }
extern Gfx night_volcano_sun[];
extern Vtx night_volcano_band[];
extern Vtx night_volcano_dawn_band[];
#define NIGHT_VOLCANO_DAWN { 119, 13, 43 }
extern Gfx night_volcano_dawn[];
#define NIGHT_VOLCANO_PICS { 0x80100008u, 0x00000000u }
#define NIGHT_VOLCANO_ROWS { night_volcano_rows0, 0 }
#define NIGHT_VOLCANO_SKY_VTX { 0x80101008u, 0x801011E8u }
#define NIGHT_VOLCANO_SKY_VTX_N { 30, 24 }
#define NIGHT_VOLCANO_R_LO 24
#define NIGHT_VOLCANO_R_SCALE 37
#define NIGHT_VOLCANO_TWINKLES 41
extern Gfx night_volcano_sky[];
extern Gfx night_volcano_twinkle[][2];
extern const u8 night_volcano_rows0[32];

// valley
#define NIGHT_VALLEY_SETUP 0x80100720u
#define NIGHT_VALLEY_SPARE 0x80102C80u
#define NIGHT_VALLEY_KEY { 112, 39, 45 }
#define NIGHT_VALLEY_SUN { 117, 13, 47 }
extern Gfx night_valley_sun[];
extern Vtx night_valley_band[];
extern Vtx night_valley_dawn_band[];
#define NIGHT_VALLEY_DAWN { -117, 13, -47 }
extern Gfx night_valley_dawn[];
#define NIGHT_VALLEY_PICS { 0x80101770u, 0x80100768u }
#define NIGHT_VALLEY_ROWS { night_valley_rows0, night_valley_rows1 }
#define NIGHT_VALLEY_SKY_VTX { 0x80102770u, 0x80102950u }
#define NIGHT_VALLEY_SKY_VTX_N { 30, 24 }
#define NIGHT_VALLEY_R_LO 14
#define NIGHT_VALLEY_R_SCALE 21
#define NIGHT_VALLEY_TWINKLES 48
extern Gfx night_valley_sky[];
extern Gfx night_valley_twinkle[][2];
extern const u8 night_valley_rows0[32];
extern const u8 night_valley_rows1[32];

// tunnel
#define NIGHT_TUNNEL_SETUP 0x8011E6CCu
#define NIGHT_TUNNEL_SPARE 0x00000000u
#define NIGHT_TUNNEL_KEY { -115, 35, 42 }
#define NIGHT_TUNNEL_SUN { -117, 24, 43 }
extern Gfx night_tunnel_sun[];
extern Vtx night_tunnel_band[];
extern Vtx night_tunnel_dawn_band[];
#define NIGHT_TUNNEL_DAWN { 117, 24, -43 }
extern Gfx night_tunnel_dawn[];
#define NIGHT_TUNNEL_PICS { 0x00000000u, 0x00000000u }
#define NIGHT_TUNNEL_ROWS { 0, 0 }
#define NIGHT_TUNNEL_SKY_VTX { 0x00000000u, 0x00000000u }
#define NIGHT_TUNNEL_SKY_VTX_N { 0, 0 }
#define NIGHT_TUNNEL_R_LO 14
#define NIGHT_TUNNEL_R_SCALE 28
#define NIGHT_TUNNEL_TWINKLES 47
extern Gfx night_tunnel_sky[];
extern Gfx night_tunnel_twinkle[][2];

#endif
