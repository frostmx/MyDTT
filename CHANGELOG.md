# Changelog

## 3.0.0

- Stations can name themselves. With *Name stations from their belts* on (Settings, off by
  default), the mod walks the conveyors around every truck dock and writes what it found into the
  station's name: `Factory-2 (Iron Ore) -240/m -> Ingots` - the resource, how much the chain
  behind it wants per minute, and what that chain finally makes. It also sets the station's own
  LOAD/UNLOAD switch, so a new dock needs no configuration at all.
- Text you wrote yourself is kept: everything left of the tag is yours, and a name ending in a
  full stop is yours entirely and is never touched. `(F)` and `(D)` stations are left alone.
- Your own word for a resource survives: `(Bauxite)` written in your language stays as you typed
  it for as long as the belt carries that item. The mod only writes the game's name where it
  cannot tell what you meant.
- The number in a tag you wrote, as in `(Motor-40)`, survives a rewrite; it is dropped only when
  the chain starts carrying something else, because it was a level for a different cargo.
- What the tail means: `-` the station feeds the factory (a truck brings this), `+` the station
  collects from it (a truck takes this away), `*` more than one station carries the same cargo into
  the same chain and both show its whole demand, `?` the mod could not tell what this station
  carries - the dock was empty and the chain wants several things - so the biggest one won. Write
  the resource in the brackets yourself and the mark goes away.
- Smart and programmable splitters are read, not guessed: a branch filtered to another item is not
  counted. What a station carries is taken from its own cargo, or,
  when the dock is empty, from the resource already in its name: which ingredient arrives on a
  belt cannot be read off the connection, because the game sorts an arriving item into whichever
  slot accepts it. A container between the dock and the factory is a buffer, and the chain is
  followed through it. Rates follow the machine's current overclock.
- `/dtt autoname` lists what would change without changing anything; `apply` writes it all,
  `strip` takes the captions back off and leaves the
  resource tags in place, so the stations keep working, and `/dtt autoname <station>` explains one dock: the
  chain it feeds, every machine on it and the name it would get.
- Recalculated a couple of seconds after anything is built, dismantled, re-piped or renamed, and
  once a minute for everything else (that is what notices an overclock change).
- Window protocol 12: the settings carry five new fields. Server and clients must match.

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
