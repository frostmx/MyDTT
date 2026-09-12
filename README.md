# MyDTT

Unofficial DynamicTimeTable fork with dynamic train balancing and demand-based truck dispatch.
Plugin, C++ module and Mod Reference: `MyDTT`. Redirects from the historical
`DynamicTimeTable` paths are shipped for existing saves.

Upstream mod page: https://ficsit.app/mod/3ArTdBPY7fSAYT

Russian user guide: [static site](https://frostmx.github.io/MyDTT/) or
[standalone HTML](USER_GUIDE.html).

The source is public for inspection and personal builds. This fork is not published on Ficsit.app.

## Tags

Everything is configured through station names; the mod stores nothing of its own except what
the save needs to bring a parked truck back parked.

| Tag | Where | Meaning |
|---|---|---|
| `[name]`, `[name,3]` | train or truck station | balancing group, optionally with three slots |
| `(Resource)` | truck station | LOAD: produces the resource; UNLOAD: requests it while not full |
| `(Resource-N)` | truck station, UNLOAD | requests only while more than N stacks are free |
| `(F)` | truck station | fuel stop a dispatch truck may borrow |
| `(D)` | truck station | depot: where pool trucks wait between deliveries |

## How the pool works

A truck whose route contains a `(D)` depot belongs to the shared pool. It drives to the nearest
free depot (one truck per depot) and parks there, held docked, until some `(Resource)` UNLOAD
station is below its threshold. The dispatcher then sends the nearest parked truck of the right
kind (solid or fluid, not carrying anything else) to the loading station with the most stock, on
to the requesting stations - as many as its cargo covers - and back to the nearest free depot,
where it parks again. Stations move exactly what the lap planned: the loading station gives the
truck the requesters' shortfall, a requester is filled to its threshold, a depot moves nothing.

A truck whose first stop is a `(Resource)` LOAD station and whose route has no depot runs the 1.x
dispatch laps bound to that producer. Saving a route without a depot takes a truck out of the
pool; switching its autopilot off hands it back to you until it next docks at a depot.

`/dtt` opens the window (Groups, Dispatch and Trucks tabs); `/dtt dispatch`, `/dtt trucks` and
`/dtt depots` print the same in chat; `/dtt park <truck>` and `/dtt release <truck>` move one
truck by hand.
