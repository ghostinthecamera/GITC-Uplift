# GITC Uplift

GITC Uplift is a ReShade add-on that runs NVIDIA DLSS Neural Rendering (DLSS-NR) in games: Direct3D 9, 10, 11 and 12, Vulkan and
OpenGL, 64-bit and 32-bit. You drop it into ReShade's add-on folder next to NVIDIA's runtime, and it works on the game's image: on the
finished frame in every game, and inside the game's own frame, right after DLSS, in games that use DLSS. When you switch NR off, Uplift
releases NR's video memory.

GITC Uplift is not affiliated with NVIDIA or with ReShade.

## Requirements

- An NVIDIA GeForce RTX GPU, a recent NVIDIA driver, and Windows 10 or 11 (64-bit).
- [ReShade](https://reshade.me) **with full add-on support** (the "Addon" build of the installer):
  - 6.0 or newer runs NR on the presented image;
  - 6.1 or newer for NR after DLSS;
  - 6.8 for Vulkan and OpenGL games.
- NVIDIA's DLSS-NR runtime, `nvngx_dlssnr.dll` version 310.8, which **you supply**. Uplift never includes NVIDIA files.
- NVIDIA's DLSS-NR runtime only enables itself on GeForce RTX 50-series GPUs. On other GPUs it refuses to start, and Uplift's status card
  shows NVIDIA's error.
- **Use GITC Uplift instead of other DLSS-NR add-ons**, for example RenoDX's DLSS and DLSS5 add-ons (`renodx-dlss*.addon64`): remove them
  from ReShade's add-on folder. If one is still loaded, Uplift stands down and its card says so.

## Before you start

- **Single-player only.** Uplift hooks NVIDIA NGX calls (in games that use DLSS) and the creation of a Vulkan game's device. Anti-cheat
  systems may object to that. Do not use it in online or competitive games.
- **Antivirus.** Antivirus tools sometimes flag ReShade add-ons and helper executables. Each release publishes the SHA-256 of every file
  in `SHA256SUMS.txt`: compare them, or build Uplift yourself (see "Building from source").

## Install

Download the latest release from [GitHub Releases](https://github.com/ghostinthecamera/GITC-Uplift/releases). A release contains
`gitc-uplift.addon64`, `gitc-uplift.addon32`, `gitc-uplift-helper64.exe`, `Uplift.fx` and `UpliftMask.fx`. Which files a game needs
depends on its API and bitness:

| Game | Files in ReShade's add-on folder |
|---|---|
| 64-bit Direct3D 10, 11 or 12, Vulkan, OpenGL | `gitc-uplift.addon64` and `nvngx_dlssnr.dll` |
| 64-bit Direct3D 9 | `gitc-uplift.addon64`, `gitc-uplift-helper64.exe` and `nvngx_dlssnr.dll` |
| Any 32-bit game | `gitc-uplift.addon32`, `gitc-uplift-helper64.exe` and the (64-bit) `nvngx_dlssnr.dll` |

- If you used an earlier Uplift build, delete its `uplift.addon64`, `uplift.addon32` and `uplift-helper64.exe` first, so that two copies
  do not load.
- ReShade's add-on folder is the game's executable folder unless `AddonPath` in the game's `ReShade.ini` names another one.
- `nvngx_dlssnr.dll` can also sit next to the game's executable, or anywhere you point **Runtime path** (Advanced) at.
- The add-on and `gitc-uplift-helper64.exe` must come from the same release.

### 64-bit Direct3D 10, 11 and 12 games

1. Install ReShade with full add-on support for the game.
2. Copy `gitc-uplift.addon64` and `nvngx_dlssnr.dll` into the add-on folder.
3. Start the game, open ReShade's overlay (Home by default) and switch to the **GITC Uplift** tab.

In a Direct3D 12 game that uses DLSS, NR can run inside the game's frame (after DLSS, before the HUD). Direct3D 10 and 11 games run NR
on the presented image.

### 64-bit Direct3D 9 games

Install ReShade (64-bit, full add-on support) as `d3d9.dll`, then copy `gitc-uplift.addon64`, `gitc-uplift-helper64.exe` and
`nvngx_dlssnr.dll` side by side into the add-on folder. NR runs in `gitc-uplift-helper64.exe`, a small 64-bit process with no window that
Uplift starts when NR turns on. It never outlives the game, and it exits a few seconds after NR turns off, returning all of NR's memory.

### 32-bit games (Direct3D 9, 10, 11 and 12, Vulkan, OpenGL)

NVIDIA's runtime is 64-bit only, so a 32-bit game runs NR in `gitc-uplift-helper64.exe`.

1. Install the 32-bit ReShade with full add-on support for the game: as `d3d9.dll` for Direct3D 9, as `dxgi.dll` for Direct3D 10, 11
   and 12, as `opengl32.dll` for OpenGL, or as the Vulkan layer (see "Vulkan games").
2. Copy `gitc-uplift.addon32`, `gitc-uplift-helper64.exe` and the 64-bit `nvngx_dlssnr.dll` side by side into the add-on folder.
3. The first time NR turns on, Uplift starts the helper. Turning NR on again after it exited takes a second or two.

Plain Direct3D 9 sends each frame through shared memory, about 3 ms per frame at 1080p. **Use Direct3D 9Ex** (Advanced, then restart
the game) brings that to about 0.4 ms by asking ReShade for a Direct3D 9Ex device. Some games do not start on 9Ex: start the game again
and Uplift turns the option off by itself.

### Vulkan games (64-bit and 32-bit, DXVK included)

1. Install ReShade **6.8's Vulkan layer**: run the ReShade 6.8 installer (full add-on support), pick the game's executable and choose
   **Vulkan**. The layer is installed once for the whole PC (replacing an older one asks for administrator rights) and loads for games
   that have a `ReShade.ini` next to their executable. If the installer finds an existing `ReShade.ini`, choose **Update ReShade only**
   (Uninstall deletes `ReShade.ini`).
2. Copy the files for the game's bitness into the add-on folder (see the table above).

- A game that runs through DXVK (DXVK's `d3d9.dll`, `d3d11.dll` or `dxgi.dll` next to it) is a Vulkan game: ReShade must then be the
  Vulkan layer, not a `d3d9.dll` or `dxgi.dll` proxy.
- Uplift adds three sharing extensions to the game's Vulkan device when the game creates it, only when the GPU offers them and the game
  did not ask for them. They let the GPU order NR's work with the game's. `ReShade.log` says what was added. `AdjustVulkanDevices=0`
  turns this off (see "Settings in ReShade.ini").
- In a 64-bit Vulkan game that uses DLSS (No Man's Sky, for one), After DLSS and Before upscaling are available as explicit choices.
  Auto stays at Present, because NVIDIA's driver keeps about 0.5 to 0.6 GB of video memory per device after their first use, until the
  game exits.

### OpenGL games (64-bit and 32-bit)

Install ReShade **6.8** with full add-on support as `opengl32.dll` next to the game's executable, in the game's bitness, then copy the
files for that bitness into the add-on folder. Uplift needs OpenGL 4.5 and NVIDIA's shared-memory extensions, which NVIDIA's driver
provides. NR runs on the presented image.

### Direct3D 8 and 9 games through dgVoodoo2

dgVoodoo2 can run a Direct3D 8 or 9 game on Direct3D 11 or 12. Uplift then treats it as a 32-bit Direct3D 11 or 12 game: frames reach NR
on the GPU, with no copy through memory and no 9Ex switch.

1. Put dgVoodoo2's `MS\x86\D3D9.dll` (or `D3D8.dll`) next to the game's executable.
2. In `dgVoodooCpl.exe`, set **Output API** to a Direct3D 11 entry, or to "Direct3D 12 (feature level 12.0)".
3. Install the 32-bit ReShade with full add-on support as `dxgi.dll`, then Uplift as for any 32-bit game.

Other `d3d9.dll` wrappers (DXVK, game fixes that ship their own `d3d9.dll`) conflict with dgVoodoo2's.

### Optional: Uplift.fx and UpliftMask.fx

Copy them into one of ReShade's effect search paths.

- **`Uplift.fx`** adds the `Uplift` technique. When NR runs on the presented image, enable it and drag it to where NR should run among
  your effects: effects above it see the game's image, effects below it see NR's result. Without it, NR runs before every effect. It is
  also needed for iMMERSE Launchpad's motion vectors (place `Uplift` below Launchpad) and for multisampled back buffers in Direct3D 9 and
  OpenGL games.
- **`UpliftMask.fx`** is an example NR mask. Any effect can write a texture named `UPLIFT_MASK`: its red channel limits NR per pixel (0
  keeps the game's image, 1 is full NR). Uplift uses the previous frame's mask.

## Using it

Open ReShade's overlay and switch to the **GITC Uplift** tab. **Enable** at the top switches NR on and off; **Enable hotkey** binds a
key to it.

### The status card

The card at the top says what NR is doing. While NR works it is green, with three rows:

- **Running at:** where NR runs (Present, After DLSS or Before upscaling).
- **Motion vectors:** "DLSS (the game's)", "DLSS (the game's, copied for Present)", "Launchpad" or "none".
- **Resolution:** the size NR works at, for example "3840x2160 (Full)" or "2560x1440 of 3840x2160 (Quality, Edge-aware)".

A muted "Why: ..." line explains a temporary difference from your choice (for example "After DLSS starts when the game renders with
DLSS."), and the footer shows the API, the passes and NR's video memory. When NR is not working, the card names the first problem, the
fix, and sometimes a button (**Turn on**, **Retry now**, **Clear latch**). The **Details** header lower down lists the engine's own lines:
placement, motion vectors, working size, the API and NR's memory.

### Setup

- **NR stage:** Before upscaling (NR works on the game's render-size image and the game's DLSS upscales the result; experimental),
  After DLSS (on DLSS's output inside the game's frame, before the HUD and post-processing), or Present (on the finished frame, HUD
  included; works in every game).
- **Motion vectors:** Off, DLSS (the game's own DLSS motion vectors), or Launchpad (iMMERSE Launchpad's, on the presented image).
- **Resolution:** Full, Quality (67 %), Balanced (58 %), Performance (50 %), Match game or Custom. Below Full, NR's change is upsampled
  back (Classic or Edge-aware) and costs less GPU time and memory. NR never works below 1280x720.

An option that cannot run in this game right now is greyed out: hover it for the reason ("Available once the game renders with DLSS",
"NR after DLSS needs a Direct3D 12 game", ...). The highlighted option is what actually runs. Your own choice stays saved and comes back
by itself when it can run again.

Below Setup, **Look** sets how NR changes the image (Intensity, Model, Structure, Local tone, Character mask, Pass count), and **Result
shaping**, **Mask and HUD** and **Fixes** refine it. **Compare** switches between your look and the built-in one.

### Off means off

When you switch NR off, the card reads "NR is releasing its memory" and, after the grace period (5 s by default), all of NR's video
memory is released. When NR runs inside the game (64-bit Direct3D 10 to 12, Vulkan and OpenGL), a one-time cost of about 165 to 190 MiB
stays until the game exits, so switching NR on again is instant. A 64-bit Vulkan game keeps more after After DLSS or Before upscaling (see
"Compatibility"). When NR runs in the helper, everything is returned.

### Settings in ReShade.ini

Settings live in the game's `ReShade.ini`, under `[Uplift]`. Most are in the window; these are the ones you may need to set by hand:

| Key | Default | What it does |
|---|---|---|
| `SnippetPath` | empty | **Runtime path** (Advanced): the full path of `nvngx_dlssnr.dll`. Empty looks next to the add-on, then next to the game. Applies from the next game start. |
| `UseD3D9Ex` | `0` | **Use Direct3D 9Ex** (Advanced): `1` asks ReShade for a Direct3D 9Ex device in Direct3D 9 games, so frames reach NR on the GPU. Restart the game. Uplift sets it back to `0` when a start with it never reached the first frame. |
| `NgxHooks` | `0` | **NGX hooks** (Advanced): `1` is safe mode. Nothing is hooked, and NR runs on the presented image only. Applies from the next game start. |
| `AdjustVulkanDevices` | `1` | Hidden. `0` leaves a Vulkan game's device exactly as the game asked: NR then runs CPU-ordered (the game's thread waits for NR each frame). Uplift sets `0` by itself when the last start with `1` never reached its first frames. Applies from the next game start. |
| `GraceSeconds` | `5` | **Grace (seconds)** (Advanced): how long NR keeps its memory after you switch it off, so a quick toggle does not reload it. |

Other Advanced settings: **VRAM margin**, **Resume when VRAM frees up**, **Show the NVIDIA indicator**, **Log level**, **When another NR
producer runs** (Yield stands down, Observe keeps running), **Retry automatically after a failure** and **Present with frame generation**.

## Compatibility

### Feature tiers

| | DX12 | DX11 | DX10 | DX9 | Vulkan | OpenGL |
|---|---|---|---|---|---|---|
| **64-bit** | **Present · DLSS · FG** | Present | Present | Present | **Present · DLSS · FG** ¹ | Present |
| **32-bit** | Present ² | Present ³ | Present | Present | Present ⁴ | Present |

| Tier | NR stage | Motion vectors | Frame generation | Match game resolution |
|---|---|---|---|---|
| **Present · DLSS · FG** | Present, After DLSS, Before upscaling | DLSS (the game's), Launchpad, Off | Detected: after DLSS, NR runs before frame generation; at Present it skips the generated frames unless **Present with frame generation** is on | Yes |
| **Present** | Present | Launchpad, Off | Not detected: NR processes every presented frame | No |

Every tier has the whole look stage, the NR mask, the `Uplift` technique, multiple passes, the working resolutions (Full, Quality,
Balanced, Performance, Custom; Classic or Edge-aware upsampling) and the 1280x720 minimum.

1. 64-bit Vulkan: After DLSS and Before upscaling are explicit choices (Auto stays at Present), because NVIDIA's driver keeps about 0.5
   to 0.6 GB per device after their first use. DLSS's motion vectors also reach NR at Present. Frame generation is handled as on Direct3D
   12, but no Vulkan game with DLSS frame generation has been tried yet.
2. 32-bit Direct3D 12 is dgVoodoo2's Direct3D 12 output.
3. 32-bit Direct3D 11 includes dgVoodoo2's Direct3D 11 output.
4. 32-bit Vulkan includes Direct3D 9 games under DXVK.

### Where NR runs, and the video memory that stays after NR is off

| | DX12 | DX11 | DX10 | DX9 | Vulkan | OpenGL |
|---|---|---|---|---|---|---|
| **64-bit** | the game's own device · about 165 MiB | in the game · about 180 MiB | in the game · about 190 MiB | helper · **0** | in the game · about 180 MiB (After DLSS or Before upscaling: about 0.5 to 0.6 GB) | in the game · about 180 MiB |
| **32-bit** | helper · **0** | helper · **0** | helper · **0** | helper · **0** | helper · **0** | helper · **0** |

- **In the game:** a private Direct3D 12 device inside the game's process (or, in a 64-bit Direct3D 12 game, the game's own device). What
  stays (NGX's one-time cost and the device) stays until the game exits, so switching NR on again is instant.
- **Helper:** `gitc-uplift-helper64.exe`, which exits a few seconds after NR turns off and returns all of NR's memory.
- **Per-frame cost:** plain Direct3D 9 copies each frame through the CPU (about 3 ms at 1080p, about 0.4 ms with **Use Direct3D 9Ex**).
  Direct3D 10 makes four copies a frame through a small relay device. Vulkan and OpenGL fall back to CPU-ordered frames on a device or
  driver without the GPU sync extensions.

### Tested with

- Onimusha: Way of the Sword (Direct3D 12)
- Final Fantasy XV (Direct3D 11)
- No Man's Sky (Vulkan, at Present and After DLSS)
- Lightning Returns: Final Fantasy XIII (32-bit Direct3D 9, and Direct3D 11 through dgVoodoo2)

## Troubleshooting

Uplift's lines in `ReShade.log` start with `[Uplift]`. ReShade writes the log next to its `ReShade.ini`, normally in the game's
executable folder; its first line names the ReShade version that loaded. Set **Log level** (Advanced) to Debug for more detail.

| The card says | What to do |
|---|---|
| "Uplift's 64-bit helper stopped": "gitc-uplift-helper64.exe was not found next to gitc-uplift.addon32" (or `.addon64`) | Copy `gitc-uplift-helper64.exe` next to the add-on, then press **Retry now**. |
| "Uplift's 64-bit helper stopped": "gitc-uplift-helper64.exe (build X) does not match the Uplift add-on next to it (build Y)" | Copy the add-on and the helper from the same release, then press **Retry now**. |
| "The NR runtime was not found" | Copy `nvngx_dlssnr.dll` next to the add-on or the game, or set **Runtime path** (Advanced), and restart the game. |
| "NR failed", with an NVIDIA error code (for example "NR feature creation failed: 0x..." or "NR runtime failed to load: 0x...") | NVIDIA's runtime refused to start. It only enables itself on GeForce RTX 50-series GPUs. Otherwise, check that the driver is current and that `nvngx_dlssnr.dll` is version 310.8. |
| "Waiting for a usable frame": "NR needs at least 1280x720." | NR needs the game's back buffer (the display or window size, not the 3D render size) to be at least 1280x720. Raise the game's resolution or window size. |
| "The Vulkan swap chain cannot take NR's result: this ReShade did not let Uplift add copy access ..." | Update ReShade's Vulkan layer with the 6.8 installer (**Update ReShade only**) and restart the game. |
| "The Vulkan swap chain cannot take NR's result: the game asks for exclusive fullscreen ..." | Use borderless or windowed mode. |
| "Multisampled back buffers need the Uplift technique on Direct3D 9" (or OpenGL) | Enable the `Uplift` technique from `Uplift.fx`: NR then runs on ReShade's resolved copy. |
| "Multisampled back buffers are not supported on ..." | Turn off MSAA in the game. |
| "Another NR tool is running" (for example "RenoDX DLSS5 is running NR in this game") | Remove the other DLSS-NR add-on (`renodx-dlss*.addon64`) from the add-on folder and restart the game. |
| "Not enough address space in this 32-bit game for the frame copy ..." | Apply a 4 GB (large-address-aware) patch to the game's executable. |
| "Not enough video memory for NR" or "Paused: the game needs video memory" | Lower **Resolution** or **Pass count**, or close other GPU programs. |
| "NR after DLSS is off" (after a device removal) | NR stays on the presented image. **Clear latch** allows NR after DLSS again from the next start. |

At the end of a 64-bit game's `ReShade.log`, the line `Add-on "GITC Uplift" was not unregistered!` is expected: Uplift keeps itself
loaded for the life of the game.

### Reporting a problem

Open an issue at [github.com/ghostinthecamera/GITC-Uplift/issues](https://github.com/ghostinthecamera/GITC-Uplift/issues) with:

- the whole `ReShade.log` from a session that shows the problem;
- the card's text and the **Details** lines from the GITC Uplift tab;
- your GPU and NVIDIA driver version;
- your ReShade version (and whether it is the Vulkan layer);
- the game, its API and bitness (the Details lines name them), and what you expected to see.

## Building from source

### Prerequisites

- **Visual Studio 2026** (version 18) or **Visual Studio 2022** (17.14), any edition, with the **Desktop development with C++**
  workload. It brings MSVC, the Windows SDK, and the CMake (3.25 or newer) and Ninja that the build uses. `build.cmd` finds the newest
  Visual Studio installation with the C++ tools.
- **The Windows SDK** (10.0.26100 was used): its `dxc.exe` compiles the Direct3D shaders.
- **The Vulkan SDK** (1.4.335.0 was used), with `VULKAN_SDK` set (its installer sets it): both halves compile against its headers, and
  its `dxc.exe` compiles the SPIR-V shaders. Nothing from it is linked or shipped.
- **Git**, and an internet connection for the first build.

### Build

From a command prompt (in PowerShell, type `.\build.cmd` and `.\tools\fetch_deps.cmd`):

```bat
git clone https://github.com/ghostinthecamera/GITC-Uplift.git
cd GITC-Uplift
tools\fetch_deps.cmd
build.cmd release
build.cmd release-x86
```

- `tools\fetch_deps.cmd` fetches the build's dependencies into `third_party\_deps`: the NVIDIA NGX SDK headers (headers only, no NVIDIA
  binary), the ReShade 6.0.0 add-on API headers, the matching Dear ImGui header, and MinHook. `build.cmd` also runs it, so this step only
  matters the first time.
- `build.cmd release` makes `build\release\gitc-uplift.addon64` and `build\release\gitc-uplift-helper64.exe`.
- `build.cmd release-x86` makes `build\release-x86\gitc-uplift.addon32`.
- `build.cmd debug` and `build.cmd debug-x86` make debug builds.

Each build carries a build id, `1.0.0+<commit>` in a git checkout or `1.0.0+source` in a source archive. The add-on and the helper refuse
each other when their build ids differ, so always use the two from one tree.

## Licence

GITC Uplift is released under the MIT licence: see [LICENSE](LICENSE). The licences of the code it derives from or builds against are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation.
