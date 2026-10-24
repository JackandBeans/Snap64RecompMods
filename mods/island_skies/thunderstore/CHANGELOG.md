# Changelog

## 1.1.2

- The River's fireflies are few and seldom. Each flashes by itself,
  a soft rise and a longer fade under a second in all, at a moment of
  its own drawn afresh every twelve seconds, five times in eight, so no
  two keep time and no stretch of the ride repeats; a flash shows
  somewhere every few seconds. 1.1.1 lit them sixteen at a time, in four
  groups that each glowed for a second and a half of every three, and
  the water swarmed. There are 32 where there were 64, each smaller and
  dimmer, and they keep near the water, from a little below the camera
  to a little above it, where some hung high in the sky. Asked for by
  JackandBeans on Oct 18 2026, twice: the first cut still blinked too
  fast and too often.
- A mist at dawn on the River and the Valley: the fog draws in a little
  and goes from the sky's own horizon color a quarter of the way to a
  pale gray, so the far land fades into a lighter sky, and wisps of it
  lie on the water and along its banks, with open water between them,
  faint by the cart and fullest in the middle distance, thinning again
  toward the far fog. The wisps lie still on the world and drift a
  little, and they follow the shore up the bank's foot and clear as the
  ground rises; a wall carries none. It gathers through the night's last
  hour, lies thickest as the sky pinks and lifts as the day comes under
  Live; a ride at Dawn starts in it and sees it thin as the ride goes.
  Clear weather only. Chosen by JackandBeans on Oct 18 2026 from a list
  of what the mod still lacked. Four cuts of a flat sheet riding with
  the camera went before it: the first pulled the fog most of the way to
  white and hung the sheet over the water, and the walls and the logs
  went white (JackandBeans, Oct 18 2026); the second lay as a film over
  all the water and was still too much (Oct 19 2026); the third moved
  its picture in jumps against the gliding world; the fourth still moved
  oddly, since the port glides a sheet between the game's frames while
  its picture is written only at each frame, and its flat plane cut the
  sloping banks in a straight edge (JackandBeans, Oct 19 2026). The mist
  is now drawn on the water's and the shore's own triangles instead. Its
  first cut that way was drawn in the middle of the scene and left its
  modes to the land drawn after it, which went wrong, drew a seam along
  every shared edge, and took no fog, so a block coming into view brought
  a bright veil with it (JackandBeans, Oct 19 2026, "extremely glitchy
  and choppy"): it is drawn after the whole scene now, under the camera's
  own matrices, without the edge smoothing, and fogged as the land is.
  That cut still drew bright lines along the water's rows and glitched
  (JackandBeans, Oct 19 2026): it was drawn under whatever matrix the
  scene's last object had left, which the port glides between the game's
  frames, and the reader of the camera's matrices looked for a view the
  game folds into its projection, so the fog never reached it. The mist
  now stands in the world's own frame in a group of its own the port
  leaves still, writes its depth so that where two of its triangles lie
  on one pixel the second blends no more, and takes the fog. With that
  it looked better, with a little flickering and some lines left
  (JackandBeans, Oct 19 2026): its frame's books now close with its own
  draw rather than with the sky's, so a model drawn before the sky in a
  frame keeps its mist; the shore's mist stands a hair nearer than the
  water's, so the two never tie where they cross; and a block's water
  coming into the record no longer moves the mist already lying. His
  pictures of that cut showed a bright hairline along the water's edge
  under the rock walls and a dashed line across the intro's water: the
  river's bed within a unit under the water carried mist too, and that
  mist, pulled nearer than the water's, showed through the water as a
  band along the walls' feet and a line over a shallow. No mist lies
  under the water now: a shore corner under it is lifted to the water's
  level, the water's mist is drawn first and writes its depth, and the
  shore's, pulled less, stays unseen wherever the two meet. That cut kept
  the intro's dashed line and glitched the mist on the grass as the cart
  went (JackandBeans, Oct 20 2026): the water's mist, pulled toward the
  eye, came through the lowest grass by the water as the view moved, and
  the River's first stretch is in the record twice, so its mist was laid
  twice. The shore's mist is now drawn as a decal on the land itself, at
  the land's own depth and under each part's own matrix, pulled nowhere
  and lifted nowhere, so it lies only where the land shows: not under the
  water's mist, nor under the water, nor over what stands on the bank; and
  a triangle whose corners the frame has drawn already is drawn no more.
  His pictures of a test build with the water's mist blue and the shore's
  red showed the intro's line gone and no red at all: the renderer draws a
  decal only within a tolerance of the depth it finds, and refused every
  bit of the shore's. The shore's mist is now drawn through the very matrix
  commands the game drew that part of the land with, so its depths come
  out the same as the land's, and with a depth test that lets an equal
  depth through; and the water's mist stands two units over the water
  instead of being pulled toward the eye, since the pulled mist came
  through the low grass by the water as the view moved and, where the
  water runs under a wall's foot, through the wall as a line along it.
  With that it "looks perfectly fine" near and was too much from a
  distance (JackandBeans, Oct 20 2026): it now thins from 700 units out
  and is gone at 1500, where it stood full to 1400 and went at 2600, and
  its full thickness is a tenth less.
- In snow every wall holds a dusting: a face steeper than 66 degrees,
  which held no snow at all, is now its own rock, a little lighter and
  cooler, and the snow line runs from white to dusted rock. The rock under
  snow is a little less dark to match. Found by JackandBeans on Oct 20
  2026: the walls stood bare in a white course.
- The River's three shimmering panels where Porygon hides, two of rock
  and one of grass, are land to the mod now: snow lies on them as on the
  walls around them, and by night they take the walls' light. They share
  the moving water's material, and the mod took them for water, so the
  rock panel stood bare and brown in a white wall (JackandBeans's
  picture, Oct 20 2026). A panel is told from water by its own palette,
  earth colored where the water's is blue or absent and the lava's red;
  across the seven courses only those three panels have one.
- Custom has a **Dawn Mist** row, On or Off: the mist at dawn on the
  River and the Valley, in the fog and on the water. Simple and Live keep
  it on, as before. The Effects at Night row now says spray for the
  game's own water, since mist is the dawn's.
- A meteor shower on every clear night: streaks every few seconds from
  one point of the sky, each with its own length and brightness, the
  radiant drawn for the ride and kept by its photos. Under Live the peak
  nights of the year's seven showers (the Quadrantids, the Lyrids, the Eta
  Aquariids, the Perseids, the Orionids, the Leonids and the Geminids)
  come at their own pace, thickest for the Geminids and the Perseids, and
  the details page names the shower. The lone shooting star of 1.1.0 is
  part of it. Asked for by JackandBeans on Oct 23 2026, first as a rare
  thing, then, having seen it, for every night.
