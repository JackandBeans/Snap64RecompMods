# Island Skies

A mod for [Snap64 Recomp](https://github.com/JackandBeans/Snap64Recomp), the
native PC port of Pokémon Snap.

The island's sky at any hour and in any weather. Night, sunset, dawn, a
dusk that falls during the ride, or live: the island follows your own clock
round the whole day, under weather of its own that holds from one ride to
the next and turns during a ride when its time comes. Stars that twinkle,
a moon with its glint on the water, a shooting star now and then, the
Volcano lit by its lava; rain, storms with lightning, snow that lies on
the Valley, ash over the Volcano. By day the island is exactly as the
cartridge draws it, and photos keep the sky of their instant.

## Installing

Mod managers do not support Snap64 Recomp. Use **Manual Download** on this
page, then drop the zip on the game window, or put it in the port's `mods`
folder beside `Snap64Recomp.exe` (on a Mac, in `~/Library/Application
Support/Snap64 Recomp/`). The mod loads the next time the game starts.
Needs Snap64 Recomp 1.1.3 or later. Options > Mods turns it on or off.

## Setting it up

In the game, open Options > Mods, select Island Skies and press Z. The
first row is **Setup**:

- **Simple** is two more rows. **Time**: Night (the default), Sunset,
  Dawn, Dusk (the sun sets during the ride), Day, or Your clock.
  **Weather**: Island's own (rain on the River, clear elsewhere), Clear,
  Rain, Storm, Snow, or Random.
- **Live** is your clock and the island's own weather, with nothing else
  to set.
- **Custom** opens every row: each course's hour and weather, and the fine
  switches.

The mod's details page (Options > Mods, Island Skies) says what it is
doing right now: the hour and its light, the weather, the sun's hours, and
the next change of weather when one is near. As a ride starts, a line at
the top left of the screen says the same for a few seconds, and a change
of weather during a ride is announced the same way.

## How Live works

The island's hours, the same the year round unless the Sun row says
otherwise:

- 05:00 to 05:45, the night turns to dawn: the stars and the moon go, the
  sun comes up pink.
- 05:45 to 06:45, the dawn turns to day.
- 06:45 to 17:30, day, as the cartridge has it.
- 17:30 to 18:30, the day turns to sunset: the sky warms and the sun comes
  down to the horizon.
- 18:30 to 19:15, dusk: the sun sinks and fades, the stars rise, the moon
  comes.
- 19:15 to 05:00, night.

A ride that crosses one of these hours changes as it goes. A photo's world
keeps the hour of its instant.

The weather comes from the date and the hour, so every ride of the same
half hour has the same weather and the next ride finds what the last one
left: mostly clear, rain about a quarter of the time, a storm one hour in
eight, a change about every two hours. A change comes some minutes into
its half hour and takes a minute and a half, during a ride when it falls
in one: the sky grays and the stars go, then the rain comes; clearing, the
rain thins first and the stars return. The odds lean with the season, and
in the winter months the Valley's rain is snow.

The **Sun** row sets the hours by the sun instead: with **By latitude**
the **Latitude** row (degrees north, minus for south) gives sunrise and
sunset for today's date, with no internet, and makes the dawn and the dusk
as long as the twilight is there, short at the equator and long in a far
northern June. The **Longitude** row (degrees east, minus for west) puts
solar noon where your clock has it; at 0 the mod takes your clock's zone,
which is within half an hour. The moon shows its phase by the date, new
to full and back, and south of the equator its lit side is the other one.

## Custom's rows

- **Beach, Tunnel, River, Volcano, Valley**: each course's own hour. The
  Tunnel has no sky, only its light and the color where it opens to the
  air, and its roof keeps the weather out.
- **Beach Weather, River Weather, Valley Weather, Volcano Weather**: each
  course's own sky; the Valley can have snow, the Volcano ash.
- **Effects at Night**: splashes, dust and mist take the night's light, or
  keep their day colors.
- **Water Glow**: the moon's glint on the water and the River's fireflies.
- **Moonlight on Pokémon**: the scene's light leans toward the moon, or
  stays where the game puts it.
- **Night Sounds**: insects chirping in the dark on a clear night.
- **Lightning**: Gentle (the default) keeps the bolt and the thunder but
  dims the flash that fills the screen, for sensitive eyes; Full is the
  storm as drawn; Off leaves a storm its rain and wind.

## Source

[github.com/JackandBeans/Snap64RecompMods](https://github.com/JackandBeans/Snap64RecompMods),
GPLv3.
