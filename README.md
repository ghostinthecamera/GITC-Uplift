# GITC Uplift

GITC Uplift is a virtual photography focused version of the DLSS 5 mod: NVIDIA's DLSS Neural Rendering (DLSS-NR) through ReShade,
with greater flexibility, ease of use, stability and hassle-free compatibility across APIs (DirectX 9, 10, 11 and 12, Vulkan and
OpenGL, 64-bit and 32-bit). Drop it in, switch it on, and it works out the rest. Switch it off and it gives its video memory back.

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
2) Copy everything in the release's `Addons` folder (the files and the `GITC-Uplift` folder) to where your ReShade add-ons are.
3) Copy the `nvngx_dlssnr.dll` file into that `GITC-Uplift` folder!
4) Copy the files in the release's `Shaders` folder to your ReShade `Shaders` folder.

Done!

- Updating from an earlier Uplift build? Delete its `uplift.addon64`, `uplift.addon32` and `uplift-helper64.exe` first, so that two
  copies don't load. Copy the new `Uplift.fx` over the old one too.
- ReShade's add-on folder is the game's folder, unless `AddonPath` in the game's `ReShade.ini` points somewhere else.

## Using it

1) Start the game and open ReShade (the Home key).
2) Go to the **GITC Uplift** tab and tick **Enable**.

That's it. The card at the top always tells you what NR is doing right now. It's green when NR is working; when it isn't, the card
says why and what to do.

