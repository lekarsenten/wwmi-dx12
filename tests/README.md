# Regression checks

On Windows with Visual Studio's Desktop development with C++ workload and a
Windows SDK installed, first build the Release x64 solution (with
`/p:WwmiDeploy=false` to avoid deployment), then run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run.ps1
```

The script locates MSVC with `vswhere`, compiles with `/W4 /WX`, and places its
output under `build_addon/tests`. No game installation or third-party test
framework is required. It returns a failure if WARP is unavailable. The parser
checks link the Release addon objects and engine library to exercise the real
XXMI parser. INI fixtures and any generated files stay under `build_addon/tests`.

The checks cover:

- Parsed shader operands anywhere in compound conditions, including the right
  side of a comparison, compact `!=`, functions, and shader member access.
- Variables, namespaced resources, and stage resource bindings that must not be
  mistaken for bare shader operands.
- Nested `if`/`elif`, pre/post command lists, observed command wrappers, linked
  lists and cycles, plus six full INI sections loaded by the engine.
- Fence completion, timeout, event-registration failure, device removal before
  and after waiting, and stale event notifications.
- Real D3D12 WARP command submission, allocator reuse after fence completion,
  and event reuse after a timed-out registration.

These checks do not reproduce Wuthering Waves crashes or validate specific mods,
frame generation, or behavior on hardware GPU drivers. The PresentPass failure
branches and in-game stopped notice still need in-game validation.
