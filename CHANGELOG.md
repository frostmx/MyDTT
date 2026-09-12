# Changelog

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
