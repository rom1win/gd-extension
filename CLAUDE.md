# Godot TressFX Hair — Project Brief for Claude Code

Read this fully before touching anything. This version (2026-07-13) replaces the earlier
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
- **A1 — main-RD migration: DONE, gate green (2026-07-11).** The C++ simulation runs
  entirely on the main RenderingDevice: kernel PSOs compiled and dispatched on the
  render thread via `RenderingServer::call_on_render_thread`, zero submit/sync (the
  per-frame GPU stall is gone), skeleton matrices snapshotted on the main thread and
  passed by value, debug readback async-only (`buffer_get_data_async`), GPU teardown
  scheduled on the render thread behind in-flight ticks. Capture switch:
  `gate_capture_mode` on the TressFXCharacter node (in `demo/babylon.tscn`) — forces
  fixed dt=1/60, identity bones, wind off, writes dumps at sim steps 1/30/120 to
  `reference_new/`. With identity bones the hair is deliberately NOT skinned to the
  body, so it looks rotated/offset during capture — expected, never ship with it on.
- **Solver is NOT bit-reproducible run-to-run** (measured 2026-07-11, RX 7900 XTX):
  two F5 runs of the identical build diverge up to ~2.4 cm at frame 120 (chaotic
  amplification of GPU float scheduling noise). Frame 1 IS reproducible to ~1e-5.
  Consequence: never compare simulation outputs with exact tolerances past frame 1;
  never interpret sub-noise-floor drift as a bug.
- **Regression harness**: `reference/` dumps (fixed dt=1/60, wind off, identity bones,
  frames 1/30/120) + `tools/compare_dump.py` (statistical gate: frame 1 strict, frames
  30/120 within 3x the measured run-to-run noise floor — the script's docstring
  explains why and how to re-derive limits with `--noise`). Physics-affecting changes
  must keep it green (or consciously re-baseline with my approval).
- **Main-RD pattern first proven** by the GDScript harness (`demo/addons/tressfx/`),
  which stays as **validation tooling only, not product**.
- **A2 — visible hair: DONE, gate green (2026-07-12).** Real rendered strands
  replace the debug guide lines. GPU-to-GPU pipeline, no CPU readback on the
  render path: a compute pass (`TressFXPositionTexture.Copy.comp.glsl`) copies
  every simulated vertex into a 512x512 RGBA32F texture
  (`EI_Device::DispatchPositionTextureCopy`/`GetPositionTextureRID`,
  `TressFXCharacter::get_position_texture`); a ribbon `ArrayMesh` (placeholder
  vertex data only — real positions come from the texture) is shaded by
  `demo/shaders/hair_ribbon.gdshader`, whose vertex stage expands each strand
  into a camera-facing ribbon and whose custom `light()` implements real
  Kajiya-Kay hair lighting (ported from `TressFXLighting.hlsl`
  `ComputeDiffuseSpecFactors`). New node properties: `hair_fiber_radius`,
  `hair_root_color`, `hair_tip_color`, `show_gpu_debug_lines` (debug lines now
  hidden by default). Gate: `compare_dump.py` green (physics unchanged),
  visually confirmed lit/shadowed fur with a moving light, no popping/culling
  from any camera angle. **1080p/60fps was not measured** — the maintainer
  explicitly waived that check ("don't worry about FPS, they are fine"); if
  performance is ever in question later, measure it then.
  Two Godot 4 shader gotchas hit during this work, worth knowing before
  touching `hair_ribbon.gdshader` again: (1) the `light()` processor function
  cannot read `TANGENT`/`BINORMAL` directly (only `vertex()`/`fragment()`
  can) — pass what you need through a `varying` set in `fragment()`; (2)
  `SHADOW_ATTENUATION` is a Godot 3 built-in that no longer exists in Godot 4
  — `ATTENUATION` already includes the shadow term.
- **AMD reference vendored**: `thirdparty/tressfx/` (all needed sources, all 13 HLSL
  shaders, AMD MIT license). The untracked `TressFX/` checkout is obsolete. Never read
  either tree broadly — use `NOTES.md` first, then specific vendored files.
- Git tags: `cpp-reference` (pre-rewrite baseline, never delete/rewrite), `gate-1`,
  `gate-2`, `gate-a1`, `gate-a2` (added at the A2 merge).

## Repository state

