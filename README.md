# GITC Uplift

**DLSS Neural Rendering through ReShade, built for performance and ease of use.**

Drop it in, switch it on, and tune the look to suit your gameplay or shot.

Supports DirectX 9–12, Vulkan and OpenGL, in both 64-bit and 32-bit games.

**[Download](https://github.com/ghostinthecamera/GITC-Uplift/releases/latest)** · [Illustrated guide](https://github.com/ghostinthecamera/GITC-Uplift/wiki/Guide) · [Get help](https://github.com/ghostinthecamera/GITC-Uplift/issues)

Plug of my website and camera mods (which this mod works great with)! Take shots and much more with these:  
Documentation: https://ghostinthecamera.co.uk/  
Mods: https://www.patreon.com/ghostinthecamera  

## What you need

- An **NVIDIA GeForce RTX** graphics card with up-to-date drivers.
- [ReShade](https://reshade.me) **with full add-on support**. Vulkan and OpenGL require version 6.8 or newer.
- **`nvngx_dlssnr.dll`** compatible with your graphics card, supplied separately. NVIDIA files are not included.

Use in **single-player games only**, and remove other DLSS-NR add-ons before installing.

## Install

1. Download and extract the latest release.
2. Copy everything inside **`Addons`** into your ReShade add-on folder.
3. Put **`nvngx_dlssnr.dll`** inside the copied **`GITC-Uplift`** subfolder.
4. Copy the contents of **`Shaders`** into your ReShade shader folder.

Your add-on folder is usually the game's folder, unless you changed `AddonPath` in `ReShade.ini`. Keep the NVIDIA DLL inside `GITC-Uplift`, not beside the game's executable.

**Updating?** Replace the existing files with the new release. If you have the older files named `uplift.addon64`, `uplift.addon32` or `uplift-helper64.exe`, delete those too.

## Switch it on

1. Start the game and press **Home** to open ReShade.
2. Open **GITC Uplift** and tick **Enable**.

That's it.

Adjust **Look** to taste. Enable **Keep faces** to help preserve facial detail, or lower **Resolution** for better performance and lower memory use.

The status card tells you whether Uplift is working and what to do if something needs attention. For more control, see the [illustrated guide](https://github.com/ghostinthecamera/GITC-Uplift/wiki/Guide).

## What works where

| | DX12 | DX11 | DX10 | DX9 | Vulkan | OpenGL |
|---|---|---|---|---|---|---|
| **64-bit** | ✓ + DLSS | ✓ + DLSS | ✓ | ✓ | ✓ + DLSS | ✓ |
| **32-bit** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |

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
| "NR skipped: needs 1.2 GB, 0.6 GB free" | The game is using most of the video memory. Lower **VRAM margin**, or set **Video memory check** to **Relaxed** or **Off** (both in Advanced). |
| A Vulkan game crashes while starting, with Uplift | Start it again: Uplift turns off its additions to the game's Vulkan device by itself. If it keeps crashing, set **Vulkan NR at Present** (Advanced) to **Helper**. |
| NR costs a lot at a very low resolution | Set **Pass count** to 1. Below about 720p, NR's cost hardly shrinks with the image, so **Resolution** helps little there. |
| At **Present** with **Motion vectors** on DLSS, the image shimmers when the camera moves | The game keeps its images upside down (many Unity games do). Set `RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN` to 1 in ReShade's global preprocessor definitions, as for depth effects: Uplift then turns DLSS's motion vectors the right way up. |
| With **Keep faces** on, characters get specks or glowing spots | Tick **Show the face mask** (Advanced) and tune it: see [the wiki](https://github.com/ghostinthecamera/GITC-Uplift/wiki/Guide#keep-faces). |
| "Another NR tool is running" | Remove other DLSS-NR add-ons and restart the game. |
| "The Vulkan swap chain cannot take NR's result" | Update ReShade to 6.8 (choose **Update ReShade only** in the installer). If it mentions exclusive fullscreen, use borderless or windowed mode. |
| "Not enough address space in this 32-bit game" | Apply a 4 GB (large-address-aware) patch to the game. |

Still stuck? [Open an issue](https://github.com/ghostinthecamera/GITC-Uplift/issues) and include:

- `ReShade.log` (it's next to `ReShade.ini`, usually in the game's folder);
- what the card says, and its **Details** lines;
- your graphics card, driver and ReShade version;
- the game.

---

[MIT licence](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md) · [Contributing](CONTRIBUTING.md)

GITC Uplift is not affiliated with NVIDIA or ReShade.
