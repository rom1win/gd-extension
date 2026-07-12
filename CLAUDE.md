# Godot TressFX Hair — Project Brief for Claude Code

Read this fully before touching anything. This version (2026-07-11) replaces the earlier
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
- **AMD reference vendored**: `thirdparty/tressfx/` (all needed sources, all 13 HLSL
  shaders, AMD MIT license). The untracked `TressFX/` checkout is obsolete. Never read
  either tree broadly — use `NOTES.md` first, then specific vendored files.
- Git tags: `cpp-reference` (pre-rewrite baseline, never delete/rewrite), `gate-1`,
  `gate-2`, `gate-a1` (added at the A1 merge).

## Repository state

- `src/` — **the product**: C++ GDExtension (TressFX core + Godot RenderingDevice
  backend). Loads Ratboy `.tfx`/`.tfxbone`, simulates on the **main** RD (render-thread
  dispatch, no submit/sync — since A1), draws debug guide lines from async readback.
- `Sconstruct` + `godot-cpp/` submodule — the build. `scons -Q` from repo root;
  Windows builds get unique timestamped DLL names and the manifest
  (`demo/bin/gdexample.gdextension`) is auto-repointed.
- `demo/shaders/glsl/TressFXSimulation.*.comp.glsl` — the 6 audited GLSL kernels
  (crown jewels; shared by the C++ product and the GDScript harness).
- `demo/` — Godot 4.4 project: `main.tscn` is the F5 scene; it instances
  `babylon.tscn`, which contains the `TressFXCharacter` node (and its
  `gate_capture_mode` flag). `gdscript_hair.tscn` = validation harness. Ratboy assets.
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

- **A2 — visible hair.** Replace the debug guide lines with real rendered strands for
  the FULL hair (guides + follow — `UpdateFollowHairVertices` already runs). The
  rendering approach is DECIDED (do not reopen it): expand strands into camera-facing
  ribbons drawn through Godot's standard 3D pipeline (gets lights and shadows for
  free; MSAA + alpha-to-coverage, no TAA). AMD's ShortCut OIT is a possible later
  upgrade, not part of A2. Positions flow GPU-to-GPU — the sim's positions buffer is
  copied into an RGBA32F texture each frame and read in a Godot vertex shader via
  `Texture2DRD` (the same mechanism the debug overlay already uses — see
  `m_gpu_guide_lines_texture` in `src/tressfx_character.cpp`). NO CPU readback on the
  render path. Subtasks, in order, each with its check:
  Two DIFFERENT shader languages are involved — do not mix them up:
  (a) RenderingDevice compute GLSL (`.comp.glsl`, like the kernels) for the A2.1 copy
  pass; (b) Godot's own shading language (`.gdshader`, `shader_type spatial;`) for the
  A2.2/A2.3 ribbon material. `Texture2DRD` is the bridge between the two worlds.
  - **A2.1 — position texture.** After the sim dispatches in `_rt_sim_tick`, run a
    small compute pass (new `.comp.glsl` file in `demo/shaders/glsl/`, NOT a kernel
    change) copying all 143360 float4 positions into an RGBA32F texture. 512x512 =
    262144 texels is enough at 1 texel per vertex; vertex v lives at texel
    `(v % 512, v / 512)`. To create the texture and compile/dispatch the pass on the
    main RD, copy the existing debug-overlay pattern (search "GuideLines" in
    `src/GodotEngineInterfaceImpl.cpp` — it already creates a main-RD texture, wraps
    it in `Texture2DRD`, and dispatches a compute pass on the render thread). Expose
    the texture on TressFXCharacter as a `Texture2DRD` property. Check: maintainer
    presses F5 with a debug TextureRect showing the texture — a colorful pattern that
    changes as the hair moves.
  - **A2.2 — ribbon mesh + vertex shader.** Build one static ArrayMesh: for each of
    the 4480 strands, (vps-1)=31 segments x 2 triangles; vertices carry only
    (global_vertex_index, side = -1 or +1) packed into `ARRAY_TEX_UV` (no real
    positions — the vertex shader creates them). The `.gdshader` (spatial) vertex
    shader does `texelFetch(position_tex, ivec2(v % 512, v / 512), 0)` for this
    vertex and the next one on the strand, computes the tangent, and offsets the
    vertex sideways perpendicular to the camera by the hair radius (`fiber_radius`,
    default 0.0021, expose as a node property). Expansion-math reference: vendored
    `TressFXStrands.hlsl` (GetExpandedTressFXVert). TWO PITFALLS, both mandatory:
    (1) the ArrayMesh has garbage local positions, so Godot will frustum-cull it —
    call `MeshInstance3D::set_custom_aabb` with a generous box, e.g. AABB((-2,-2,-2),
    (4,4,4)) around the character (the old debug-lines mesh never needed this because
    it carried real CPU positions; the ribbon mesh does not); (2) attach the
    MeshInstance3D with the same parent/transform as
    the existing GPU debug-lines mesh (see `refresh_gpu_debug_hair_lines_3d` in
    `src/tressfx_character.cpp`) so sim-space positions land in the right place in
    the world. Check: F5 shows the mohawk as solid ribbon geometry following the sim,
    from every camera angle, no popping when orbiting the camera.
  - **A2.3 — shading.** Fragment shader: base/tip color properties, Kajiya-Kay-style
    anisotropic highlight (reference: vendored `TressFXLighting.hlsl`), alpha-to-
    coverage for soft edges (`ALPHA_SCISSOR`/`ALPHA_ANTIALIASING_EDGE` in Godot),
    shadows on (standard pipeline handles them). Check: F5 with a moving light —
    highlight moves along strands, fur casts and receives shadows.
  - **A2.4 — performance + gate.** Turn off the debug line mesh by default (keep the
    property). Run the regression gate (box above). Check FPS.
  **Gate A2:** Ratboy with full lit/shadowed fur at 1080p ≥ 60 FPS (maintainer reads
  the FPS counter), and `compare_dump.py` green.

- **A3 — animation + collision.** Two independent halves; do them in this order.
  - **A3.1 — animation.** Play Ratboy's walk/idle animation (an `AnimationPlayer`
    exists in the scene tree; if none, ask the maintainer). The bone path already
    works (`SnapshotBoneMatrices` per frame); this validates it under real motion.
    Check: F5 with animation playing — roots stay glued to the scalp through the
    whole clip, no lag, no explosion.
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
