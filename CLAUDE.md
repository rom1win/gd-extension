# Godot TressFX Hair — Project Brief for Claude Code

Read this fully before touching anything. This version (2026-07-06) replaces the earlier
brief, which wrongly declared a GDScript rewrite as the end product. **The product is
the C++ GDExtension.** That confusion cost us a near-deletion of the product — never
repeat it.

## Who you are working with

I am a Python developer. I do not read C++ or GLSL fluently, and I am not a graphics
programmer. Consequences for you:

- Every piece of C++/GLSL/GDScript you write must come with a check **I** can run
  (a Python script, an F5/F6 press in the editor, or a visual check with explicit
  pass/fail criteria). I verify outputs, not code.
- Explain changes in plain language. When graphics concepts are unavoidable, define them.
- I use git. One branch per milestone, merge only on a green gate, tag every gate.
  Never add Co-Authored-By trailers to commits.
- If a gate fails, stop and report; do not stack speculative fixes.
- If the repo contradicts this brief, trust the repo and tell me.

## Objective (the real one)

**Publish a Godot 4.4 (Forward+) C++ GDExtension** that brings AMD TressFX 4.1
real-time hair/fur simulation and rendering to Godot: a user adds hair to a rigged
character, authored in Blender (body exported as glTF, hair as Alembic), and gets
simulated, lit, shadowed strands in-game. The AMD GLSL kernel ports are the technical
heart; the C++ extension around them is the product.

## What is already done and proven (do not redo)

- **Kernel adaptation validated** (the original "phase 1" goal): all 6 TressFX
  simulation kernels ported HLSL→GLSL, audited line-by-line against AMD's originals
  (`AUDIT_KERNELS.md` — zero bugs), and proven to run correctly on Godot's
  RenderingDevice with quantified solver behavior (`NOTES.md` §9).
- **Extracted spec**: `NOTES.md` — UBO layout, buffer bindings, dispatch order,
  `.tfx`/`.tfxbone` real formats, RatBoy parameter defaults, matrix packing convention
  (the authoritative comment lives in `src/GodotScene.cpp`).
- **Main-RD pattern proven**: a GDScript harness (`demo/addons/tressfx/`) runs the same
  kernels on the MAIN RenderingDevice with no per-frame stall — it is the working
  blueprint for milestone A1 and stays as **validation tooling only, not product**.
- **Regression harness**: `reference/` dumps (fixed dt=1/60, wind off, identity bones,
  frames 1/30/120) + `tools/compare_dump.py`. Physics-affecting changes must keep it
  green (or consciously re-baseline with my approval).
- **AMD reference vendored**: `thirdparty/tressfx/` (all needed sources, all 13 HLSL
  shaders, AMD MIT license). The untracked `TressFX/` checkout is obsolete. Never read
  either tree broadly — use `NOTES.md` first, then specific vendored files.
- Git tags: `cpp-reference` (pre-rewrite baseline, never delete/rewrite), `gate-1`.

## Repository state

- `src/` — **the product**: C++ GDExtension (TressFX core + Godot RenderingDevice
  backend). Loads Ratboy `.tfx`/`.tfxbone`, simulates on a **local** RD with per-frame
  submit/sync and CPU readback (bring-up shortcut, replaced in A1), draws debug lines.
- `Sconstruct` + `godot-cpp/` submodule — the build. `scons -Q` from repo root;
  Windows builds get unique timestamped DLL names and the manifest
  (`demo/bin/gdexample.gdextension`) is auto-repointed.
- `demo/shaders/glsl/TressFXSimulation.*.comp.glsl` — the 6 audited GLSL kernels
  (crown jewels; shared by the C++ product and the GDScript harness).
- `demo/` — Godot 4.4 project: `main.tscn` (C++ demo with Ratboy),
  `gdscript_hair.tscn` (validation harness), Ratboy assets.
- `demo/addons/tressfx/` — GDScript validation harness (tfx_asset.gd,
  hair_simulator.gd, hair_sim_node.gd). Not shipped; keep working.
- `thirdparty/tressfx/` — vendored AMD reference + `LICENSE.AMD-TressFX.txt`
  (must ship with the extension; our GLSL ports are derivative works).

## Hard rules

- **Never delete or "retire" the C++ layer** (`src/`, `Sconstruct`, `godot-cpp/`,
  demo scenes, the manifest). It is the product.
- Main RenderingDevice rules: schedule GPU work on the render thread; **no `submit()`,
  no `sync()` on the main RD**. Snapshot skeleton data on the main thread, pass by value.
- Editor safety: no RenderingDevice work under `Engine.is_editor_hint()`.
- No Godot API calls during DLL load (static ctors must not touch Godot types).
- Kernel changes require: audit note referencing `TressFXSimulation.hlsl`, plus a green
  `tools/compare_dump.py` run (or an approved re-baseline).
- Commit only verified-working states. Smallest possible diffs.

## Roadmap (gates are blocking; never start the next rung on a red gate)

- **A1 — main-RD migration.** Move the C++ simulation off the local RD onto the main
  RenderingDevice: dispatches on the render thread, no submit/sync, debug readback
  async-only. Blueprint: `demo/addons/tressfx/hair_simulator.gd`. **Gate A1:** C++ demo
  (F5) visually unchanged; a C++ position dump at fixed dt matches `reference/` dumps
  via `tools/compare_dump.py` (identity bones); frame rate no worse than before.
- **A2 — visible hair.** Real rendered strands for the FULL hair (guides + follow
  hairs — the `UpdateFollowHairVertices` kernel is already ported and audited).
  Rendering approach decided at A2 kickoff; recommendation: expand strands into
  camera-facing ribbons drawn through Godot's standard 3D pipeline (buys lights and
  shadows for free; MSAA + alpha-to-coverage, no TAA), with AMD's ShortCut OIT as a
  later upgrade if quality demands it. Shading reference: vendored
  `TressFXRendering.hlsl` / `TressFXLighting.hlsl` / `TressFXStrands.hlsl`.
  **Gate A2:** Ratboy with full lit/shadowed fur at 1080p ≥ 60 FPS.
- **A3 — animation + collision.** Hair follows the animated skeleton (bone path exists;
  validate under motion) and capsule collision (port the ~40-line block held in reserve
  in `TressFXSimulation.hlsl`). **Gate A3:** violent head-shake keeps roots glued, no
  visible scalp penetration; `compare_dump.py` still green with collision off.
- **B — Blender pipeline.** Editor-only C++ importer: parse Alembic hair curves →
  resample to fixed vertices-per-strand (8/16/32/64) → root-bind to the glTF body mesh
  (nearest scalp triangle, barycentric bone weights) → cook rest lengths, follow
  offsets, movability flags. **Gate B (two parts):** (1) binding math run on Ratboy's
  own mesh reproduces the `.tfxbone` weights — the answer key; (2) my own rigged
  Blender character imports, binds, and simulates. Only after Gate B may the `.tfx`
  parser be removed (or kept as a bonus format — my call then).
- **C — packaging.** Release-template builds (not just debug), clean node API +
  parameter presets, docs, AMD license included. **Gate C:** a fresh Godot project can
  install the addon and put hair on a character following only the docs.

## Where knowledge lives

- `NOTES.md` — the technical spec (formats, layouts, conventions, measured behavior).
- `AUDIT_KERNELS.md` — kernel audit findings F1–F12.
- `thirdparty/tressfx/README.md` — what's vendored and why; do not crawl AMD trees.
- Old TODO.md / IMPLEMENTATION_PLAN.md history lives in git if ever needed.
