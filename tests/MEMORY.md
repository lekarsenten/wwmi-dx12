# Resource memory diagnostics

Build the Release x64 solution with `WwmiDeploy=false`, then run from a Windows
PowerShell prompt with Visual Studio's Desktop development with C++ workload:

```powershell
msbuild wwmi-dx12.sln /p:Configuration=Release /p:Platform=x64 /p:WwmiDeploy=false /m
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run-memory.ps1
```

The runner locates Visual Studio and links the addon/engine objects.
It checks concurrent accounting, unknown allocation sizes, WARP-backed buffer
and texture creation, GPU-delayed retirement, immediate and deferred readback
release, and the start/end/peak values in a generated 10-second report.
Generated executables, logs and reports stay under ignored `build_addon/tests`.

The counters cover resources created by the DX11-to-DX12 scripting device.
At creation, we call `GetResourceAllocationInfo` for the allocation size,
including alignment, mip levels and array slices. Residency can change while
these totals stay the same. We count default, upload and readback heaps;
staging textures use DX12 buffers and appear in the buffer rows. Failed size
queries have a separate count. Game resources, shared upload/staging pools,
CPU copies, descriptors and other addon caches fall outside these counters.

We update the counters under a mutex when creating, retiring or releasing a
resource. Each sample copies four rows. Reports sample once a second and at
start/end, so brief spikes can fall between samples. Each column has its own
peak for the report period.

For issue #12, collect reports with the same scene, mods and graphics settings,
first with frame generation off, then at the affected multiplier, and record
the multiplier and elapsed play time. Compare process/video memory with the
active and retired rows. A growing retired row shows resources accumulating
pending release. The MFG crash still needs an in-game reproduction.
