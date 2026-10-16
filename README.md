<h1 align="center">wwmi-dx12</h1>

<h4 align="center">WWMI mods for Wuthering Waves in DirectX 12</h4>

A [ReShade](https://reshade.me) add-on that runs the [WWMI](https://github.com/SpectrumQT/WWMI-Package)
engine with the game in **DirectX 12**, on the WWMI folder and mods you already have.

## Disclaimers

- **Beta** - Expect rough edges and please report them.
- **Honesty** - An 8-month, hand-shoveled AI slop project. Tested only on a 12th-gen Intel CPU with an
  NVIDIA RTX 40-series GPU; yours is the next test.
- **Paranoia** - Not code-signed, so a picky antivirus may complain. The source is on GitHub.

## What it can do

- **Textures** - texture overrides, also when the game's texture is not the one the mod was made for: a
  lower-resolution streamed copy, the UHD resources, or a texture a client update re-encoded. Each is
  matched to the mod's texture by its lower mips, by how it looks, or by where it is drawn on the
  character (see Texture matches). If a mod's textures are missing, open that character's screen once:
  the match is learned there and kept
- **Meshes** - mesh replacement on merged skeletons; several mods on one mesh (a body mod and a face mod)
  all draw, as on DX11
- **Copies of a character** - two copies of one modded character at once (an idle pose, an ult's clone) each
  keep their own skeleton instead of mixing as on DX11; tested on two characters
- **Shape keys**
- **RabbitFX**
- **Decensor**
- **Hide UID** - your player number hidden at the bottom right, in the Esc menu and in the Friends list
  (window option, off by default)
- **Frame generation** - modded characters and generated frames together
- **Mod logic** - hosts the XXMI fork of 3dmigoto: a mod's ini logic, toggles and keys run as written
- **Same WWMI, same mods** - your WWMI and `Mods` folders as they are, nested folders included, nothing
  to convert
- **Some mods broken on DX11 work here**
- **DirectX 11 untouched** - XXMI works as before; the add-on sleeps whenever the game runs DX11
- **Other ReShade add-ons** - may work alongside; RenoDX is confirmed
- **One-click launcher** and a **one-checkbox debug log** for bug reports
- **Performance report** - what the add-on costs, live or as a 10-second report to send with a slowdown
- **Stops instead of freezing** - if the add-on hits an error, it switches itself off until the game
  restarts and says so, instead of taking the game down

## What it can't do yet

- **Ray tracing** - with it on, the add-on switches itself off and says so
- **Constant buffer regions** - binding part of a buffer as a constant buffer
  (`vs-cb0 = ref ResourceFoo->Region($offset, $size)`, XXSetConstantBuffers1) is skipped and logged once;
  no WWMI mod is known to use it

## Why DirectX 12

- **Frame generation** exists only in DirectX 12.
- **The engine may move on** - leaks hint at Unreal Engine 5 work in the game's beta, though Kuro has
  said a full move is hard. Whether DirectX 11 would survive one is unknown; if not, this is how mods
  survive.
- **Ray tracing** is DirectX 12 only too, and next on the list.

## Installation

`<Win64>` is the game's `Wuthering Waves Game\Client\Binaries\Win64`.

1. Windows 10/11 x64. Have [WWMI](https://github.com/SpectrumQT/WWMI-Package) through [XXMI Launcher](https://github.com/SpectrumQT/XXMI-Launcher) or standalone (a
   folder with `d3dx.ini`, `Core\WWMI\`, `Mods\`), and check your mods in DX11.
2. Recommended: start the game once in DirectX 12 through the official launcher, without ReShade, so it
   compiles its shaders.
3. Install [ReShade](https://reshade.me) 6.8.0 or newer **with full add-on support** (the standard build does not load add-ons) for
   `Client-Win64-Shipping.exe`, DirectX 10/11/12. It stays as `dxgi.dll`; XXMI runs fine with it.
4. Unzip the [latest release](https://github.com/lekarsenten/wwmi-dx12/releases/latest) into `<Win64>`.
   With `[ADDON] AddonPath` set in `ReShade.ini`, `wwmi_dx12.addon64` must be in that folder: the launcher
   copies it there (by hand: copy it yourself). The `wwmi_dx12\` folder can stay in `<Win64>`.
5. Start the game in DirectX 12, either way:
   - **Easy:** double-click `wwmi_dx12_launch.bat`. It finds your WWMI folder (or asks), saves it in
     `wwmi_dx12.ini`, writes the settings mods need into `Engine.ini` and `UserEngine.ini` and starts DX12.
     It shows the add-on and ReShade versions and both folders first; the arrow keys pick the graphics
     files (UHD, HD, SD), any other key starts the game.
   - **By hand:** set `wwmi_dir` in `wwmi_dx12.ini` once, choose DirectX 12 in the game's settings and
     start the game as usual. XXMI Launcher already writes these lines when it starts WWMI; if mod
     textures, or whole characters at a distance, stay vanilla, add them (see What it changes) or run the
     bat once.

To update, unzip over the old files and keep your `wwmi_dx12.ini`.

**Checklist** (Windows may hide `.dll`): `<Win64>` holds `dxgi.dll` (ReShade), `wwmi_dx12.addon64`, the
`wwmi_dx12` folder with `IbVbHash.cso` and `dxc`, and the launcher files. In game, Home opens ReShade
with a `wwmi-dx12` window showing "Active mods" above 0.

## Known Issues

- WWMI's own hotkeys and guides are not available
- Mods load when the game starts; restart the game after adding or changing a mod
- Decensor and RabbitFX mods prepare shaders on first sight (one-time stutter, saved for next launch)

## Mod Installation

1. Extract the mod's archive into the `Mods` folder of your WWMI folder (one folder for DX11 and DX12).
2. Check it in DX11 if it looks wrong here.

## Hotkeys

- `[Home]` - ReShade, with the `wwmi-dx12` window
- `[F6]` - turn the add-on off and on
- Keys a mod defines work as in DX11; WWMI's own (`[F10]`, `[F12]`, hunting mode) do nothing here.

## The wwmi-dx12 window

| Control | |
|---|---|
| Disable addon (F6) | all mod work off, to compare |
| Debug log / level | writes `<Win64>\wwmi_dx12_addon.log`; off by default, saved to `wwmi_dx12.ini` |
| Decensor | saved to `wwmi_dx12.ini` |
| Hide UID | hides your player number (bottom right, Esc menu, Friends list); off by default, saved to `wwmi_dx12.ini` |
| Load mod textures in the background | on (default): no stutter, textures pop in a moment later. Off: textures from the first frame, after a short pause |
| Use look-alike texture matches | on by default (see Texture matches); saved to `wwmi_dx12.ini` |
| Use texture match cache | on by default; off: no texture matches are learned, read or saved (see Texture matches) |
| Mod buffers in video memory | on by default (faster); saved to `wwmi_dx12.ini` |
| Clear caches at next launch | rebuilds saved shader patches and texture matches; try it if mods look wrong after an update |
| Performance monitor | what the add-on costs in processor and graphics card time, live |
| Save a 10-second performance report | writes `<Win64>\wwmi_dx12_perf_<date>_<time>.txt`; send it with a slowdown report |

## Texture matches

The game often draws a texture under a hash no mod lists: a lower-resolution copy at a distance, the UHD
resources' re-encoded textures, or a texture a client update changed. The add-on matches such a texture
to the one the mod was made for:

- **By its mips** - a lower mip of a known texture is the same image, a proven match. Learned the first
  time both load (a character's screen loads them).
- **By its look** - a visual fingerprint of five small patches, compared with a histogram and SSIM
  (structural similarity) against fingerprints taken from the HD textures.
- **By its place** - the HD character gallery was sampled for which texture each character draws where;
  a texture drawn in the same place is matched to it.

Matches are kept in `<Win64>\wwmi_dx12\cache` (`lod_tex_cache.lodcache`, `tex_look.bin`). The package
brings seeds of what was already learned (`wwmi_dx12\cache\seed-<version>.lodcache` and
`seed-<version>.texlook`), taken in at the first start like a shared file.

- **Sharing** - any other `.lodcache` file put in that folder is taken in at the next start, then renamed
  `DISABLED_<date>_<name>`. Only proven matches are taken; where one disagrees with yours, yours stays, or
  the copy is left vanilla rather than risk the wrong texture. To pass yours on, send
  `lod_tex_cache.lodcache` under another name (the receiver already has a file by that name).
- **Turning it off** - untick **Use texture match cache**: nothing is learned, read or saved, and distant
  copies stay vanilla.
- **Look-alike matches** (window option, on by default) - matches by look or by place, which can't be
  proven from the game's data. Untick it if a texture looks wrong: only proven matches are used then, and
  UHD textures mostly stay vanilla.

## Performance

- On par with standard WWMI, a bit better in 1% lows, though not compared extensively.
- The add-on does on the CPU, per modded draw, what WWMI does in DX11's driver hooks, plus each mod's
  ini logic. That is close to what the approach allows: don't expect big gains later.

## Plans

- **Ray tracing**, next.
- **Performance** - less CPU work per modded draw, and video-memory management so mod buffers and
  textures stay resident without pushing the game's own data out.
- **Easier setup** - a streamlined install and a better launcher.
- **Shared texture matches** - an automatically updated seed of learned texture matches, so mods look
  right on first launch. Sharing a file by hand works already (see Texture matches).
- **EFMI-DX12** - XXMI's EFMI mods for Arknights: Endfield in DirectX 12.

## Reporting a problem

1. Tick **Debug log** (or set `debug = 1` in `wwmi_dx12.ini` to log from the start).
2. Make the problem happen, close the game, copy `<Win64>\wwmi_dx12_addon.log` (and
   `wwmi_dx12_crash.log`) **before the next launch**, which starts a new log.
3. For a slowdown, also save a performance report where it happens (**Save a 10-second performance
   report**) and send the `wwmi_dx12_perf_*.txt` file with the logs.
   For memory growth with frame generation, save reports from the same scene and mods with frame
   generation off and at the affected multiplier. Include the multiplier with each report. The mod
   resource counters show active buffers/textures and resources waiting for release.
4. Send them with what went wrong, which mods were on screen, whether they work in DX11, and your GPU:
   a [GitHub issue](https://github.com/lekarsenten/wwmi-dx12/issues), or a DM or comment on the mod's
   GameBanana page. The log names folders on your PC.

## What it changes

- `Engine.ini`, `[SystemSettings]`: `r.TextureStreaming=1`, `r.Streaming.FullyLoadUsedTextures=1`,
  `r.Streaming.Boost=30`, `r.Streaming.MinBoost=30`, `r.Streaming.HLODStrategy=2`, `r.MipMapLODBias=-1`,
  so mod textures match at full resolution. The launcher keeps the file read-only for the first minute
  of a launch (the game rewrites it at start-up), then writable again.
- `Client\Config\UserEngine.ini`, `[ConsoleVariables]`: `r.Kuro.SkeletalMesh.DistanceLODBaseFOV=165`, as
  XXMI Launcher sets it, so characters keep the model mods replace out to the farthest camera.
- In `<Win64>\wwmi_dx12\cache`: learned texture matches and patched shaders.
- In your WWMI folder: only `d3dx_user.ini` (as in DX11). Mods are only read.
- The launcher stops on a `dxgi.dll` that is not ReShade.
- **Uninstall:** delete everything starting with `wwmi_dx12` in `<Win64>` (the `wwmi_dx12` folder holds
  the caches); ReShade if nothing else uses it; optionally the `Engine.ini` and `UserEngine.ini` lines.

## Building

Visual Studio 2022 or later (C++ desktop, Windows SDK). `git clone --recurse-submodules`, open
`wwmi-dx12.sln`, pick Release x64 and build (F6); or
`msbuild wwmi-dx12.sln /p:Configuration=Release /p:Platform=x64`. The add-on's shaders build with it. The
output is `build_addon\wwmi_dx12.addon64`.

`msbuild package.proj` builds the same and packs the release zip into `build_addon\release\`.

Decensor and RabbitFX also need `dxcompiler.dll` and `dxil.dll` at run time, in `<Win64>\wwmi_dx12\dxc\`:
copy them there from `third_party\dxc\bin\x64` (a
[DirectXShaderCompiler release](https://github.com/microsoft/DirectXShaderCompiler/releases), unmodified).

## Resources

- [WWMI](https://github.com/SpectrumQT/WWMI-Package) · [XXMI Launcher](https://github.com/SpectrumQT/XXMI-Launcher) ·
  [WWMI Tools](https://github.com/SpectrumQT/WWMI-Tools)
- [WuWa mods on GameBanana](https://gamebanana.com/games/20357) · [Discord](https://discord.com/invite/agmg)

## Built on

The work wwmi-dx12 is built from and runs:

- Chiri, [Bo3b](https://github.com/bo3b), [DarkStarSword](https://github.com/DarkStarSword) - original 3dmigoto
- [SilentNightSound](https://gamebanana.com/members/2176153) - the 3dmigoto fork, initial WuWa research
- [Nurarihyon](https://gamebanana.com/members/2841781) - XXMI DLL code
- [SpectrumQT](https://gamebanana.com/members/2837527), [SinsOfSeven](https://gamebanana.com/members/2823441) - WWMI
- [caverabbit](https://gamebanana.com/members/2987570) - RabbitFX
- [crosire](https://github.com/crosire) - ReShade and its add-on API

**Mod authors:** [Actice](https://gamebanana.com/members/3279260), [ahuang](https://gamebanana.com/members/2623583), [Ander001](https://gamebanana.com/members/2872805), [Apoc23](https://gamebanana.com/members/2844846), [astrais](https://gamebanana.com/members/3591617), [ganyu520](https://gamebanana.com/members/2503894), [Hua-Hua](https://gamebanana.com/members/5335246), [HuiYoo](https://gamebanana.com/members/4301549), [kongxinrumeng](https://gamebanana.com/members/2437196), [MochiDauXanh](https://gamebanana.com/members/3249732), [monsoon04](https://gamebanana.com/members/2288970), [Morning_Star](https://gamebanana.com/members/2798606), [xucaikui](https://gamebanana.com/members/3720155), [Zlevir](https://gamebanana.com/members/2694449)

Thanks to everyone credited by [WWMI](https://github.com/SpectrumQT/WWMI-Package#credits), whose work this builds on.

## License

GPLv3 (`wwmi_dx12\LICENSE`). Contains 3Dmigoto (XXMI fork) code, GPL-3.0. Third-party notices:
`wwmi_dx12\THIRD-PARTY-NOTICES.md`, licence texts in `wwmi_dx12\licenses`.
