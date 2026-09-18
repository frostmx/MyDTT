# Changelog

## 3.4.0

- A Timetable tab, which answers the question the other tabs only circle around: what is happening
  right now. Three blocks on one page - the laps being driven, the requests waiting, and what the
  dispatcher could not do on its last pass.
- A lap is drawn as its stops, one mark each, with the one the truck is driving to lit. Where a
  truck is in its round is a glance now; as a line of station names it was text nobody read.
- Next to it, the metres of road left before the lap ends. It is road, walked leg by leg over the
  vehicle path network, not a line drawn across the map - only the first piece, from wherever the
  truck stands to the stop it is heading for, is straight, because a truck between two nodes is
  not somewhere the network can be asked about. There is deliberately no arrival time: the game
  never says what speed a truck will hold, and a made-up ETA is worse than none. "/dtt why" prints
  the same number, so the window can be checked against something.
- The queue is the open requests in the order the dispatcher itself will take them - an empty
  station first, then whoever has waited longest - so the page cannot invent an order of its own.
- "Why nothing moved" counts the reasons the last pool pass gave up, per resource: no producer
  with stock, no free truck that fits, the truck is carrying something else, solid against fluid,
  no road. They are recorded where the dispatcher already logs them, as it runs, rather than
  worked out again afterwards - which would be a second opinion, and a path search per resource
  on every refresh.

## 3.3.0

- A Stations tab: every docking station in the world in one table, whatever it carries - the
  resource ones, the depots and the fuel stops, which belong to no resource and so appear in no
  row of the Dispatch tab. Name, group, what it is, which way its switch is set, its stock against
  the line it calls at, the limit its name spells out, what it is doing and how long it has been
  doing it. The filter takes a name, a resource or a group.
- The limit is editable from the window at last, instead of by renaming the station in the world.
  Tick the rows, put a number in the box above the table, and every ticked station gets it; the
  box is above the table and not in each row because a control inside a row is recycled into
  another station a second and a half later. Four or more stations ask twice before the rename.
- Setting a limit rewrites only the inside of the tag. A station named by the mod keeps the
  caption it was given, a station named by a player keeps every character of it.
- An open station row also offers what used to need the chat or the station's own panel: keep the
  truck until empty (the "=" marker), drop the claim a truck is holding, and ask the dispatcher to
  look at this station now.
- The rows of the new table cost the wire almost nothing: the names go into the tables the snapshot
  already carried and the eight flags of a row into one byte, so four hundred docks are about 20 KB
  instead of the 65 KB that would have been dropped in silence.

## 3.2.0

- The Dispatch tab is a table. A resource is one row: its producers, a small bar for every
  requester, what state it is in and the longest anybody has waited. On each bar the stock is
  filled in and two marks show where that station starts calling and what a delivery fills it to,
  so which stations are about to call is a glance instead of three numbers read one at a time.
  Twenty rows fit on the screen where four did.
- The Trucks tab is a table the same way: cargo, name, phase, where it is going, how full it is.
- A row opens where it stands, one at a time, and the detail that used to fill the whole card is
  inside it. The rest of the page does not move when a row opens.
- An open truck row carries four buttons: rebuild the lap, send to a depot, release, and the
  autopilot. They are the same operations `/dtt park` and `/dtt release` run, so nothing can drift
  apart between the chat and the window. Switching an autopilot off asks twice, because without it
  the mod stops planning for that truck.
- The window takes a share of the screen rather than a fixed 1120x820, so a big monitor shows more
  rows. Column captions stay above the table when it scrolls, and the result of the last thing
  asked for is on a line at the bottom, readable from every tab.
- The snapshot names every station and truck once and the rows point into those tables. A truck's
  route used to travel as full station names on every truck, which would have overrun the 64 KB a
  single call may carry on a large enough world - a call the server drops in silence.

## 3.1.0

