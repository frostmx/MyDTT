# Changelog

## 1.0.0

First private MyDTT release.

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

This package is intended for private use and is not a public redistribution of the upstream mod.