- `src/` — **the product**: C++ GDExtension (TressFX core + Godot RenderingDevice
  backend). Loads Ratboy `.tfx`/`.tfxbone`, simulates on the **main** RD (render-thread
  dispatch, no submit/sync — since A1), renders lit/shadowed ribbon hair (since A2).
- `Sconstruct` + `godot-cpp/` submodule — the build. `scons -Q` from repo root;
  Windows builds get unique timestamped DLL names and the manifest
  (`demo/bin/gdexample.gdextension`) is auto-repointed.
- `demo/shaders/glsl/TressFXSimulation.*.comp.glsl` — the 6 audited GLSL kernels
  (crown jewels; shared by the C++ product and the GDScript harness).
  `demo/shaders/glsl/TressFXPositionTexture.Copy.comp.glsl` — A2's GPU-to-GPU
  position feed (also RenderingDevice compute GLSL, NOT a kernel).
- `demo/shaders/hair_ribbon.gdshader` — the ribbon material (Godot's own shading
  language, a completely different system from the `.comp.glsl` files above).
- `demo/` — Godot 4.4 project: `main.tscn` is the F5 scene; it instances
  `babylon.tscn`, which contains the `TressFXCharacter` node (and its
  `gate_capture_mode` flag). `gdscript_hair.tscn` = validation harness. Ratboy assets.
  `free_cam.gd` (F5 scene camera: WASD+mouse, Esc releases cursor),
  `orbit_light.gd` (on `pointLight1` in `babylon.tscn`: auto-orbiting light)
  and `head_shake.gd` (on the Skeleton3D in `babylon.tscn`: procedural
  head-shake for A3, `shaking` export toggles it) are testing tools, not
  product — they exist so visual checks need no manual input.
- `demo/addons/tressfx/` — GDScript validation harness (tfx_asset.gd,
  hair_simulator.gd, hair_sim_node.gd). Not shipped; keep working.
- `thirdparty/tressfx/` — vendored AMD reference + `LICENSE.AMD-TressFX.txt`
  (must ship with the extension; our GLSL ports are derivative works).

## Running from a fresh clone (any computer)

1. `git lfs install` (once per machine), then
   `git clone --recursive https://github.com/rom1win/gd-extension.git`
   (`--recursive` pulls the godot-cpp submodule; LFS pulls the committed DLL).
   If already cloned without LFS: `git lfs pull`.
2. Install Godot 4.4 stable (Forward+). Open `demo/project.godot`, press F5.
   **No C++ build needed to just run**: the debug DLL the manifest points at
   is committed via LFS.
3. To MODIFY the C++: install Visual Studio Build Tools (MSVC) + Python +
   `pip install scons`, then `scons -Q` from the repo root. Each Windows build
   creates a NEW timestamped DLL and auto-repoints the manifest. `*.dll` is
   gitignored, so after a build that should ship: `git add -f demo/bin/<new dll>`
   (it lands in LFS automatically) and commit it together with the manifest.
4. Specs live in `NOTES.md` (formats/layouts/conventions) and
   `AUDIT_KERNELS.md` (kernel audit); the plan is this file. Reference dumps
   for the regression gate are committed in `reference/`.

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

## Working procedure (follow this loop for every task, no exceptions)

1. One milestone = one git branch (e.g. `a2-visible-hair`). Create it from `main`.
2. Do ONE subtask at a time, smallest diff that completes it.
3. After any C++ change: `scons -Q` from the repo root. It must end with
   "Linking Shared Library". If it fails, fix the build before anything else.
4. You cannot run Godot. To test, tell the maintainer exactly: what to click/press,
   what they should see if it works, what they should see if it broke.
5. When the subtask's check passes, commit (small message, no Co-Authored-By),
   then start the next subtask.
6. Physics-affecting change (kernels, buffers, UBO, dt, parameters, asset cooking)?
   Run the regression gate below before committing.
7. Stuck, or something contradicts this brief? Stop and ask. Do not improvise
   around a failing check, and never stack a second fix on an unverified first one.

## How to run the regression gate (memorize this; it recurs at every milestone)

1. In `demo/babylon.tscn`, set `gate_capture_mode = true` on the TressFXCharacter node.
2. Ask the maintainer to F5, wait for the three "GATE A1 dump written" lines, close.
3. Run `python tools/compare_dump.py` — must print `REGRESSION: PASS`.
4. Set `gate_capture_mode = false` again. Never commit the scene with it on.

