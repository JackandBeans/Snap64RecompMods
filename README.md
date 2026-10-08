# Snap64 Recomp mods

Mods for [Snap64 Recomp](https://github.com/JackandBeans/Snap64Recomp), the
native PC port of Pokémon Snap. Each is one `.nrm` file, also packed as a
Thunderstore zip. They need the port at 1.0.9 or later.

## The mods

### Unlimited Film

The roll never runs out. Every course still starts with 60 shots, but the
counter never falls and the ride never ends for want of film: when the roll
is full, each new photo pushes the oldest one out, so the 60 you bring to
Professor Oak are the newest 60. A setting in the mod's options lets the
counter in the corner show 60 throughout (the default) or count the shots
you have taken.

Checked on the released 1.0.9 with a scripted Beach ride that took 108
photos: the roll dropped its oldest 48 times, the ride ended as usual and
Oak's evaluation scored the photos. Checked again on 1.1.0 with the port's
scoring replay: 45 photos scored with the mod on.

### Unlock Everything

Every course open from the start, with the Apple, the Pester Ball, the Poké
Flute and the Dash Engine in hand and the Beach tutorial marked done. It
sets these in your save the way the game itself does when you earn them, so
they stay when the game next saves. Your Pokémon Report and your album are
not touched. Copy `saves/` somewhere safe first if you may want to go back.

Checked on 1.1.0 from a save with only the Beach open and no items: with the
mod on, the course map listed all seven courses, and the Beach showed the
three item buttons, where the same moment without the mod shows none. That
the change stays in the save is read from the game's code, not watched: the
mod's setters write the save the game keeps in memory (the decompilation's
`src/more_funcs/5BF20.c`), and the game's save writes that whole record to
the cartridge's flash.

### Island Skies

The island's sky at any hour and in any weather. The Beach, the Tunnel, the
River, the Volcano and the Valley after dark, at sunset, at dawn, through a
dusk that falls during the ride, or live: the island follows the player's
own clock round the whole day, and the weather is drawn afresh for each
ride. Each course's sky, ground, water and light are the course's own,
turned as the course loads: stars that twinkle between the clouds, a moon
over the open sky with its glint on the water, now and then a shooting
star, the Volcano lit by its own lava with embers rising off it and its
clouds pulsing in the glow, and insects chirping in the dark. Rain, storms
with lightning and thunder, snow that falls and lies on the Valley, ash over
the Volcano. By day the island is exactly as the cartridge draws it: the day
end of every blend is the course's own picture, corner colors, fog and
light, kept as the course loads. Photos keep the sky of their instant.
Simple set-up is three rows (Setup, Time, Weather), Live is one; Custom
gives each course its own hour and weather and opens the fine switches,
among them Lightning, gentle by default for sensitive eyes. The
hour by the clock comes from the port (`include/snap64.h`, Snap64 Recomp
1.1.3), and so does the date: the moon shows its phase, and a Sun row can
set the hours by a latitude, with no internet. The Tunnel has no sky box, so it takes the hour's light and sky
color and keeps the weather out.

Checked on the port's scoring replay: the 45 photos of the ride scored the
same with the mod on as off. The night, the sunset, the dusk, the live
cycle through an evening and a morning (the port's clock run forty times
fast), the weather, the moon's glint on the River and the Valley, the
Volcano's embers and ash and the shooting star were checked frame by frame
in captured rides. The sounds (the rain, the thunder, the insects) are the
cartridge's own effects, chosen by ear.

## Installing

Download a mod from the
[Releases](https://github.com/JackandBeans/Snap64RecompMods/releases) page:
the `.nrm`, or the Thunderstore zip, which carries the `.nrm` with an icon
and a README.

- Snap64 Recomp 1.1.0 or later: drop the `.nrm` or the zip on the game
  window, or put either in the `mods/` folder next to `Snap64Recomp.exe`
  (or the Linux binary; on a Mac, the `mods/` folder in
  `~/Library/Application Support/Snap64 Recomp/`). The mod loads the next time
  the game starts.
- Snap64 Recomp 1.0.9: put the `.nrm` in the `mods/` folder (take it out of
  the zip first).

A new mod is on the first time it is found. Options > Mods in the game turns
it on or off, and Z there opens a mod's own settings.

## Building

The same tools as the
[mod template](https://github.com/JackandBeans/Snap64RecompModTemplate):
`clang` and `ld.lld` with the MIPS target, `make`, and `RecompModTool` from
N64Recomp. Check out the
[decompilation](https://github.com/ethteck/pokemonsnap) as `pokemonsnap`
and the [symbol files](https://github.com/JackandBeans/Snap64RecompSyms) as
`Snap64RecompSyms` beside this README (no build of the decompilation is
needed), then:

    make MOD=unlimited_film
    RecompModTool mods/unlimited_film/mod.toml build/unlimited_film

`build/unlimited_film/snap64_unlimited_film.nrm` is the mod. The same with
`unlock_everything`. Then

    python tools/pack_thunderstore.py unlimited_film unlock_everything

writes each mod's Thunderstore zip to `dist/`: a `manifest.json` made from
`mod.toml`, and the README, changelog and icon from
`mods/<mod>/thunderstore/`, beside the `.nrm`. `tools/make_icons.py` drew
the icons from the port's own logo. The CI workflow does all of this from a
clean clone on every push.

One thing the template does not say yet: this Makefile passes
`-fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-bcopy`, because clang
otherwise turns a copy into a call to `memmove`, which the game does not
have. And at 1.0.9 a mod's hook failed on three kinds of function: one that
calls the game's `memcpy`, one the port's own patches replace, and one that
waits a frame (the game stopped when its process ended). Snap64 Recomp
1.1.0 fixes all three. Unlimited Film was written around the first, with a
patch of a small function that runs at the same moment, so it runs on
1.0.9 too.

## License

GPLv3, like the port. Copyright (C) 2026 JackandBeans.