- A photo keeps the sky it was taken under. The game draws a saved photo's
  world afresh each time, and the mod made it the rows' sky, so with Time
  at Night every photo in the album had turned to night (JackandBeans,
  Oct 23 2026). Now the shutter writes the hour, the weather of that
  instant, whether an aurora stood and, under Live, the ride's first
  minute into two bytes of the photo's record that the game leaves empty
  and saves with it, and the photo is drawn under that sky whatever the
  rows say, in the album and in Oak's review. A photo from before has no such note and follows the
  rows, as before. The moon's phase in a photo is by the day it is looked
  at, not the day it was taken.
- An aurora on some clear nights: two curtains of light low in the sky a
  quarter turn around from where the sun sets, green at the foot and
  violet at the top, their rays brightening and fading across them, their
  feet wavering, taller where they are brighter, the whole breathing over
  forty seconds, with the stars showing through. They are drawn as the
  horizon's glow is, strips of colored corners and no picture, in the
  N64's own way, and move by the world's clock, so a photo's night has
  the curtains its instant had. On a fixed hour one ride in three has
  one; under Live the latitude decides, most nights beyond 55 degrees
  north or south and about one in forty near the equator, the same all
  night and on the other side south of the equator; Custom has an
  **Aurora** row, Some nights, Every night or Off. The Tunnel has none.
  Chosen by JackandBeans on Oct 23 2026 from three ways to make the night
  sky more interesting.

