## Quick orientation

This repo combines a Godot GDExtension example with the AMD TressFX sample and the `godot-cpp` C++ bindings.
Key areas:
- `godot-cpp/` — C++ bindings and templates for GDExtensions (must match your target Godot version).
- `src/` — GDExtension entry and glue for this extension (see `src/register_types.cpp`).
- `demo/` — Godot demo project and generated `.gdextension` files in `demo/bin/`.
- `TressFX/` — upstream AMD TressFX sample code and build artifacts (heavy native code; separate build flow).

## Big-picture architecture

- Bindings layer: `godot-cpp/` provides header and compiled library used by the native extension.
- Extension code: `src/` implements the GDExtension entry point and registers classes with Godot's ClassDB (see `src/register_types.cpp`).
- Demo packaging: `demo/bin/*.gdextension` points Godot to the native library and defines the `entry_symbol` (see `godot-cpp/README.md` for `.gdextension` format).
- Native samples: `TressFX/` is a separate native demo (DirectX/Vulkan) and uses its own build tools (`TressFX/build/GenerateSolutions.bat`).

Why it is structured this way: `godot-cpp` mirrors Godot's module/extension lifecycle so native types can be registered similarly to engine modules. `TressFX/` is integrated as a native library/demo and must be treated as an external native dependency.

## Build & developer workflows (concrete)

- Build the gd-extension shared lib (SCons-based): repository root uses an `SConstruct` that references `godot-cpp/SConstruct`. Install SCons and run from repo root:

```powershell
pip install scons
scons -Q
```

- godot-cpp: follow `godot-cpp/README.md` — use the matching branch/tag for your Godot version and run the instructions there (CMake/SCons vary by target platform). On Windows you typically generate Visual Studio solutions or use SCons wrappers.

- TressFX native demo (Windows): inside `TressFX/build` run `GenerateSolutions.bat` then open the generated Visual Studio solution and build `TressFX_DX12.exe` or `TressFX_VK.exe`.

Notes and caveats:
- Ensure `godot-cpp` is checked out at a branch/tag that matches your Godot editor. The API is version sensitive (see `godot-cpp/README.md` warnings).
- The extension entry must export the init function name referenced by `.gdextension` (see `src/register_types.cpp` for `tfx_bridge_init`).

## Project-specific coding patterns to follow

- GDExtension init layout: follow the pattern in `src/register_types.cpp` — an `extern "C"` init function that creates a `godot::GDExtensionBinding::InitObject`, registers initializers/terminators, sets `MODULE_INITIALIZATION_LEVEL_SCENE`, then calls `init()`.
- Register classes with `GDREGISTER_CLASS(...)` inside the initializer and guard by `p_level` (compare `initialize_module` in `src/register_types.cpp`).
- Keep the minimum library initialization level at scene unless a broader lifecycle is required.

## Integration points & external dependencies

- Godot engine — must be the matching version for `godot-cpp` (see `godot-cpp/README.md`).
- Vulkan SDK / DirectX SDK / Visual Studio — required to build `TressFX/` native demos (see `TressFX/README.md`).
- The `.gdextension` file in `demo/bin/` points the engine to the compiled native library and the `entry_symbol` name. When changing the exported symbol, update the `.gdextension` accordingly.

## Files to check when making changes

- `src/register_types.cpp` — GDExtension entry/registration example.
- `Sconstruct` and `godot-cpp/SConstruct` — repo build glue (SCons). Use these when adding source files or changing build layout.
- `demo/bin/*.gdextension` — runtime wiring between Godot and the native lib.
- `godot-cpp/README.md` and `TressFX/README.md` — platform/tooling reference and constraints.

## Examples the agent can use

- To add a new exported class: copy the `GDREGISTER_CLASS(MyClass)` pattern from `src/register_types.cpp` and add the implementation under `src/` so SCons picks it up (`SConstruct` globs `src/*.cpp`).
- To change the entry symbol name: update `extern "C"` function in `src/register_types.cpp` and mirror it in `demo/bin/*.gdextension`.

## Do not assume

- Do not assume `godot-cpp` is compatible with arbitrary Godot versions — always check `godot-cpp/README.md` and branch/tag alignment.
- Do not assume TressFX native builds are integrated into the SCons scripts — they are separate and use their own generator (`TressFX/build/GenerateSolutions.bat`).

## TL;DR for a quick task

- To add a new native type:
  1. Add `.cpp`/`.h` in `src/`.
  2. Register it in `initialize_module` using `GDREGISTER_CLASS(...)`.
  3. Run `scons` at repo root to rebuild the shared library.
  4. Ensure `demo/bin/*.gdextension` `entry_symbol` points to the exported init symbol.

If anything here is unclear or you want me to expand any section (examples, more build commands for Windows vs Linux, or how to tie in CI), tell me which area and I will iterate.
