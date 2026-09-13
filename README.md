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
| `(Resource)` | truck station | LOAD: produces the resource; UNLOAD: requests it below the settings' limit (40 stacks solid, 56 stacks fluid) |
| `(Resource-N)` | truck station, UNLOAD | solid: requests while fewer than N stacks are in stock; fluid: while the tank is below N % |
| `(F)` | truck station | fuel stop a dispatch truck may borrow |
| `(D)` | truck station | depot: where pool trucks wait between deliveries |

## How the pool works

A truck whose route contains a `(D)` depot belongs to the shared pool. It drives to the nearest
free depot (one truck per depot) and parks there, held docked, until some `(Resource)` UNLOAD
station is below its limit. The dispatcher then sends the nearest parked truck of the right
kind (solid or fluid, not carrying anything else) to the loading station with the most stock, on
to the requesting stations - as many as its cargo covers - and back to the nearest free depot,
where it parks again - unless another request is open when it leaves its last requester, in
which case the next lap starts right there. Stations move exactly what the lap planned: the
loading station gives the truck the requesters' shortfall, a requester is filled to its limit plus
the request buffer (8 stacks by default), which covers what it consumes while the truck drives.

Leftovers: the last requesting station of a lap takes everything it can, threshold or not, and
the truck stays docked there, unloading again every second, until it is empty or the *Leftover
hold* time is up (60 s by default; 0 turns it off). Whatever is still on board is unloaded into
the depot if the depot is in UNLOAD mode, so a depot with a container on its output is where
leftovers end up; a depot in LOAD mode moves nothing. A parked truck that still carries the
requested resource is sent even when no loading station has stock, and skips the loading station
when its cargo already covers the planned deliveries. The Trucks tab shows what each truck
carries and why a parked one waits.

A truck whose first stop is a `(Resource)` LOAD station and whose route has no depot runs the 1.x
dispatch laps bound to that producer. Saving a route without a depot takes a truck out of the
pool; switching its autopilot off hands it back to you until it next docks at a depot.

`/dtt` opens the window (Groups, Dispatch, Trucks and Settings tabs); `/dtt dispatch`, `/dtt trucks`
and `/dtt depots` print the same in chat; `/dtt park <truck>` and `/dtt release <truck>` move one
truck by hand.

## Settings

The Settings tab (or `/dtt settings` and `/dtt set <field> <value>`) holds the tunables: the
solid and fluid request limits and the request buffer, *fluid trucks always carry a full load*
(a fluid lap serves one station, the tank is filled completely and drained there), *drain
leftovers at the last stop*, leftover hold and top-up interval, depot unloading, the producer
skip, the pool dispatch interval, requesters per lap, the requester claim timeout, the idle
recheck interval and the two fuel margins. They are saved with the world; on a dedicated server
the server's values apply and remote clients edit them through the same window.