A picture guide to every setting: [the wiki](https://github.com/ghostinthecamera/GITC-Uplift/wiki/Guide)

**Setup** has three choices:

- **NR stage:** **Present** works in every game and processes the finished image. **After DLSS** (in games that use DLSS) runs inside
  the game's frame, before the HUD. **Before upscaling** is experimental.
- **Motion vectors:** Auto picks for you. The others are DLSS (the game's own), Launchpad (iMMERSE Launchpad's), Lumenite
  (LumeniteFX's Kernel) and None.
- **Resolution:** Full looks best. Lower settings are faster and use less memory.

An option that can't work in your game right now is greyed out: hover over it to see why. Your choice is remembered and comes back by
itself when it can.

In most games, if you turn DLSS off in the game, NR moves to Present by itself and comes back when DLSS is on again.

The **Look** section below lets you tune the result to taste. When you switch NR off, it frees its memory after a few seconds.

**Keep faces** (in Look) keeps NR's lighting and shading on characters, but stops NR reshaping their faces. It costs more:
NR runs one more time per frame, and it needs more video memory (about 1 GB at 4K). How to tune it:
[the wiki](https://github.com/ghostinthecamera/GITC-Uplift/wiki/Guide#keep-faces).

The shaders are optional:

- **`Uplift.fx`** adds an `Uplift` technique: drag it in ReShade's effect list to choose where NR runs among your effects. Without it,
  NR runs before all of them. Launchpad's and Lumenite's motion vectors need it:
  - **Launchpad:** turn on Launchpad and put Uplift below it.
  - **Lumenite:** turn on **LUMENITE: Kernel 2.0** and put Uplift below it. Lumenite needs the `Uplift.fx` from Uplift 1.2.0 or newer,
    and doesn't work in DX9 games.
  - Then set **Motion vectors** to Auto, or pick Launchpad or Lumenite. With both turned on, Auto uses Launchpad.
- **`UpliftMask.fx`** is an example mask that limits NR to parts of the image.

## What works where

| | DX12 | DX11 | DX10 | DX9 | Vulkan | OpenGL |
|---|---|---|---|---|---|---|
| **64-bit** | ✓ + DLSS | ✓ + DLSS | ✓ | ✓ | ✓ + DLSS | ✓ |
| **32-bit** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

- **✓** NR runs on the finished image, with everything in the Uplift tab.
- **+ DLSS** In games that use DLSS, NR can also run right after DLSS inside the game's frame and use the game's own motion vectors. On
  DX12 and Vulkan it also works with frame generation. On Vulkan, pick After DLSS yourself: Auto stays at Present (see "If you know what
  you are doing").
- DX11 games with DLSS added by a mod work too. If the mod runs DLSS on a DirectX 12 device of its own, NR stays at Present, and the
  Uplift tab says so.
- **64-bit Vulkan:** NR runs on the game's own Vulkan device. If it can't there, Uplift moves NR to a private DX12 device, and if that
  can't either, to its helper process, by itself; **Details** says where and why. **Vulkan NR at Present** (Advanced) picks one yourself.
  Only the game's own device (**Native**) runs NR inside the game's DLSS. Pick Direct3D 12 or Helper and NR moves to Present, with After
  DLSS and Before upscaling greyed out. If native NR fails only at Present, After DLSS and Before upscaling stay available.
- DXVK games count as Vulkan. dgVoodoo2 games count as 32-bit DX11 or DX12.

**Tested with:** Onimusha: Way of the Sword (DX12), Final Fantasy XV (DX11), No Man's Sky (Vulkan), Lightning Returns: Final Fantasy
XIII (32-bit DX9, and DX11 through dgVoodoo2).

## If something's wrong

The card at the top of the Uplift tab tells you what's wrong. The usual ones:

| The card says | What to do |
|---|---|
| "The NR runtime was not found" | Put `nvngx_dlssnr.dll` into the `GITC-Uplift` folder next to the add-ons and restart the game. |
| "Move nvngx_dlssnr.dll" | Move `nvngx_dlssnr.dll` away from the game's .exe into the `GITC-Uplift` folder next to the add-ons, and restart the game. |
| "NR failed", with an NVIDIA error code | Your `nvngx_dlssnr.dll` doesn't work with your card (see "What you need"), or your driver is out of date. |
| "Uplift's 64-bit helper stopped" | Copy `gitc-uplift-helper64.exe` from the same release next to the add-ons, then press **Retry now**. |
| "The GPU device was removed" | Press **Retry now** if the card has it; otherwise restart the game. After a stop, some video memory (1 GB or more at 4K) stays in use until you restart the game. If it happens twice, restart the game, and please report it with `ReShade.log`. |
| "NR needs at least 640x360" | Raise the game's resolution or window size. |
| A Vulkan game crashes while starting, with Uplift | Start it again: Uplift turns off its additions to the game's Vulkan device by itself. If it keeps crashing, set **Vulkan NR at Present** (Advanced) to **Helper**. |
| NR costs a lot at a very low resolution | Set **Pass count** to 1. Below about 720p, NR's cost hardly shrinks with the image, so **Resolution** helps little there. |
| At **Present** with **Motion vectors** on DLSS, the image shimmers when the camera moves | The game keeps its images upside down (many Unity games do). Set `RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN` to 1 in ReShade's global preprocessor definitions, as for depth effects: Uplift then turns DLSS's motion vectors the right way up. |
| With **Keep faces** on, characters get specks or glowing spots | Tick **Show the face mask** (Advanced) and tune it: see [the wiki](https://github.com/ghostinthecamera/GITC-Uplift/wiki/Guide#keep-faces). |
| "Another NR tool is running" | Remove the other DLSS5 add-on (see "Other DLSS5 add-ons") and restart the game. |
| "The Vulkan swap chain cannot take NR's result" | Update ReShade to 6.8 (choose **Update ReShade only** in the installer). If it mentions exclusive fullscreen, use borderless or windowed mode. |
| "Not enough address space in this 32-bit game" | Apply a 4 GB (large-address-aware) patch to the game. |

Still stuck? [Open an issue](https://github.com/ghostinthecamera/GITC-Uplift/issues) and include:

- `ReShade.log` (it's next to `ReShade.ini`, usually in the game's folder);
- what the card says, and its **Details** lines;
- your graphics card, driver and ReShade version;
- the game.

## Known limitations

- If NVIDIA's NR crashes on the GPU, the game can crash too (as with any GPU crash). Restart the game, and please report it with `ReShade.log`.

## If you know what you are doing

A release contains `gitc-uplift.addon64`, `gitc-uplift.addon32`, `gitc-uplift-helper64.exe`, `Uplift.fx` and `UpliftMask.fx`. Which files a
game needs depends on its API and bitness, so you do not need all the files all the time, should you want to be precise for whatever
reason.

| Game | Files in ReShade's add-on folder |
|---|---|
| 64-bit Direct3D 10, 11 or 12, Vulkan, OpenGL | `gitc-uplift.addon64`, and `GITC-Uplift\nvngx_dlssnr.dll` |
| 64-bit Direct3D 9 | `gitc-uplift.addon64`, `gitc-uplift-helper64.exe`, and `GITC-Uplift\nvngx_dlssnr.dll` |
| Any 32-bit game | `gitc-uplift.addon32`, `gitc-uplift-helper64.exe`, and the (64-bit) `GITC-Uplift\nvngx_dlssnr.dll` |

- `nvngx_dlssnr.dll` can also sit right next to the add-ons, or anywhere you point **Runtime path** (Advanced) at. Not next to the game's .exe: on
  RTX 50 cards NVIDIA's DLSS loads it from there first, and Uplift can't use that copy.
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
- **64-bit Vulkan games** run NR on the game's own Vulkan device, at Present and at the DLSS stages. NVIDIA's driver keeps about 0.5 to
  0.6 GB with the game after the first use, until the game exits. Where that can't run, NR moves to a private DX12 device inside the game,
  and from there to `gitc-uplift-helper64.exe`, which some programs need (an emulator that reserves most of its own memory, for one).
  Those two run NR at Present only.
- **Vulkan devices:** Uplift adds three sharing extensions to the game's Vulkan device when the game creates it, so the GPU can keep NR
  in step with the game, and, while **Vulkan NR at Present** is Native, the four NVIDIA extensions NR needs on the game's device. If a
  game misbehaves with that, Uplift drops NVIDIA's extensions by itself, then the rest (`AdjustVulkanDevicesForNgx`, `AdjustVulkanDevices`,
  below). NR then still works, a little slower.
- **Multisampled images (MSAA):** DX9 and OpenGL need the `Uplift` technique turned on. DX10 and 32-bit DX12 can't use MSAA with NR:
  turn it off in the game.

### Settings in ReShade.ini

Settings live in the game's `ReShade.ini`, under `[Uplift]`. Most are in the Uplift tab; these are the ones you may need by hand:

| Key | Default | What it does |
|---|---|---|
| `SnippetPath` | empty | **Runtime path** (Advanced): the full path of `nvngx_dlssnr.dll`. Empty looks in the `GITC-Uplift` folder next to the add-on, then next to the add-on, then next to the game. |
| `UseD3D9Ex` | `0` | **Use Direct3D 9Ex** (Advanced): `1` makes DX9 games much faster (see above). A game in exclusive fullscreen then runs as a borderless window. Restart the game. |
| `NgxHooks` | `0` | **NGX hooks** (Advanced): `1` is safe mode: nothing is hooked, and NR runs at Present only. Restart the game. |
| `AdjustVulkanDevices` | `1` | Hidden. `0` leaves a Vulkan game's device alone (see above). Uplift sets it to `0` by itself if a game crashed while starting with it. Restart the game. |
| `VulkanNr` | `0` | **Vulkan NR at Present** (Advanced): `0` Native, on the game's own device; `1` a private DX12 device; `2` Uplift's helper process. Each falls back to the next by itself. Native needs a game restart after you pick it. Only Native runs After DLSS and Before upscaling: `1` and `2` run at Present, and picking one in the tab moves the NR stage to Present. |
| `AdjustVulkanDevicesForNgx` | `1` | Hidden. `0` leaves out NVIDIA's extensions for native Vulkan NR. Uplift sets it to `0` by itself if a game crashed while starting with them. Restart the game. |
| `VulkanNativeNr` | `1` | Hidden. `0` skips native Vulkan NR. Uplift sets it to `0` by itself if native NR crashed a game; set it back to `1`, or press Clear latch, to try again. Restart the game. |
| `AutoExposureMode` | `0` | **Auto exposure** (Fixes, with **Input exposure** Auto): `0` Blend mixes the game's exposure with Uplift's meter; `1` Switch uses the game's own. A broken game exposure always falls back to the meter. |
| `AutoExposureBlend` | `0.8` | **Blend (game → meter)** (Fixes), 0 to 1, shown as 0-100 %: `0` is the game's exposure, `1` the meter's. Lower stays closer to the game's look; higher is more even between scenes. |
| `GraceSeconds` | `5` | **Grace (seconds)** (Advanced): how long NR keeps its memory after you switch it off, so a quick toggle doesn't reload it. |
| `KeepFaces` | `0` | **Keep faces** (Look): `1` keeps NR's lighting and shading on characters, but stops NR reshaping their faces. NR runs one more time per frame, and needs about 1 GB more video memory at 4K. |
| `FaceProtection` | `0` | **Face protection** (Look), 0 to 1: how much of NR's fine structure faces still get. `0` leaves faces as the game drew them; `1` lets NR's structure through. |
| `LightingScale` | `2.5` | **Lighting scale (% of height)** (Look), 0.5 to 10: on characters, NR's change broader than this counts as lighting and is kept. Larger keeps only the broadest lighting; smaller keeps more of NR's change on faces. |
| `ShowFaceMask` | `0` | **Show the face mask** (Advanced): `1` tints what Keep faces protects in magenta, while you tune it. |
| `FaceMaskThreshold` | `1.5` | **Mask threshold (% of brightness)** (Advanced), 0 to 10: raise it if the mask speckles the background or bright glints; lower it if faces are only faintly tinted. |
| `FaceMaskFull` | `6` | **Full protection at (% of brightness)** (Advanced), 1 to 30: lower it if faces are tinted only in patches; raise it if hair or clothes are tinted too. |
| `FaceMaskStrength` | `2` | **Mask strength** (Advanced), 0.5 to 4: raise it if the mask has holes inside faces; lower it if it spreads past them. |
| `FaceMaskSoftness` | `0.5` | **Mask softness (% of height)** (Advanced), 0.1 to 2: raise it if the mask looks blotchy or flickers; lower it if it spreads past faces. |
| `FaceEdgeFalloff` | `0.15` | **Edge falloff (% of height)** (Advanced), 0.05 to 1: lower it if light glows past a character's outline; raise it if outlines show a dark rim. |
| `FaceLightingDensity` | `0.3` | **Lighting density** (Advanced), 0.05 to 1: raise it if glints or bright edges leave glowing spots on characters; lower it if the lighting fades near the edges of faces. |
| `FaceSpeckSize` | `2` | **Remove specks (px)** (Advanced), 0 to 3: bright specks up to about this size (glints, sparkles) keep NR's normal result. Larger sizes also stop protecting tiny face details, such as a far-away eye. `0` turns it off. |
| `FaceFillSkin` | `1` | **Fill skin by colour** (Advanced): `1` grows the mask over nearby skin of the same colour, such as skin in shadow. `0` if clothes or hair of a skin-like colour turn magenta. |
| `FaceFillRadius` | `3` | **Fill radius (% of height)** (Advanced), 0.5 to 10: how far the fill reaches from marked skin. Raise it if broad patches of skin are still uncovered; lower it if the fill spreads onto nearby surfaces. |
| `FaceFillTolerance` | `0.04` | **Colour tolerance** (Advanced), 0.005 to 0.2: raise it if skin in shadow is still uncovered; lower it if clothes or hair turn magenta. |

### More from the card

- **"... does not match the Uplift add-on next to it":** the add-on and `gitc-uplift-helper64.exe` come from different releases. Copy both
  from one.
- **"Not enough video memory for NR"** or **"Paused: the game needs video memory":** lower **Resolution** or **Pass count**, or close
  other GPU programs.
- **"Keep faces paused"**: there isn't enough video memory for its extra NR run. NR keeps going without it, and Keep faces comes back by
  itself when there is room. Lower **Resolution** or **Pass count** to make room.
- **"Keep faces stopped"**: its extra NR run failed, and NR keeps going without it. Untick and tick **Keep faces** to try again.
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
