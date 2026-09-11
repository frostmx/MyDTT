# Changelog

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
