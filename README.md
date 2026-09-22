# Snap64 Recomp mods

Mods for [Snap64 Recomp](https://github.com/JackandBeans/Snap64Recomp), the
native PC port of Pokémon Snap. Each is one `.nrm` file. They need the port
at 1.0.9 or later.

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
Oak's evaluation scored the photos.

### Unlock Everything

Every course open from the start, with the Apple, the Pester Ball, the Poké
Flute and the Dash Engine in hand and the Beach tutorial marked done. It
sets these in your save the way the game itself does when you earn them, so
they stay when the game next saves. Your Pokémon Report and your album are
not touched. Copy `saves/` somewhere safe first if you may want to go back.

Built against 1.0.9. Not yet played through here; a report is welcome.

## Installing

1. Download the `.nrm` from the
   [Releases](https://github.com/JackandBeans/Snap64RecompMods/releases)
   page.
2. Put it in the `mods/` folder next to `Snap64Recomp.exe` (or the Linux
   binary).
3. Start the port. A new mod is on the first time it is found; Options >
   Mods in the game turns it on or off, and holds the mod's own settings.

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
`unlock_everything`.

One thing the template does not say yet: this Makefile passes
`-fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-bcopy`, because clang
otherwise turns a copy into a call to `memmove`, which the game does not
have. And a mod's hook on a function that calls the game's `memcpy` crashed
the port at 1.0.9 (the runtime recompiles a hooked function afresh and could
not resolve that call); a patch of a small function that runs at the same
moment worked, and that is how Unlimited Film is written.

## Licence

GPLv3, like the port. Copyright (C) 2026 JackandBeans.