- A station can ask for the full load. "=" right behind the resource in its tag - `(Bauxite=)`,
  `(Bauxite=-10)` - keeps the truck standing at that station, draining into it and topping it up,
  until its cargo is gone. It is what the StayUntilEmpty setting asks of every requester in the
  world, asked here by one station, so the two or three lines that want a whole truckload no
  longer force it on all of them. A station that stops consuming keeps the truck, which is why
  this is written per station and off unless the name says otherwise.
- The game's own station window carries a NORMAL/FULL switch under the vehicle settings that turns
  the marker on and off - the game's own kind of switch, under its UNLOAD/LOAD one. The marker
  lives in the station's name, so the switch only ever shows what the station is called, and typing
  the "=" by hand does exactly the same thing. A station with no `(Resource)` tag has no switch.
- Auto-naming keeps the marker when it rewrites a tag, the way it already keeps a hand-written
  limit. A station the belts turn into a producer loses it: standing until empty is a requester's
  business.
- `/dtt why` says which of the two asked the truck to wait - the station's tag or the setting.
- Fixed: the station window no longer writes back the name it was opened with. Its name field kept
  that text and gave it to the station whenever it lost focus, so a name changed while the window
  stood open - by auto-naming, by another player, by the new switch - came back as the old one a
  moment later. The field now follows the station, unless you are typing your own name into it.
- Fixed: auto-naming no longer writes a second caption after the first. A name that lost its tag
  but kept its caption read as if the caption were your own text, and the pass added another one:
  `(Silica) -200 for Aluminium -1500/m -> Aluminum Ingot -1500/m -> Aluminum Ingot`. A name already
  spoiled that way is put right on the next pass.
- Client build changed: `RemoteVersionRange` is now `>=3.1.0 <3.2.0`, window protocol 14.

## 3.0.2

- The pool serves the request that has waited longest. The dispatcher used to rank open requests
  by how far below the threshold they were, so a small station that dipped a little long ago kept
  losing every free truck to a big one that had just dipped a lot. Now an empty station comes
  first, then the request that has been open and unclaimed the longest; the stock ratio only
  breaks ties. The same order picks which stations a pool lap visits. Legacy trucks on their own
  routes still take the most starved requesters. `/dtt why` prints the waiting time of the first
  request of every resource.
- Client build unchanged; `RemoteVersionRange` stays `>=3.0.0 <3.1.0`.

## 3.0.1

- The game's own "Truck Station" is no longer copied into a name. A station nobody has renamed
  used to come out as `(Steel Ingot) Truck Station -60/m -> Steel Beam`, with the default text
  baked in the server's language; it is now `(Steel Ingot) -60/m -> Steel Beam`. A head that is
  that default text - left by an earlier version - goes the same way on the next pass. Text you
  typed yourself, even if it is "Truck Station", is a plain string and stays.
- A station that gets a new resource word takes the word the other stations already use for that
  item: `(Cubes)` on three stations makes the fourth `(Cubes)` too, not `(Modular Frame)`. The most
  common word wins where they differ; the game's name is used only where nobody has given the item
  a word. This applies to new stations, to a station whose cargo changed, and to `apply`, which
  rewrites game words; the timed sweep still never rewrites a word.
- `/dtt autoname gamenames` previews, and `/dtt autoname gamenames apply` writes, the game's names
  back into every resource tag: a word that is an item's name in any language, or that the stations
  carrying it hold, becomes that item's name in the naming language. The number, your text, the
  group and the caption stay. Every station with a resource tag is covered, including the ones the
  auto-namer skips; words that stand for nothing are listed and left alone; `(F)`, `(D)` and names
  ending in a full stop are not touched.
- The warehouse mark is a crate, `+▣` / `-▣`: the plain square looked like a missing glyph. Names
  written with the old square are still read and get the crate on their next pass.
- A station you just placed, renamed, or ran a belt or pipe to is named first. Building anything
  queues every dock for a new walk, a few per second, and a new station used to wait its turn
  through the whole list - about a minute on a big factory. Now it is walked two seconds after
  the belt lands.
- Truck cards in the window no longer show the truck's object number after its name; the number
  is still what `/dtt trucks` prints and `/dtt why` takes.
