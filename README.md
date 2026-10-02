# GITC Uplift

GITC Uplift brings NVIDIA's DLSS Neural Rendering (DLSS-NR) to your games through ReShade: DirectX 9, 10, 11 and 12, Vulkan and
OpenGL, 64-bit and 32-bit. Drop it in, switch it on, and it works out the rest. Switch it off and it gives its video memory back.

GITC Uplift is not affiliated with NVIDIA or with ReShade.

## What you need

- An NVIDIA GeForce RTX graphics card with an up-to-date driver.
- [ReShade](https://reshade.me) **with full add-on support** (the "Addon" version). Use the latest one: Vulkan and OpenGL games need
  6.8 or newer.
- NVIDIA's DLSS-NR runtime, `nvngx_dlssnr.dll` version 310.8, which **you supply**. Uplift never includes NVIDIA files. You need a
  version appropriate for your video card, as the default runtime only works on 50-series cards.

## Other DLSS5 add-ons

- **Use GITC Uplift instead of other DLSS-NR add-ons**, for example RenoDX's DLSS and DLSS5 add-ons (`renodx-dlss*.addon64`): remove them
  from ReShade's add-on folder. If one is still loaded, Uplift will not work.

## Before you start

- **Single-player only.** Uplift hooks into the game, and anti-cheat systems may not like that. Don't use it in online games.
- **Antivirus** tools sometimes flag ReShade add-ons. Every release lists the SHA-256 of its files in `SHA256SUMS.txt`, or you can build
  Uplift yourself (see "Building from source").

## Install

Installation is really simple:
1) Download the latest release from [GitHub Releases](https://github.com/ghostinthecamera/GITC-Uplift/releases).
2) Copy all the files in the release's `Addons` folder to where your ReShade add-ons are.
3) Copy the `nvngx_dlssnr.dll` file to the same place as step 2! The dll and the add-ons must be in the same folder.
4) Copy the files in the release's `Shaders` folder to your ReShade `Shaders` folder.

Done!

- Updating from an earlier Uplift build? Delete its `uplift.addon64`, `uplift.addon32` and `uplift-helper64.exe` first, so that two
  copies don't load.
- ReShade's add-on folder is the game's folder, unless `AddonPath` in the game's `ReShade.ini` points somewhere else.

## Using it

1) Start the game and open ReShade (the Home key).
2) Go to the **GITC Uplift** tab and tick **Enable**.

That's it. The card at the top always tells you what NR is doing right now. It's green when NR is working; when it isn't, the card
says why and what to do.

A picture guide to every setting: [docs/GUIDE.md](docs/GUIDE.md)

**Setup** has three choices:

- **NR stage:** **Present** works in every game and processes the finished image. **After DLSS** (in games that use DLSS) runs inside
  the game's frame, before the HUD. **Before upscaling** is experimental.
- **Motion vectors:** Off, DLSS (the game's own) or Launchpad (iMMERSE Launchpad's).
- **Resolution:** Full looks best. Lower settings are faster and use less memory.

An option that can't work in your game right now is greyed out: hover over it to see why. Your choice is remembered and comes back by
itself when it can.

The **Look** section below lets you tune the result to taste. When you switch NR off, it frees its memory after a few seconds.

The shaders are optional:

- **`Uplift.fx`** adds an `Uplift` technique: drag it in ReShade's effect list to choose where NR runs among your effects. Without it,
  NR runs before all of them. Launchpad's motion vectors need it, placed below Launchpad.
- **`UpliftMask.fx`** is an example mask that limits NR to parts of the image.

## What works where

| | DX12 | DX11 | DX10 | DX9 | Vulkan | OpenGL |
|---|---|---|---|---|---|---|
| **64-bit** | ✓ + DLSS | ✓ | ✓ | ✓ | ✓ + DLSS | ✓ |
| **32-bit** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

- **✓** NR runs on the finished image, with everything in the Uplift tab.
- **+ DLSS** In games that use DLSS, NR can also run right after DLSS inside the game's frame, use the game's own motion vectors and
  work with frame generation. On Vulkan, pick After DLSS yourself: Auto stays at Present (see "If you know what you are doing").
