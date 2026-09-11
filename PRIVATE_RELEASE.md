# MyDTT 1.0.3 — private release

## Identity

- Display name: `MyDTT`
- Version: `1.0.3`
- Mod Reference: `MyDTT`
- SML dependency: `^3.12.0`
- Supported package targets: Windows client and Windows dedicated server
- Client installation: optional (`RequiredOnRemote: false`); the server-side dispatcher still runs

The plugin, content mount and C++ module use the new `MyDTT` reference. `DefaultMyDTT.ini` redirects
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

For personal storage it may be copied as `MyDTT-1.0.3.zip`.

Verified release artifact:

- File: `C:\Users\frost\SF_server\dist\MyDTT-1.0.3.zip`
- Size: `43,850,475` bytes
- SHA-256: `35b53fa610016070a181f047f7d6127f7f2b92534ab55ae732d35ba8580dd26a`
- Targets: `Windows`, `WindowsServer`

## Release notes

> MyDTT adds demand-based truck delivery to DynamicTimeTable. Put a tagged LOAD producer first in the
> truck route and mark every participating LOAD/UNLOAD station with `(Resource)`. The truck's original
> name is irrelevant; MyDTT renames it to `Resource → Current station`. An UNLOAD station requests a
> delivery below 10 full stacks; `(Resource-N)` overrides that threshold. The dispatcher builds each
> lap from the producer to the most starved open requester, prevents trucks from claiming the same
> station, and can include an optional `(F)` fuel stop. A resource label may use the player's language
> or a custom alias when the matching LOAD producer contains one unambiguous cargo item class.
> Parenthesized tags on a group are inherited by every station in that group. Use `/dtt dispatch` to see
> live stock, station modes, open requests, claims and fuel stops.
>
> Since 1.0.0: a truck's own `(F)` fuel stop now survives save/load correctly instead of occasionally
> disappearing from its route. Trucks with no `(F)` of their own automatically borrow the nearest
> reachable one when running low, spreading demand across a `[tag,N]` group instead of piling onto a
> single station. Two related running-dry cases are fixed: the low-fuel check now keeps a real safety
> margin instead of triggering right at the edge, and it keeps working even for a truck whose resource
> currently has no open delivery request.

## Before using the archive

1. Keep a backup of the save.
2. Remove or disable the upstream `DynamicTimeTable` package first to avoid two dispatchers and two
   `/dtt` commands acting on the same world.
3. Install the matching target from the archive on the server and, if the `/dtt` window is needed, on
   the client as well.
4. Verify the loaded-mods screen reports `MyDTT 1.0.3`.
5. Run `/dtt dispatch` and confirm station modes and stock before enabling vehicle autopilot.
