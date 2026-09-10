# MyDTT 1.0.0 — private release

## Identity

- Display name: `MyDTT`
- Version: `1.0.0`
- Internal Mod Reference: `DynamicTimeTable`
- SML dependency: `^3.12.0`
- Supported package targets: Windows client and Windows dedicated server
- Client installation: optional (`RequiredOnRemote: false`); the server-side dispatcher still runs

The internal reference is deliberately unchanged. Renaming it would change the plugin content mount,
Unreal object paths and the client-mod check used before registering the RCO. For a private replacement
build, keeping the reference is safer and avoids a save/content migration.

Do not install this package alongside the upstream `DynamicTimeTable` package: both use the same Mod
Reference and this build is its replacement.

## Build

From PowerShell:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File C:\Users\frost\SF_server\alpakit-dev.ps1 `
  -ModName DynamicTimeTable -NoDeploy
```

The merged upload/install archive is produced at:

`C:\SFMod\SatisfactoryModLoader\Saved\ArchivedPlugins\DynamicTimeTable\DynamicTimeTable.zip`

For personal storage it may be copied as `MyDTT-1.0.0.zip`; the archive contents and internal Mod
Reference must remain unchanged.

Verified release artifact:

- File: `C:\Users\frost\SF_server\dist\MyDTT-1.0.0.zip`
- Size: `43,721,969` bytes
- SHA-256: `bd46699b94ab88c3143baf15d6fe537d04b58bc6300992e86ec8e643156fff52`
- Targets: `Windows`, `WindowsServer`

## Release notes

> MyDTT adds demand-based truck delivery to DynamicTimeTable. Name a truck after the item it carries,
> mark a LOAD station with `(Resource+)`, and mark UNLOAD stations with `(Resource-N)`, where N is the
> restock threshold in full stacks. The dispatcher builds each lap from the producer to every open
> requester, prevents trucks from claiming the same station, and can include an optional `(F)` fuel stop.
> Parenthesized tags on a group are inherited by every station in that group. Use `/dtt dispatch` to see
> live stock, station modes, open requests, claims and fuel stops.

## Before using the archive

1. Keep a backup of the save.
2. Remove or disable the upstream `DynamicTimeTable` package first.
3. Install the matching target from the archive on the server and, if the `/dtt` window is needed, on
   the client as well.
4. Verify the loaded-mods screen reports `MyDTT 1.0.0`.
5. Run `/dtt dispatch` and confirm station modes and stock before enabling vehicle autopilot.