- DXVK games count as Vulkan. dgVoodoo2 games count as 32-bit DX11 or DX12.

**Tested with:** Onimusha: Way of the Sword (DX12), Final Fantasy XV (DX11), No Man's Sky (Vulkan), Lightning Returns: Final Fantasy
XIII (32-bit DX9, and DX11 through dgVoodoo2).

## If something's wrong

The card at the top of the Uplift tab tells you what's wrong. The usual ones:

| The card says | What to do |
|---|---|
| "The NR runtime was not found" | Put `nvngx_dlssnr.dll` next to the add-ons and restart the game. |
| "NR failed", with an NVIDIA error code | Your `nvngx_dlssnr.dll` doesn't work with your card (see "What you need"), or your driver is out of date. |
| "Uplift's 64-bit helper stopped" | Copy `gitc-uplift-helper64.exe` from the same release next to the add-ons, then press **Retry now**. |
| "NR needs at least 1280x720" | Raise the game's resolution or window size. |
| "Another NR tool is running" | Remove the other DLSS5 add-on (see "Other DLSS5 add-ons") and restart the game. |
| "The Vulkan swap chain cannot take NR's result" | Update ReShade to 6.8 (choose **Update ReShade only** in the installer). If it mentions exclusive fullscreen, use borderless or windowed mode. |
| "Not enough address space in this 32-bit game" | Apply a 4 GB (large-address-aware) patch to the game. |