- Dispatch cards show an auto-named station by its caption, `-5/m -> Adaptive Control Unit`,
  instead of "(no name)" when the station has no text of its own.
- Server and client may run different 3.0.x versions: the descriptor now declares
  `RemoteVersionRange >=3.0.0 <3.1.0`. From here on, a release that does not change the client
  build bumps the last number and keeps the range; one that does bumps the middle number and
  moves the range with it.

## 3.0.0

- Stations can name themselves. With *Name stations from their belts* on (Settings, off by
  default), the mod walks the conveyors and pipes around every truck dock and writes what it found into the
  station's name: `(Iron Ore) Factory-2 -240/m -> Ingots` - the resource, how much the chain
  behind it wants per minute, and what that chain finally makes. The tag comes first, so a
  station list sorted by name falls into resources. It also sets the station's own
  LOAD/UNLOAD switch, so a new dock needs no configuration at all.
- Text you wrote yourself is kept: everything between the tag and the tail is yours, and a name ending in a
  full stop is yours entirely and is never touched. `(F)` and `(D)` stations are left alone.
- Your own word for a resource survives: `(Bauxite)` written in your language stays as you typed
  it for as long as the belt carries that item, and a word the game does not know in any language
  stays whatever the dock holds - it is your name for what the trucks carry. The mod only writes
  the game's name where your word is the game's name for a different item - or when you pick
  another naming language and the word is one of the game's own (see below).
- The number in a tag you wrote, as in `(Motor-40)`, survives a rewrite; it is dropped only when
  the chain starts carrying something else, because it was a level for a different cargo.
- What the tail means: `-` the station feeds the factory (a truck brings this), `+` the station
  collects from it (a truck takes this away), `*` more than one station carries the same cargo into
  the same chain and both show its whole demand, `?` the mod could not tell what this station
  carries - the dock was empty and the chain wants several things - so the biggest one won. Write
  the resource in the brackets yourself and the mark goes away.
- Smart and programmable splitters are read, not guessed: a branch filtered to another item is not
  counted. What a station carries is taken from its own cargo, or,
  when the dock is empty, from the resource already in its name. A station nobody has delivered to
  yet - a new one - is named from what the factory behind it is missing: the ingredient its
  machines need and no belt already brings. Where even that is ambiguous, because both ingredients
  come by truck, the port the belt lands on decides and the name is marked as a guess; the first
  delivery corrects it either way. A container between the dock and the factory is a buffer, and the chain is
  followed through it. Rates follow the machine's current overclock.
- `/dtt autoname` lists what would change without changing anything; `apply` writes it all,
  `strip` takes the captions back off and leaves the
  resource tags in place, so the stations keep working, and `/dtt autoname <station>` explains one dock: the
  chain it feeds, every machine on it, the name it would get and which signal decided it.
- Recalculated a couple of seconds after anything is built, dismantled, re-piped or renamed, and
  once a minute for everything else (that is what notices an overclock change).
- Pipes count too. The walk follows pipelines, junctions, pumps and valves the way it follows
  belts, so a fluid truck station is named like a solid one - `(Crude Oil) Refinery-1 -600/m ->
  Plastic`, rates in cubic metres per minute - and a fluid buffer is a buffer like a container.
  Alumina reaching the foundries by pipe now counts as supplied, so the silica station is no
  longer asked to bring it.
- A generator is a consumer. The station feeding a row of fuel generators shows what they burn
  and, at the end of the tail, what they make: `(Fuel) -20/m -> 1250 MW`. Water for coal and
  nuclear plants is counted as well.
- A station with no machine behind it - only a container or a train platform - is a warehouse. It
  is named after its cargo with `+▣` or `-▣` in place of a rate: the sign says which way the
  trucks go, the crate says nothing is consumed or made here.
- The names can be written in any language the game itself is translated into: *Language of the
  names* in Settings, English by default, `/dtt set naminglanguage ?` for the codes. Only the item
  names change and they are the game's own translations; the mod's window and chat stay English.
  Changing the language rewrites the resource words that are the game's names for items, in any
  language; a word of your own, which the game has in no language, stays as you typed it. An
  ordinary sweep never touches those words - only `apply` and a language change do - so a world
  full of tags in your language looks the same after the update until you ask.