## 1.1.1

- The Tunnel has the stars and the moon at night, and the sunset's and
  the dawn's sun, over both its open ends, as every other course has;
  1.1.0 left its painted sky bare. They stand behind the hills and the
  rock, so the mouth and the arch take them as the cart passes. Asked
  for by JackandBeans on Oct 17 2026.
- A photo of the Tunnel keeps those stars, as the photos are checked
  after the ride. The photo's world is drawn by the photo code's own
  camera, which the ride's way of drawing them never saw, so the first
  build showed them on the ride and not in the photo. Found by
  JackandBeans on Oct 17 2026.
- The Tunnel's photos keep the rest of its sky too. A photo taken under
  the roof has the cave's own light and fog, as the ride had there;
  1.1.0 gave every photo of the Tunnel the open air's, so a photo in the
  cave on a rainy night came out grayer and foggier than the ride. A
  photo of an open end keeps its rain or snow, and a storm's flash, as
  the other courses' photos do.
- The Tunnel's open ends have a storm's bolt and flash over the painted
  sky, the shooting stars of a clear night, and Moonlight on Pokémon
  while the cart is under the open sky, as the other courses have. The
  rock and the hills hide the bolt and the stars as they hide the moon.
- Day with Island's own is the cartridge's day on every course. Since
  1.0.1 the River rained by day under Island's own, against that
  version's own note and the README.
- Under Live, the Volcano's ash days and the snow days of a far latitude
  turn at midnight as the island's weather turns, not all at once
  during a ride; and a photo taken before midnight and checked after it
  keeps its own day's weather.
- Under Live, a snowfall that begins during a ride no longer lights the
  bare land as if snow lay on it; snow that lies keeps its light when
  the sky clears.
- The README says that the details page's line and the line over the
  course come under Live or Your clock, and that Live shows the Sun,
  Latitude and Longitude rows.
- These six were found in a review of the code on Oct 17 2026.

## 1.1.0

- The weather is the island's, the same on every course but the Rainbow
  Cloud: rain, a storm or snow falls on the Beach, the River, the Volcano
  and the Valley alike, and snow lies on the Beach's sand as it did on the
  Valley's banks. 1.0.x had snow on the Valley alone, rain and storms
  turned to ash on the Volcano, and no weather at all in the Tunnel.
  Asked for by JackandBeans on Oct 11 2026.
- Ash is the Volcano's own weather now, not a stand-in for its rain: a
  fifth choice on its Custom row, a draw of its own under Random, and
  under Live three days in eight when the island is clear.