Still stuck? [Open an issue](https://github.com/ghostinthecamera/GITC-Uplift/issues) and include:

- `ReShade.log` (it's next to `ReShade.ini`, usually in the game's folder);
- what the card says, and its **Details** lines;
- your graphics card, driver and ReShade version;
- the game.

## If you know what you are doing

A release contains `gitc-uplift.addon64`, `gitc-uplift.addon32`, `gitc-uplift-helper64.exe`, `Uplift.fx` and `UpliftMask.fx`. Which files a
game needs depends on its API and bitness, so you do not need all the files all the time, should you want to be precise for whatever
reason.

| Game | Files in ReShade's add-on folder |
|---|---|
| 64-bit Direct3D 10, 11 or 12, Vulkan, OpenGL | `gitc-uplift.addon64` and `nvngx_dlssnr.dll` |
| 64-bit Direct3D 9 | `gitc-uplift.addon64`, `gitc-uplift-helper64.exe` and `nvngx_dlssnr.dll` |
| Any 32-bit game | `gitc-uplift.addon32`, `gitc-uplift-helper64.exe` and the (64-bit) `nvngx_dlssnr.dll` |

- `nvngx_dlssnr.dll` can also sit next to the game's executable, or anywhere you point **Runtime path** (Advanced) at.
- The add-ons and `gitc-uplift-helper64.exe` must come from the same release.

### ReShade for each kind of game

- **DX10, 11, 12:** a normal ReShade install.
- **DX9:** ReShade as `d3d9.dll`. NR copies each frame through memory, about 3 ms at 1080p. **Use Direct3D 9Ex** (Advanced, then
  restart the game) brings that down to about 0.4 ms. If the game then won't start, start it again: Uplift turns the option back off.
- **Vulkan (DXVK included):** ReShade 6.8's Vulkan layer. Run the installer, pick the game's executable and choose **Vulkan**. If it
  finds a `ReShade.ini`, choose **Update ReShade only** (Uninstall deletes your `ReShade.ini`). A game running through DXVK must use the
  Vulkan layer, not a `d3d9.dll` or `dxgi.dll` ReShade.
- **OpenGL:** ReShade 6.8 as `opengl32.dll`, in the game's bitness.
- **Old DX8 and DX9 games through dgVoodoo2:** faster than plain DX9. Put dgVoodoo2's `MS\x86\D3D9.dll` (or `D3D8.dll`) next to the game,
  set **Output API** in `dgVoodooCpl.exe` to Direct3D 11 or "Direct3D 12 (feature level 12.0)", and install the 32-bit ReShade as
  `dxgi.dll`. Other `d3d9.dll` wrappers (DXVK, game fixes) conflict with it.

### Where NR runs, and memory

- **64-bit games** run NR inside the game. After the first time you switch it on, about 165 to 190 MB stays with the game until it
  exits, so switching NR on again is instant.
- **32-bit and DX9 games** run NR in `gitc-uplift-helper64.exe`, a small background process that Uplift starts when NR turns on. It
  exits a few seconds after NR turns off and gives all of its memory back.
- **Vulkan, After DLSS and Before upscaling:** NVIDIA's driver keeps about 0.5 to 0.6 GB per game after the first use, until the game
  exits. That's why Auto stays at Present on Vulkan.
- **Vulkan devices:** Uplift adds three sharing extensions to the game's Vulkan device when the game creates it, so the GPU can keep NR
  in step with the game. If a game misbehaves with that, set `AdjustVulkanDevices=0` (below). NR then still works, a little slower.
- **Multisampled images (MSAA):** DX9 and OpenGL need the `Uplift` technique turned on. DX10 and 32-bit DX12 can't use MSAA with NR:
  turn it off in the game.

### Settings in ReShade.ini

Settings live in the game's `ReShade.ini`, under `[Uplift]`. Most are in the Uplift tab; these are the ones you may need by hand:

| Key | Default | What it does |
|---|---|---|
| `SnippetPath` | empty | **Runtime path** (Advanced): the full path of `nvngx_dlssnr.dll`. Empty looks next to the add-on, then next to the game. |
| `UseD3D9Ex` | `0` | **Use Direct3D 9Ex** (Advanced): `1` makes DX9 games much faster (see above). Restart the game. |
| `NgxHooks` | `0` | **NGX hooks** (Advanced): `1` is safe mode: nothing is hooked, and NR runs at Present only. Restart the game. |
| `AdjustVulkanDevices` | `1` | Hidden. `0` leaves a Vulkan game's device alone (see above). Uplift sets it to `0` by itself if a game crashed while starting with it. Restart the game. |
| `GraceSeconds` | `5` | **Grace (seconds)** (Advanced): how long NR keeps its memory after you switch it off, so a quick toggle doesn't reload it. |

### More from the card

- **"... does not match the Uplift add-on next to it":** the add-on and `gitc-uplift-helper64.exe` come from different releases. Copy both
  from one.
- **"Not enough video memory for NR"** or **"Paused: the game needs video memory":** lower **Resolution** or **Pass count**, or close
  other GPU programs.
- **"NR after DLSS is off"** (after the GPU crashed): NR stays at Present. **Clear latch** allows After DLSS again from the next start.
- Uplift's lines in `ReShade.log` start with `[Uplift]`; set **Log level** (Advanced) to Debug for more. The line
  `Add-on "GITC Uplift" was not unregistered!` at the end of a 64-bit game's log is normal.

## Building from source

You need:

- **Visual Studio 2026** or **2022** (17.14), any edition, with the **Desktop development with C++** workload (it includes CMake, Ninja
  and the Windows SDK, whose `dxc.exe` compiles the Direct3D shaders);
- the **Vulkan SDK** (it sets `VULKAN_SDK`), for its headers and the `dxc.exe` that compiles the SPIR-V shaders;
- **Git**, and an internet connection for the first build.

Then, from a command prompt (in PowerShell, type `.\build.cmd` and `.\tools\fetch_deps.cmd`):

```bat
git clone https://github.com/ghostinthecamera/GITC-Uplift.git
cd GITC-Uplift
tools\fetch_deps.cmd
build.cmd release
build.cmd release-x86
```

- `tools\fetch_deps.cmd` downloads the headers the build needs into `third_party\_deps` (NVIDIA's NGX SDK headers, no NVIDIA binaries;
  the ReShade 6.0.0 add-on headers and Dear ImGui; MinHook).
- `build.cmd release` makes `build\release\gitc-uplift.addon64` and `build\release\gitc-uplift-helper64.exe`.
- `build.cmd release-x86` makes `build\release-x86\gitc-uplift.addon32`.
- The add-ons and the helper check each other's build id, so always use the ones from the same build.

## Licence

GITC Uplift is released under the MIT licence: see [LICENSE](LICENSE). The licences of the code it derives from or builds against are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation.