- A collecting station shows what reaches it, not what the line makes. At every splitter on the
  way the demand of the other branches comes off, at every merger the supply of the other inputs
  comes off, the way EfficiencyCheckerMod counts it: ten refineries make 200 plastic, four
  constructors on the side branches take 100, the dock says `+100/m`, and `/dtt autoname
  <station>` says "100/min of the 200/min the line makes". A sink, a container or another
  station on a side branch takes nothing, nor does an overflow branch. Pipes have no direction to
  discount along, so a fluid station still shows the whole network.
- Fixed: a belt on a station's fuel input was read as the station's cargo, so a coal belt into a
  truck stop's fuel slot made it a coal collector. Only the storage inputs count now.
- Window protocol 13: the settings carry six new fields. Server and clients must match.

## 2.5.0

- Pool laps are no longer padded with copies of the depot at all. The autopilot keeps its target
  index across a route write, so the lap is now turned to that index instead: the ring
  producer - requesters - depot is rotated until its first stop lands on the index the truck
  holds. The stop count is the lap's own, the route in the vanilla panel is the route driven, and
  the chain between laps no longer has a length limit.
- `/dtt why <truck>` explains one truck: its phase, route and target, what it carries, the lap it
  is running or the hold it is under, and - for a truck waiting at its depot - every open request
  with the reason this truck is not taking it (wrong form, foreign cargo, no producer with stock).
  Plus what its tank covers, in laps of the route it is on.
- New setting *Stay at the requester until empty* (off by default): the truck stays docked at each
  stop of its lap, topping it up as it consumes, until its cargo is gone - and only then drives
  on. Meant for a consumer that should be served to the end rather than visited.
- With *Exact quantities* off, a lap is planned by what the stops can hold, not by what they are
  short of. The game's own transfer empties the truck into the first stop with room for it, so
  planning by the request target sent trucks to four stops and unloaded everything at the first.
- The two fuel margins that were constants are settings now: *Leg guard margin* (how many times
  the estimated cost of a leg must be in the tank before it is driven, 2) and *Detour reach* (the
  share of the tank a detour to a borrowed fuel stop may cost, 0.8).
- Window protocol 11: the settings carry three new fields. Server and clients must match.

## 2.4.3

- Pool trucks no longer collect a growing string of depot stops at the front of their route. The
  autopilot keeps its target index across a route write, and every chained lap pushed it two
  stops further; the route was padded with copies of the depot to match. Now the lap boundary
  writes a two-stop depot loop first, the autopilot resets the index on its next tick, and the
  next lap starts with the producer at the front. A parked truck's route is reset to the depot
  loop as well, so the vanilla panel shows the stops it will actually drive.
- *Drain leftovers at the last stop* works with *Exact quantities* off too. Until now the drain
  and the leftover hold were switched off together with the quantities; with them off the drain
  plan moves what the game would, and the hold (*Leftover hold, seconds*) is the difference.
- Window protocol unchanged (10); save data unchanged.

## 2.4.2

- No more extra fuel stop after a refuel: when a truck reaches its fuel stop at the end of a lap,
  the next lap is planned as if the tank were already full. Previously the same pump was visited
  once more a lap later with a nearly full tank.
- Window protocol unchanged (10); save data unchanged.

## 2.4.1

- A fuel stop that belongs to a `[group]` now stands for the whole group: a truck heading for it
  refuels at whichever member of the group is least busy. This applies to a truck's own `(F)` in
  its route, to a borrowed one, and to pool laps.
- Trucks no longer run dry between stops. Before every leg the truck checks that its tank covers
  the leg twice over plus the detour to the nearest fuel stop after it; if not, it refuels first.
  This also covers a lap that was cut short at an empty requester and a pool truck leaving its
  depot.
- One fuel stop per lap: when a refuel is inserted, a fuel stop planned further along the same lap
  is dropped.
