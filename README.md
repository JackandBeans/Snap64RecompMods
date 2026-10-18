# Snap64 Recomp mods

Mods for [Snap64 Recomp](https://github.com/JackandBeans/Snap64Recomp), the
native PC port of Pokémon Snap. Each mod is one `.nrm` file, also packed as
a Thunderstore zip, and each has a page of its own with its options and its
changelog.

<p>
<img src="mods/island_skies/pictures/lapras.png" width="49%" alt="Lapras on the sea at sunset">
<img src="mods/island_skies/pictures/river_stars.png" width="49%" alt="The River at night under the stars">
<img src="mods/island_skies/pictures/valley_rain.png" width="49%" alt="The Valley in rain">
<img src="mods/island_skies/pictures/volcano_snow.png" width="49%" alt="Rapidash in the snow on the Volcano">
</p>

Island Skies: the Beach at sunset, the River at night, the Valley in rain
and the Volcano in snow. More pictures are on
[its page](mods/island_skies/thunderstore/README.md).

| Mod | Version | Needs | Download |
| --- | --- | --- | --- |
| [Island Skies](mods/island_skies/thunderstore/README.md) | 1.1.1 | Snap64 Recomp 1.1.3 or later | [snap64_island_skies.nrm](https://github.com/JackandBeans/Snap64RecompMods/releases/latest/download/snap64_island_skies.nrm) |
| [Unlimited Film](mods/unlimited_film/thunderstore/README.md) | 1.0.2 | Snap64 Recomp 1.0.9 or later | [snap64_unlimited_film.nrm](https://github.com/JackandBeans/Snap64RecompMods/releases/latest/download/snap64_unlimited_film.nrm) |
| [Unlock Everything](mods/unlock_everything/thunderstore/README.md) | 1.0.2 | Snap64 Recomp 1.0.9 or later | [snap64_unlock_everything.nrm](https://github.com/JackandBeans/Snap64RecompMods/releases/latest/download/snap64_unlock_everything.nrm) |

The links are the latest release's files. Every
[release](https://github.com/JackandBeans/Snap64RecompMods/releases)
carries all three mods, each as its `.nrm` and as a Thunderstore zip; the
release's name says which mod changed and its new version, and the tag
counts the releases. Installing is one step, under [Installing](#installing).

## The mods

### Island Skies

The island's sky at any hour and in any weather: night, sunset, dawn, a
dusk that falls during the ride, or live by your own clock, under weather
of the island's own that holds from one ride to the next and turns during
a ride when its time comes. Stars, a moon with its glint on the water,
shooting stars, the Volcano's embers; rain, storms with lightning, snow
that lies, ash over the Volcano. By day the island is exactly as the
cartridge draws it, and photos keep the sky of their instant. Simple set-up
is three rows, Live adds only the sun's, Custom opens every row.

[Page](mods/island_skies/thunderstore/README.md) ·
[Changelog](mods/island_skies/thunderstore/CHANGELOG.md)

### Unlimited Film

The roll never runs out. Every course still starts with 60 shots, but the
counter never falls and the ride never ends for want of film: when the roll
is full, each new photo pushes the oldest one out, so the 60 you bring to
Professor Oak are the newest 60. An option lets the counter in the corner
show 60 throughout (the default) or count the shots you have taken.

[Page](mods/unlimited_film/thunderstore/README.md) ·
[Changelog](mods/unlimited_film/thunderstore/CHANGELOG.md)

### Unlock Everything

Every course open from the start, with the Apple, the Pester Ball, the Poké
Flute and the Dash Engine in hand and the Beach tutorial marked done. It
sets these in your save the way the game itself does when you earn them, so
they stay when the game next saves. Your Pokémon Report and your album are
not touched. Copy `saves/` somewhere safe first if you may want to go back.

[Page](mods/unlock_everything/thunderstore/README.md) ·
[Changelog](mods/unlock_everything/thunderstore/CHANGELOG.md)

## Installing

Download a mod from the
[Releases](https://github.com/JackandBeans/Snap64RecompMods/releases) page:
the `.nrm`, or the Thunderstore zip, which carries the `.nrm` with an icon
and a README.

- Snap64 Recomp 1.1.0 or later: drop the `.nrm` or the zip on the game
  window, or put either in the `mods/` folder next to `Snap64Recomp.exe`
  (or the Linux binary; on a Mac, the `mods/` folder in
  `~/Library/Application Support/Snap64 Recomp/`). The mod loads the next
  time the game starts.
- Snap64 Recomp 1.0.9: put the `.nrm` in the `mods/` folder (take it out of
  the zip first).

A new mod is on the first time it is found. Options > Mods in the game turns
it on or off, and Z there opens a mod's own settings.

## What has been checked

- **Unlimited Film.** A scripted Beach ride of 108 photos on the released
  1.0.9: the roll dropped its oldest 48 times, the ride ended as usual and
  Oak's evaluation scored the photos. The port's scoring replay on 1.1.0:
  45 photos scored with the mod on.
- **Unlock Everything.** On 1.1.0, from a save with only the Beach open and
  no items: with the mod on, the course map listed all seven courses and
  the Beach showed the three item buttons, where the same moment without
  the mod shows none. That the change stays in the save is read from the
  game's code, not watched: the mod's setters write the save the game keeps
  in memory (the decompilation's `src/more_funcs/5BF20.c`), and the game's
  save writes that whole record to the cartridge's flash.
- **Island Skies.** The port's scoring replay (`tools/check_mod.py`): the
  45 photos of the ride scored the same with the mod on as off. The cycle,
  the weather's turns, the moon and the sounds were checked frame by frame
  in captured rides, and so were 1.1.1's Tunnel photos in rain, its storm
  over the Tunnel, and the River by day under Island's own, which matched
  the game without the mod pixel for pixel.

## Building

The same tools as the
[mod template](https://github.com/JackandBeans/Snap64RecompModTemplate):
`clang` and `ld.lld` with the MIPS target, `make`, and `RecompModTool` from
N64Recomp. Check out the
[decompilation](https://github.com/ethteck/pokemonsnap) as `pokemonsnap`
and the [symbol files](https://github.com/JackandBeans/Snap64RecompSyms) as
`Snap64RecompSyms` beside this README (no build of the decompilation is
needed), then, for each of `unlimited_film`, `unlock_everything` and
`island_skies`:

    make MOD=unlimited_film
    RecompModTool mods/unlimited_film/mod.toml build/unlimited_film

`build/unlimited_film/snap64_unlimited_film.nrm` is the mod. Then

    python tools/pack_thunderstore.py unlimited_film unlock_everything island_skies

writes each mod's Thunderstore zip to `dist/`: a `manifest.json` made from
`mod.toml`, and the README, changelog and icon from
`mods/<mod>/thunderstore/`, beside the `.nrm`. The CI workflow does all of
this from a clean clone on every push.

This Makefile passes `-fno-builtin-memcpy -fno-builtin-memmove
-fno-builtin-bcopy`, because clang otherwise turns a copy into a call to
`memmove`, which the game does not have. At 1.0.9 a mod's hook failed on
three kinds of function: one that calls the game's `memcpy`, one the port's
own patches replace, and one that waits a frame. Snap64 Recomp 1.1.0 fixes
all three; Unlimited Film was written around the first, with a patch of a
small function that runs at the same moment, so it runs on 1.0.9 too.

## Writing a mod

The [mod template](https://github.com/JackandBeans/Snap64RecompModTemplate)
is the place to start: a mod's skeleton, the tool chain and the steps, the
same ones the Building section above uses. The port's
[MODS.md](https://github.com/JackandBeans/Snap64Recomp/blob/main/docs/MODS.md)
says what the port does with a mod, what it refuses to load, and where a
mod's settings and the log go. The three mods here are working examples:
each is a `mod.toml` (the manifest and the options page), a `src/` folder
and a Thunderstore page under `mods/<mod>/`, and the Makefile and
`tools/pack_thunderstore.py` take a new one by name. A mod that may touch
the scoring is checked with `tools/check_mod.py`, below.

## Tools

- `tools/pack_thunderstore.py` packs a built mod as a Thunderstore zip.
- `tools/make_icons.py` draws the mods' icons from the port's own logo, with
  the port's `tools/icon_gen.py`; the same picture is each mod's `thumb.png`.
- `tools/check_mod.py` proves a mod leaves the game's scoring alone: the
  port's evaluation replay, run without the mod and with it, must score every
  photo the same.
- `mods/island_skies/tools/gen_night.py` writes Island Skies' star and moon
  data from the player's own ROM.

## License

GPLv3, like the port. Copyright (C) 2026 JackandBeans.
