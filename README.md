# omp-wine-injector

A tiny Win32 DLL injector for launching the **open.mp / SA-MP** client inside a
**Wine / CrossOver** bottle on **macOS**.

## Why this exists

The official [open.mp launcher](https://github.com/openmultiplayer/launcher)
injects the client DLLs at runtime with `CreateRemoteThread` + `LoadLibrary`
(via the `dll_syringe` crate). That works because the launcher and the game are
both native Windows processes.

On macOS the launcher is a native Mach-O process and the game is a Windows (PE)
process living inside Wine. The Win32 process APIs (`OpenProcess`,
`VirtualAllocEx`, `CreateRemoteThread`, …) only work Windows-to-Windows, so a
native macOS process cannot inject into the Wine-hosted game. Wine-side ports
that copy DLLs next to the game and rely on a `vorbisFile.dll` proxy work, but
are a different mechanism from upstream.

This project is upstream's exact injection logic, compiled as a small **Windows
`.exe`** that the macOS launcher runs **inside the bottle** (via `cxstart`). So
the injection path matches official open.mp 1:1 — same `CreateRemoteThread`
inject, same vorbis-readiness gate, same retry/back-off — just executed from
within Wine instead of from a native Windows host.

## Behaviour (mirrors the launcher's `inject_dll`)

1. Spawn (or attach to) the game process.
2. Inject each DLL with `VirtualAllocEx` + `WriteProcessMemory` +
   `CreateRemoteThread(LoadLibraryA)`.
3. On the first failure, switch to the **vorbis-readiness** path: wait until the
   game's `vorbisFile` module is loaded (proof it is far enough into init to
   accept a `LoadLibrary`), then inject.
4. Back off `INJECTION_RETRY_DELAY_MS` (250 ms) between attempts, up to
   `INJECTION_MAX_RETRIES` (60).

## Usage

```
injector.exe --spawn "<game.exe>" "<args>" -- <dll1> [dll2 ...]
injector.exe --pid <pid> -- <dll1> [dll2 ...]
```

Example (run inside the bottle, e.g. via CrossOver `cxstart`):

```
injector.exe --spawn "C:\...\gta-sa.exe" "-c -h 127.0.0.1 -p 7777 -n Player" ^
  -- "C:\...\samp.dll" "C:\...\omp-client.dll"
```

Exit codes: `0` all injected · `1` usage · `2` spawn failed · `3` an injection failed.

## Build (on macOS, via Wine MSVC)

```
./build.sh           # -> build/injector.exe  (PE32 x86)
```

Needs CrossOver + an MSVC toolchain fetched with
[msvc-wine](https://github.com/mstorsjo/msvc-wine) at `$MSVC_DIR` (default
`/tmp/msvc`). See `build.sh` for the overridable env vars.

## Notes

- 32-bit target — GTA SA / SA-MP is a 32-bit game.
- This is original code that reproduces a public API call sequence; it contains
  no upstream source.