- Pool trucks' fuel stops now count towards the group load shown in the window.
- Window protocol unchanged (10); save data unchanged.

## 2.4.0

- New setting *Exact quantities* (`ExactQuantities`), **off by default**. Off, every docking moves
  what the game would: the loading station fills the truck, each requester takes everything that
  fits, and the request limit and buffer only decide who is called and how many stops fit in a
  lap. On restores the 2.x behaviour: the producer gives the lap's shortfall, a requester is filled
  to its target, the last one drains the rest. A world saved by 2.3 or earlier comes up with the
  setting off; turn it on in the Settings tab or with `/dtt set exactquantities on`.
- A pool truck that runs empty before its last planned stop now starts its next lap from where it
  is when a request is open (`POOL_LAP_TRIMMED`, then the usual `POOL_LAP_DONE`), instead of
  driving back to the depot first. Without an open request it drives to the depot as before.
- The drain at the last requester no longer depends on the hold. *Drain leftovers at the last
  stop* alone makes that station take everything it can during its unload; *Leftover hold* is an
  extra stay on top and now defaults to 0 (it used to be 60, and 0 switched the drain off too).
  Worlds keep the hold they have saved.
- Window protocol 10 (the settings carry the new field); server and clients must run the same
  version. Save data is unchanged.

## 2.3.0

- Request colours in the Dispatch tab swapped to match their meaning: yellow is an open request
  nobody has taken yet, green is one a truck has claimed.
- A request nobody claims for too long turns red, together with its resource card, and the
  status line shows how long it has waited. New setting *Overdue after, minutes*
  (`OverdueRequestMinutes`, default 5, 0 turns it off); the server logs
  `DISPATCH_REQUEST_OVERDUE` once per such episode and `/dtt dispatch` prints `waiting=Nm`.
- The *Recalculate bookings* button is shown on the Groups tab only: it rebuilds the group
  bookings of the balancer and has no effect on the other tabs.
- Window protocol 9 (the rows carry the waiting time and the settings the new field); server and
  clients must run the same version. Save data is unchanged.
- Fix: a fluid truck without a depot under *fluid trucks always carry a full load* now drains and
  holds at its last requester on every lap. On a lap planned without an open request the producer
  still filled the tank, but the requester was only filled to its target and the truck left at
  once with the rest on board.

## 2.2.2

- Item icons in the window: each resource card in the Dispatch tab and each truck card in the
  Trucks tab shows the item's icon next to its name, and the cargo line shows what is actually
  on board. Window protocol 8: the rows now carry the item class, so a remote client draws the
  icons too. Server and clients must run the same version.

## 2.2.1

- The Dispatch tab shows a card per resource: producers on the left with their stock,
  requesters on the right with stock, call level, fill target, a fill bar and who holds the
  claim; depots and fuel stops get their own cards. The Trucks tab shows a card per truck with a
  status badge, the route with the current stop lit, a cargo bar, the cargo note and the plan.
  The wall of text lines is gone; `/dtt dispatch` and `/dtt trucks` in chat are unchanged.
- Both tabs have a filter line: type part of a station or truck name or a resource, any case,
  and only the matching cards stay.
- A truck only claims a depot of its own kind: fluid trucks park at fluid truck stations, solid
  trucks at solid ones. The game never runs a transfer between the two, so a leftover could not
  drain there. A depot claimed before this rule is released and re-picked when the truck next
  looks for one; a truck already parked at the wrong kind stays until its next lap
  (`DEPOT_FORM_MISMATCH` in the log). `/dtt park` says which kind it could not find.

## 2.2.0

- **The number in a requester tag changed meaning.** `(Resource-N)` on a solid station now
  means "call a truck while fewer than N stacks are in stock"; on a fluid station N is a percent
  of the tank. A delivery fills the station up to the limit plus a buffer (new *Request buffer*
  setting, 8 stacks), so what is consumed while the truck is on its way is covered. Until now N
  was the number of stacks kept free. Check the tags you have: to keep the old behaviour of
  `(Motor-40)` on a 48-stack station write `(Motor-8)`; a number the station cannot hold is
  clamped and reported once (`DISPATCH_TAG_LEVEL_CLAMPED`).
