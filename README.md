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
which case the next lap starts right there.

How much moves at each stop depends on the *Exact quantities* setting, off by default:

- **Off**: every docking moves what the game would. The loading station fills the truck, each
  requester takes everything that fits, and a truck that runs empty before its last planned stop
  drops the rest of the lap: it starts the next lap from where it is when a request is open, and
  drives to the depot otherwise. The request limit and buffer only decide who is called and how
  many stops fit in one lap.
- **On**: stations move exactly what the lap planned. The loading station gives the truck the
  requesters' shortfall, a requester is filled to its limit plus the request buffer (8 stacks by
  default), which covers what it consumes while the truck drives, and the last requesting station
  of the lap takes everything it can, threshold or not (*Drain leftovers at the last stop*). With
  a *Leftover hold* (0 by default) the truck also stays docked there, unloading again every
  second, until it is empty or the time is up.

Whatever is still on board is unloaded into the depot if the depot is in UNLOAD mode, so a depot
with a container on its output is where leftovers end up; a depot in LOAD mode moves nothing. A
parked truck that still carries the requested resource is sent even when no loading station has
stock, and skips the loading station when its cargo already covers the planned deliveries. The
Trucks tab shows what each truck carries and why a parked one waits.

A truck whose first stop is a `(Resource)` LOAD station and whose route has no depot runs the 1.x
dispatch laps bound to that producer. Saving a route without a depot takes a truck out of the
pool; switching its autopilot off hands it back to you until it next docks at a depot.

`/dtt` opens the window: Groups, Dispatch (a card per resource with its icon, producers and
requesters with stock, call level, fill target and the claiming truck - yellow while a request
waits for a truck, green once one has claimed it, red when nobody has claimed it for the
*overdue* minutes; depots and fuel stops below), Trucks (a card per truck: status badge, route with the current stop lit, cargo bar with
the icon of what is on board, planned deliveries) and Settings; both card tabs have a filter line.
`/dtt dispatch`, `/dtt trucks` and `/dtt depots` print the same in chat; `/dtt park <truck>` and
`/dtt release <truck>` move one truck by hand. The window talks to the server over a versioned
protocol, so the server and the clients that have the mod must run the same version; a client
without the mod plays on such a server without the window.

## Settings

The Settings tab (or `/dtt settings` and `/dtt set <field> <value>`) holds the tunables: *exact
quantities* (off by default, see above), the solid and fluid request limits and the request
buffer, *fluid trucks always carry a full load* (a fluid lap serves one station, the tank is
filled completely and drained there), *drain leftovers at the last stop*, leftover hold (0 by
default) and top-up interval, depot unloading, the producer
skip, the pool dispatch interval, requesters per lap, the requester claim timeout, the overdue
minutes after which an unclaimed request is shown in red (5 by default, 0 turns it off), the
idle recheck interval and the two fuel margins. They are saved with the world; on a dedicated server
the server's values apply and remote clients edit them through the same window.
