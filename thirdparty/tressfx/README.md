# thirdparty/tressfx — vendored AMD TressFX 4.1 reference subset

This folder is the **only** place AMD TressFX code should ever be read from in this
repository. The full upstream checkout in `TressFX/` (668 MB, gitignored) is **not
needed anymore** — not for building, not for reference — and may be deleted or moved.

**Instructions for AI agents (Claude Code): never read, grep, or glob inside `TressFX/`.
It saturates context. Everything needed is here (572 KB) and in `NOTES.md` at the repo
root, which already distills the conventions, formats, and parameters.**

## Contents

- `src/TressFX/` — TressFX 4.1 core (`.cpp`/`.h`). Ten files are compiled into the
  C++ reference GDExtension (see the explicit list in the root `Sconstruct`); the rest
  are kept read-only for reference (SDF/PPLL/ShortCut/BoneSkinning families).
- `src/Math/` — AMD's math types, compiled (TressFXAsset depends on them).
- `src/Shaders/` — **all 13 original HLSL shaders**, reference only, never compiled:
  - `TressFXSimulation.hlsl` — audit reference for our GLSL kernels; contains the
    un-ported capsule-collision block (Phase 5)
  - `TressFXRendering.hlsl`, `TressFXLighting.hlsl`, `TressFXStrands.hlsl` — shading
    reference for Phases 4/6
  - `TressFXBoneSkinning.hlsl` — skinning convention reference
  - `TressFXSDFCollision.hlsl`, `TressFXMarchingCubes.hlsl` — SDF collision (deferred)
  - `TressFXPPLL.hlsl`, `TressFXShortCut.hlsl` — OIT paths (deferred luxuries)
  - others (`FullScreenRender`, `TressFXShadow`, `TressFXUtilities`, ...)
- `src/EngineInterface.h`, `src/MarchingCubesTables.h` — sample-level headers the core
  includes. `EngineInterface.h` is **locally modified** from upstream: it adds
  `TRESSFX_GODOT` branches that include our `GodotEngineInterfaceImpl.h` /
  `GodotScene.h` instead of AMD's VK/DX12 sample backends.
- `LICENSE.AMD-TressFX.txt` — AMD's MIT license. It must ship with anything derived
  from these files, including our GLSL kernel ports (they are derivative works).

## Provenance

Copied 2026-07-04 from the local AMD TressFX 4.1 checkout (`TressFX/`,
github.com/GPUOpen-Effects/TressFX). Only `src/EngineInterface.h` is knowingly
modified from upstream (the `TRESSFX_GODOT` include switch). Do not edit files here;
they are reference/spec material. The Godot-side code lives in `src/` at the repo root.
