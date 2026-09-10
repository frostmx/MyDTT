# Changelog

## 1.0.0

First private MyDTT release.

- Added demand-based truck dispatch using `(Resource+)` producers and `(Resource-N)` requesters.
- Read station loading direction from its native LOAD/UNLOAD setting.
- Prioritized requesters by their current stock-to-threshold ratio.
- Prevented multiple trucks from claiming the same requester.
- Added optional `(F)` fuel stops and route-based fuel consumption estimates.
- Added inheritance of parenthesized tags from station groups.
- Added `/dtt dispatch` diagnostics.
- Fixed argument handling for `/dtt groups`, `/dtt dispatch`, and `/dtt recalc`.
- Retained the original dynamic train and truck group balancing behavior.

This package is intended for private use and is not a public redistribution of the upstream mod.