- A tag without a number uses the new *Solid request limit* (40 stacks) and *Fluid request
  limit* (56 stacks of 50 000 L) settings; with the buffer both fill a station to full, so the
  shipped behaviour matches the previous "called while not full" except that a single missing
  stack no longer calls a truck.
- *Fluid trucks always carry a full load* (on by default): a fluid lap serves one station, the
  producer fills the tank completely and the truck drains it at that station, topping up while
  the station drinks, until the leftover hold runs out.
- *Drain leftovers at the last stop* switch: off, the last requester is filled to its target
  like any other stop and the truck keeps its leftovers for the next lap.
- The Settings tab scrolls and is grouped into Requests, Leftovers, Pool and Fuel; it no longer
  runs under the window's footer.
- Window protocol 7. Server and clients must run the same version; the save format is unchanged.

## 2.1.0

- Leftovers no longer ride around. The last requesting station of a lap takes everything it can,
  threshold or not, and keeps the truck docked, unloading again every second, until the truck is
  empty or a minute has passed (the *Leftover hold* setting). What is still on board goes into
  the depot when the depot is in UNLOAD mode, now and for as long as the truck stays parked.
- A parked pool truck that still carries the requested resource is sent even when no loading
  station has stock, and skips the loading station whenever its cargo already covers the
  planned deliveries.
- A pool truck that leaves the last requester of its lap while another request is open takes
  it from there (`POOL_LAP_DONE`, then `POOL_ASSIGNED … Trigger=lap end`) instead of driving
  back to the depot first; the depot is only for a truck nobody needs.
- The Trucks tab shows what each truck carries and why a parked truck is waiting (its cargo
  matches no request, or is mixed); the log says so once per parking (`POOL_CARGO_WAIT`).
- New Settings tab in `/dtt`: leftover hold and top-up interval, depot unloading, the
  producer skip, dispatch interval, requesters per lap, claim timeout, idle recheck and the two
  fuel margins. Saved with the world; the server's values apply, remote clients edit them
  through the same window. `/dtt settings` and `/dtt set <field> <value>` in chat.
- A pool lap now gets its fuel stop: the estimate failed on the lap's return to the depot it
  started from, so no pool lap ever included one.
