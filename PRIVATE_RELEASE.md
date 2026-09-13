# MyDTT 2.2.2 — private release

## Identity

- Display name: `MyDTT`
- Version: `2.2.2`
- Mod Reference: `MyDTT`
- SML dependency: `^3.12.0`
- Supported package targets: Windows client and Windows dedicated server
- Client installation: optional (`RequiredOnRemote: false`); the server-side dispatcher still runs
- Window protocol: 8; save data version: 7

The plugin, content mount and C++ module use the `MyDTT` reference. `DefaultMyDTT.ini` redirects
historical `/DynamicTimeTable` and `/Script/DynamicTimeTable` paths for existing saves. Do not install
this package alongside the upstream plugin: both would register the same `/dtt` command and operate on
the same station routes.

## Build

From PowerShell:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File C:\Users\frost\SF_server\alpakit-dev.ps1 `
  -ModName MyDTT -NoDeploy
```

The merged upload/install archive is produced at:

`C:\SFMod\SatisfactoryModLoader\Saved\ArchivedPlugins\MyDTT\MyDTT.zip`

For personal storage it is copied as `MyDTT-<version>.zip`.

Verified release artifact:

- File: `C:\Users\frost\SF_server\dist\MyDTT-2.2.2.zip`
- Size: `47,199,105` bytes
- SHA-256: `469af773387fcf7c2d9765b671f3a8b163261c65a9d0429beeba3cf0956183ba`
- Targets: `Windows`, `WindowsServer`

## Release notes

> MyDTT adds demand-based truck delivery to DynamicTimeTable. Mark every participating LOAD/UNLOAD
> station with `(Resource)`; an UNLOAD station requests a delivery while its stock is below the
> request limit (40 stacks solid, 56 stacks fluid, or `(Resource-N)` for N stacks / N % of a fluid
> tank) and is filled to the limit plus a buffer. A truck whose route contains a `(D)` depot joins
> the shared pool: it parks at the nearest free depot of its own kind and is sent, when a request
> opens, to the loading station with the most stock, on to as many requesters as its cargo covers,
> and back to a depot; the last requester of a lap drains the leftovers, the depot takes what is
> left. Fluid laps carry a full tank to one station. A truck whose first stop is a producer and
> whose route has no depot runs the 1.x dispatch laps bound to that producer. Optional `(F)` fuel
> stops are used, or borrowed, only when the fuel is low. `/dtt` opens the window with the Groups,
> Dispatch, Trucks and Settings tabs; every tunable is saved with the world and can also be set
> with `/dtt set`.
>
> Since 2.2.1: item icons on the Dispatch and Trucks cards, and the cargo line shows what is
> actually on board (window protocol 8, server and clients must match). Since 2.2.0: a card per
> resource and per truck instead of text rows, a filter line on both tabs, and a pool truck claims
> only a depot of its own kind. The full history is in `CHANGELOG.md`.

## Before using the archive

1. Keep a backup of the save.
2. Remove or disable the upstream `DynamicTimeTable` package first to avoid two dispatchers and two
   `/dtt` commands acting on the same world.
3. Install the matching target from the archive on the server and, for the `/dtt` window, on every
   client that should see it; the versions must match, an older window does not connect.
4. Coming from 1.x: the number in `(Resource-N)` changed meaning in 2.2.0 (it is now the request
   limit, not the free space) — check the tagged stations, see `CHANGELOG.md`.
5. Verify the loaded-mods screen reports `MyDTT 2.2.2`.
6. Run `/dtt dispatch` and confirm station modes and stock before enabling vehicle autopilot.