## Roadmap (gates are blocking; never start the next rung on a red gate)

- **A1 — main-RD migration: DONE** (see "already done" above). Tag: `gate-a1`.

- **A2 — visible hair: DONE** (see "already done" above). Tag: `gate-a2`.

- **A3 — animation + collision. IN PROGRESS (branch `a3-animation-collision`).**
  Two independent halves; do them in this order.
  - **A3.1 — animation. STARTED; open problem below.** babylon.tscn has NO
    AnimationPlayer (the source glTF `demo/Meshes/RatBoy/babylon.gltf` has one
    clip, "All Animations", but it was never carried into the hand-assembled
    scene). Instead, `demo/head_shake.gd` (attached to the Skeleton3D in
    babylon.tscn) procedurally shakes `frenchHornMonster_head_JNT` — this IS
    the Gate A3 stress test. **OPEN PROBLEM:** with the default 35°/2 Hz shake
    the solver explodes and the hair vanishes (NaN/inf positions make the GPU
    discard the ribbons — the failure looks like sudden baldness, not visible
    chaos). The scene currently ships with `shaking = false` on the Skeleton3D
    node; flip it to true to reproduce. The fix is stabilizing the sim under
    fast bone motion, NOT weakening the test. Candidate causes to investigate,
    in order: (1) variable per-frame dt feeding the solver while bones jump
    far per frame (try fixed dt first to isolate); (2) start gentle — find the
    amplitude/frequency where instability begins (edit exported vars on the
    Skeleton3D node) to learn whether it's a cliff or gradual; (3) the AMD
    reference clamps per-step motion (`g_ClampPositionDelta = 20`, set in
    `TressFXHairObject.cpp::UpdateSimulationParameters`) — check the GLSL
    kernels actually apply it the way `TressFXSimulation.hlsl` does; (4) VSP
    (velocity shock propagation) parameters exist precisely for fast head
    motion — check NOTES.md §6 and the VSP kernel inputs. Check when fixed:
    F5 with `shaking = true` — hair swings believably, roots stay glued, no
    disappearance, for at least 30 seconds.
  - **A3.2 — capsule collision.** Port the collision-capsule block (~40 lines,
    `CapsuleCollision()` + its call site) from vendored `TressFXSimulation.hlsl` into
    `TressFXSimulation.LengthConstriantsWindAndCollision.comp.glsl`. This IS a kernel
    change: write the audit note referencing the HLSL lines, wire capsule data from
    `TressFXCollisionNode` into the sim UBO (fields exist — see
    `TRESSFX_COLLISION_CAPSULES` in `thirdparty/tressfx/src/TressFX/TressFXHairObject.cpp`),
    add a `collision_enabled` property (default false).
  **Gate A3:** violent head-shake keeps roots glued and no visible scalp penetration
  (maintainer's visual check); `compare_dump.py` still green with collision OFF.
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

## Follow-ups (non-blocking; pick up between milestones or when asked)

- **Nondeterminism hunt.** Find WHY the solver isn't bit-reproducible run-to-run
  (candidates: threadgroup shared-memory scheduling in the strand-level kernels;
  first-frame reads of not-yet-written prev/prevPrev position buffers). If it turns
  out fixable, the strict bit-exact gate can return (needs a re-baseline + maintainer
  approval). Diagnostic tool already exists: `tools/compare_dump.py --noise dirA dirB`
  on two captures of the same build. Worth doing before A3 debugging if chaos makes a
  regression ambiguous; otherwise low priority.
- **Dead code cleanup.** `HairStrands::PackSimulatedGuidePositionsVec4` (old blocking
  readback, no callers since A1) — delete. `EI_Device::RunSelfTestOnce` submits/syncs
  on whatever `get_rd()` returns, which since A1 is the MAIN RD — guard it against the
  main RD (or delete) before anyone wires it to a button.

## Where knowledge lives

- `NOTES.md` — the technical spec (formats, layouts, conventions, measured behavior).
- `AUDIT_KERNELS.md` — kernel audit findings F1–F12.
- `thirdparty/tressfx/README.md` — what's vendored and why; do not crawl AMD trees.
- Old TODO.md / IMPLEMENTATION_PLAN.md history lives in git if ever needed.