- Snow lies as a blanket with form, in the console's own way: the painted
  colors go toward snow by the slope of each face, all of it on ground
  within 32 degrees of flat, none past 66, and a snow line between, where
  a hillside goes from white to its own rock; the snow's color turns with
  the face, cool and a little darker away from the light, warm toward it;
  rock faces stay rock, darker and bluer under snow at any hour; and the
  land's pictures are lifted and compressed under the blanket rather than
  painted white, so the same picture reads as snow under a white face and
  as rock under a dark one. 1.0.x painted every face but the near-vertical
  ones one flat white over whitened pictures, which read as chalk. On a
  flat water's shore, the sand within a few units above the water stays
  wet and bare, the surf line (the Beach's sea); the record is lit again
  once, when the water's height is first known. On the Volcano snow is a
  dusting only, the ground warm, with the rock showing through.
- The Tunnel takes the weather at both ends. Over the open-air start,
  rain, a storm or snow stands as one volume of the world, the slope's
  whole width, from the course's edge to the cave's mouth and a little
  under its arch, as under any eave; the mouth's line is read from the
  block's own triangles (z 1380 of the first block). The rock hides it
  once the cart is under the roof, and the mouth shows it looking back.
  At the far end the second tunnel's roof ends in an arch that opens
  1400 units into the last block, and the weather falls again from a
  little under that arch out past the gate, seen through the arch as the
  gate comes; the end's last camera stands in it. Nothing falls under
  the roof. Under the roof the cave keeps the hour's own light, fog, fog
  distance and background; the weather's light, its nearer fog and the
  snow on the ground stay with the open air. The two are told apart by
  the blocks' own ceilings, a map of every ceiling face from the
  decompilation's meshes; a block with no painted sky, like the hall in
  the middle that draws no ceiling, is cave throughout. Snow lies on the
  whole open stretch, the track with the rest, and the rock picture the
  cave shares with the cliffs outside is not lifted under snow. The
  painted sky at both ends takes the weather and the hour as the other
  courses' skies do, each corner by its height; it stood in panels of
  three tints under snow, and in a lighter panel with a hard edge at
  dusk. The rain's voice recedes with the mouth and comes back as the
  far arch nears; the storm's thunder goes with the open sky. The Custom
  row Tunnel Weather is new. Shaped on JackandBeans's rides and pictures,
  Oct 15 and 16 2026. The weather is drawn into the game's main list
  under the counter and the pointer: the see-through list the Tunnel's
  sky uses is 3072 bytes by the level's setup, and the rain's sheets
  overran it at Electrode's explosion, a hang (found by JackandBeans,
  Oct 11 2026).
- Under Live in the cold months the island's rain is snow on every
  course, and the details page says "snow" where it said "rain".
- The Cave stays as the cartridge has it: lit by its own lights, with no
  opening to the sky.

## 1.0.1

- Day with Rain, Storm, Snow or ash: the weather falls by day, as it does
  under Live. 1.0.0 took Day as the cartridge's own day, untouched,
  whatever the Weather row said, so Day and Snow on the Valley was a
  clear day with a blue sky. Day is now the cycle's day hour held at
  midday when a weather is chosen; under Island's own or Clear it is
  still the cartridge's, untouched. Found by JackandBeans on Oct 11 2026.
- The sky over a day under weather is a flat overcast: pale under snow,
  gray under rain, dark gray in a storm, dun under ash, with the fog and
  the sky's lid matched to it. The day sky had been the cartridge's
  picture dimmed and grayed, and the Valley's deep blue dimmed made a
  night-dark sky over the first snowy day JackandBeans saw.
- The Time row's help line says a change takes effect at the next ride,
  and the Weather row's that snow lies on the Valley alone. The options
  are read as a course loads, so a change made during a ride showed
  nothing until the next one, and Snow over the River was a clear sky
  with no word why: JackandBeans set Day and Snow during a ride on Oct 11
  2026 and saw no change. The README says the same.

## 1.0.0

- The first release, as Island Skies: the Beach, the Tunnel, the River, the
  Volcano and the Valley at night, at sunset, at dawn, through a dusk, or
  live by your own clock, day and night, under the island's own
  weather, which holds from ride to ride and turns during one when its
  hour comes; rain, storms, snow and ash with their wind, the moon's glint
  on the water, the River's fireflies, the Volcano's embers and ash,
  insects in the dark and a shooting star now and then; the moon's phase
  by the date, and the sun's hours, solar noon and the twilight from
  Latitude and Longitude rows, with the seasons in the weather's odds.
  Simple set-up is three rows, Live is one; Custom gives each course its
  own hour and weather. The mod's details page says what Live is doing,
  and a line over the course says the hour and the sky as a ride
  starts ("7:23 PM, clear night") and names a change of weather when one
  comes ("The rain comes").