- A truck that leaves an (F) stop with no more fuel than it arrived with is reported in the log
  (`FUEL_STOP_EMPTY`, with the pump's stock); a refuel as `FUEL_REFUELLED`.
- A resource named in the player's own words is resolved from the requesters' stock when no
  tagged producer holds it, so a parked truck can be sent from its own cargo without a producer.
- Window protocol 6, save data version 7. Server and clients must run the same version; an
  older save keeps the shipped defaults and the first lap after it has no plan to drain by.

## 2.0.0

- Trucks can now share a pool instead of each being bound to one producer. Tag a truck station
  `(D)` to make it a depot: a truck whose route contains a depot joins the pool, drives there and
  waits, docked, until a `(Resource)` station somewhere requests a delivery. The dispatcher then
  sends the nearest suitable parked truck to the loading station with the most stock, on to the
  requesting stations, and back to the nearest free depot. Solid and fluid trucks are matched to
  the resource's form; a truck carrying something else is not sent. Trucks without a depot in
  their route keep working exactly as in 1.x.
- Stations now move exactly what the delivery plans: a requesting station is filled up to its
  threshold and no further, the loading station gives the truck only what the requesters of the
  lap can take, and a depot moves nothing. Before, a docking moved everything that fit, which is
  why `(Resource-N)` had to be a workaround for "leave N stacks free".
- Docking is now observed directly instead of by polling, so bookings and claims are released the
  moment a truck docks and a lap's next step is decided the moment it leaves.
- The `/dtt` window has three tabs: Groups (as before), Dispatch (every tagged station, depot and
  fuel stop with stock, threshold, capacity, claim and the planned delivery) and Trucks (every
  dispatch and pool truck with its phase, depot, route, target and cargo). Rows update in place.
- New chat commands: `/dtt trucks [resource]`, `/dtt depots`, `/dtt park <truck>`,
  `/dtt release <truck>`.
- Server and clients have to run the same version: the window protocol is now 5. Saves gain the
  pool state (save data version 6); older saves load unchanged.
- Fixed a requesting station being released for other trucks while the truck assigned to it was
  still loading at the producer: the autopilot moves the target on at docking, and the release
  looked at the target instead of the station the truck was standing at. With several trucks per
  resource this let two of them head for the same station.

## 1.1.0

- A requester's threshold is now the station's capacity: an UNLOAD station takes whatever fits,
  so "not full" is the only demand it can express, and a neutral `(Resource)` tag requests a
  delivery whenever the station has room. `(Resource-N)` now means capacity minus N stacks - the
  station requests only with more than N stacks free. The old fixed defaults (10 stacks for
  solids, 32 for fluids) are gone, and the lap's cargo deficit is exactly the room the chosen
  stations can take. An N at or above the capacity never opens the station; that is reported
  once per rebuild as `DISPATCH_TAG_OFFSET_EXCEEDS_CAPACITY`.
- A producer that belongs to a `[group]` is now picked per lap by the group's balancing rule:
  among the group's LOAD stations of the same resource, the one with the fewest trucks booked per
  slot, ties going to the fuller station. Several loading stations of one resource thus share the
  trucks; the truck books the chosen producer until it docks there.
- `/dtt dispatch` shows the requester's capacity next to its stock and threshold.

## 1.0.9

- A truck now serves several requesters in one lap. At the producer the dispatcher takes the most
  starved open requesters, one after another, until their combined shortfall covers what the truck
  can carry (at most four per lap), and writes them all into the route. A truck that runs out of
  cargo on the way turns back to the producer instead of driving on to the remaining stops; those
  stops are released for other trucks.
- With no open request a truck now keeps only its producer and the last consumer of the previous
  lap, instead of replaying the whole previous lap empty.
- A fuel stop, own or borrowed, is now placed where it adds the least detour to the lap and can
  still be reached on the current tank, instead of always at its remembered position (own) or
  right after the producer (borrowed). The remembered position remains the fallback when the path
  network cannot be costed.
- Fluids: a resource tag naming a fluid (for example `(Crude Oil)`) now resolves by name; before it
  only worked through the producer's cargo. For fluids one "stack" is 50 m³, so a fluid truck
  station holds 64; the default requester threshold for a fluid is 32 stacks (1 600 m³), solids
  keep 10. An explicit `(Resource-N)` threshold is unchanged.
- `/dtt dispatch` lists the dispatch trucks with their current route, target and cargo.
- Fixed the game's own route re-saves being taken for player edits. The game writes a truck's
  unchanged route back on every docking; the dispatcher released that truck's remaining claims
  and treated the lap as hand-edited each time. Only a route that actually differs counts now,
  and a route the game writes back after a docking that started before the dispatcher rebuilt
  the lap is restored rather than taken as an edit.

## 1.0.8

- A fuel stop borrowed for a low tank is now visited right after the producer instead of at the
  end of the lap. Before, a truck flagged low could be asked to finish its whole delivery lap
  before reaching the pump, and could run dry on the way.

## 1.0.7

- Fixed trucks with their own `(F)` stopping refuelling for good, and trucks with no open request
  getting stuck circling their producer. Both came from 1.0.1's "bypass the fuel stop in place"
  approach, which stopped the dispatcher from noticing that a new lap had begun. It has been
  removed: each lap is now rebuilt at the producer, and a truck's own `(F)` is simply left out of
  laps that don't need it.
- A truck's own `(F)` and its position in the route are now stored in the save, so a lap that
  left it out no longer loses it across save/load. Toggling the autopilot or editing the route by
  hand no longer loses it either; to drop it, untag the station or change the first stop.
- Fixed a borrowed `(F)` occasionally becoming a permanent part of the route after a lap with no
  open request.
- With no open request, a truck keeps driving its last delivery loop, and fuel stops are added only
  when the tank needs them. A truck whose route is only its producer now picks up the first request
  within about ten seconds; before, it could wait forever.

## 1.0.6

- Fixed a truck with its own dedicated `(F)` unconditionally visiting it every lap while its
  resource currently has no active requester, even with a full tank: the check that decides
  whether to skip the stop couldn't estimate a one-stop loop and defaulted to "always visit." It
  now falls back to the truck's actual route (still carrying its last real consumer) so the skip
  decision reflects real fuel state instead of always detouring.
- Raised the critical-fuel margin from 1.5 to 2.0 lap-costs and the skip-fuel-stop margin from 2.0
  to 2.5, keeping the same buffer between "borrow or warn" and "safe to skip" but with more room
  against the engine's fuel-estimate error.

## 1.0.5

- Added an optional resource argument to `/dtt dispatch`, e.g. `/dtt dispatch Iron`: it lists only
  stations tagged with that resource and drops the fuel-stop section, since chat has no scrollback
  and the unfiltered listing gets unreadable with more than a couple of tagged resources.

## 1.0.3

- Fixed the critical-fuel check silently never running for a truck with no dedicated `(F)` while
  its resource currently has no active requester: the freshly-built route used for the estimate
  collapsed to a single stop in that case, which the fuel-cost estimator can't evaluate at all, so
  the check quietly returned "fine" regardless of the tank - even though the truck kept driving its
  last-assigned physical loop and burning fuel on it. The check now falls back to that physical
  route whenever the fresh one is too short to estimate.

## 1.0.2

- Fixed trucks occasionally running completely out of fuel on routes with no dedicated `(F)`:
  the auto-borrow threshold from 1.0.1 had no safety margin (it only kicked in below exactly one
  lap's estimated cost), so estimate error against actual consumption could empty the tank before
  a stop was ever borrowed. Borrowing now triggers with a 1.5-lap margin instead.
- Added a log line when a truck is flagged low on fuel but no reachable `(F)` exists to borrow,
  and raised the "fuel stop borrowed" line to the default log level — both were previously
  Verbose-only and left no trail when a truck ran dry.

## 1.0.1

- Fixed a dedicated `(F)` fuel stop not coming back after a save/load if the tank had been full
  enough to bypass it: the stop now stays in the route and is bypassed in place instead of being
  removed from it.
- Added automatic borrowing of the nearest reachable `(F)` fuel stop when a truck's own route has
  none and its tank is running critically low. If several such stops share a `[tag,N]` balancing
  group, demand spreads across them instead of piling onto a single station.

## 1.0.0

First MyDTT release.

- Changed the Mod Reference to `MyDTT`.
- Added neutral `(Resource)` tags: LOAD stations become producers and UNLOAD stations become
  requesters with a default threshold of 10 full stacks.
- Derived a truck's resource from the tagged LOAD producer at `Route[0]`; the initial truck name is irrelevant.
- Updated dispatch truck names automatically as `Resource → Current station`.
- Retained `(Resource-N)` as a requester threshold override and legacy `(Resource+)` compatibility.
- Added demand-based truck dispatch.
- Inferred the cargo item from the matching LOAD producer when a resource label uses another
  language or a user-defined alias.
- Selected at most one requester per truck and lap.
- Read station loading direction from its native LOAD/UNLOAD setting.
- Prioritized requesters by their current stock-to-threshold ratio.
- Prevented multiple trucks from claiming the same requester.
- Added optional `(F)` fuel stops and route-based fuel consumption estimates.
- Added inheritance of parenthesized tags from station groups.
- Canonicalized inherited group tags as `[Group,N] (Tag)` without repeating the tag inside the square marker or on the station.
- Added `/dtt dispatch` diagnostics.
- Fixed newly created or renamed empty groups not appearing in the group list until a station changed.
- Fixed argument handling for `/dtt groups`, `/dtt dispatch`, and `/dtt recalc`.
- Retained the original dynamic train and truck group balancing behavior.

The source is published as an unofficial fork for inspection and personal builds. It is not
published on Ficsit.app.
