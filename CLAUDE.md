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
- **Visual quality pass (2026-07-16, on branch `a3-animation-collision`, before A3.1
  resumed).** The A2 ribbons looked "thick and made of light" — fixed without
  touching the rendering architecture:
  - `hair_fiber_radius` default halved (0.0021 → 0.001 m) and strands now taper
    to 40% width at the tip (`tip_width_ratio`), like AMD's thin-tip feature.
  - Specular re-tinted to match AMD's composition (`TressFXLighting.hlsl`
    `ComputeDiffuseSpecFactors` / `TressFXPPLL.hlsl` `HairShading`): only the
    primary (surface-reflection) lobe stays white; the secondary (light that
    passed through the fiber) is now tinted by hair color. Untinted-white on
    both lobes was the direct cause of the washed-out/emissive look. Diffuse
    weight cut to 0.4x (AMD's own ratio is closer to 0.07 Kd against implicit
    specular-dominant shading).
  - Per-strand variation: brightness, thickness, and a soft alpha-faded tip
    length all jitter per strand from a stable hash of strand index (two
    decorrelated hashes — one drives color+thickness, one drives length).
    Cheap (no new buffers), makes the coat read as organic instead of extruded.
  - **Follow-hair radius fixed** (`src/HairStrands.cpp` `GenerateFollowHairs`
    call): was hardcoded `maxRadiusAroundGuideHair=0.0` (NOTES.md issue H2) —
    follow hairs were simulated and rendered but sat exactly on top of their
    guide, invisible. New `follow_hair_radius` property on `TressFXHairNode`
    (default 0.012 m, AMD's own sample value) fans them out; guide physics is
    unaffected (follow hairs are a pure function of guides, and the gate
    compares guide slots only).
  - Sub-pixel width clamp: ribbons thinner than ~1 screen pixel widen to a 1px
    floor and dim to compensate (in `hair_ribbon.gdshader`, using
    `PROJECTION_MATRIX[1][1]` for vertical FOV and `VIEWPORT_SIZE.y`) — fixes
    shimmer/flicker on thinned strands that `alpha_to_coverage` alone doesn't
    touch (that only softens edges, not sub-pixel geometry aliasing).
  - Fixed a spurious per-frame warning: the ribbon `ArrayMesh` (`build_ribbon_
    mesh_if_needed` in `tressfx_character.cpp`) had no `ARRAY_TANGENT`, so
    Godot warned every frame that the shader "requires tangents" even though
    the vertex shader fully overwrites TANGENT/BINORMAL itself — added a
    placeholder tangent array (values unused, immediately overwritten).
  - **Gate status: not yet run.** The follow-hair-radius change is asset
    cooking (physics-affecting per the hard rules) — run the regression gate
    before committing any of this.
- **Scene restructure: DONE, maintainer-verified (2026-07-17, commit 1217a06).**
  `demo/ratboy_node.tscn` is now THE self-contained character scene (body +
  skeleton + head_shake.gd + TressFXCharacter with Collision_body/Hair_mohawk);
  `demo/babylon.tscn` is environment-only and *instances* it; `main.tscn`'s
  `character_path` is `../babylon/RatboyNode/TressFXCharacter`. Character/hair
  properties (including `gate_capture_mode`) are edited INSIDE ratboy_node.tscn,
  not babylon. This killed a full hand-assembled duplicate of the character that
  used to live in babylon.tscn (~1170 lines).
- **Current dev machine (since 2026-07-16): Intel Iris Xe laptop (i7-1185G7).**
  Historical measurements (noise floor, FPS waiver) were on an RX 7900 XTX.
  The AMD-captured `reference/` baselines still pass on Intel (frame-1 RMS
  0.000000 — cross-vendor determinism better than assumed). Godot runtime logs:
  `%APPDATA%/Godot/app_userdata/Nouveau projet de jeu/logs/`. A "0xc000001d
  illegal instruction in godot.exe" event = Godot's own deliberate abort, usually
  after `Vulkan device was lost` (check the log, not the event viewer, for the
  real error).
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

1. In `demo/ratboy_node.tscn` (the character scene — since the 2026-07-17
   restructure the TressFXCharacter lives there, not in babylon.tscn), set
   `gate_capture_mode = true` on the TressFXCharacter node.
2. Ask the maintainer to F5, wait for the three "GATE A1 dump written" lines, close.
3. Run `python tools/compare_dump.py` — must print `REGRESSION: PASS`.
4. Set `gate_capture_mode = false` again. Never commit the scene with it on.

## Roadmap (gates are blocking; never start the next rung on a red gate)

- **A1 — main-RD migration: DONE** (see "already done" above). Tag: `gate-a1`.

- **A2 — visible hair: DONE** (see "already done" above). Tag: `gate-a2`.

- **A3 — animation + collision: DONE, Gate A3 green (2026-07-17), tag `gate-a3`.**
  Two independent halves, done in this order.
  - **A3.1 — animation. CORE FIXED (2026-07-16), maintainer-verified: 35°/2 Hz
    shake with roots glued, believable bending, no vanishing.** babylon.tscn
    has NO AnimationPlayer (the source glTF has one clip, "All Animations",
    never carried into the hand-assembled scene); `demo/head_shake.gd` on the
    Skeleton3D procedurally shakes `frenchHornMonster_head_JNT` and IS the
    Gate A3 stress test. **The old "solver explodes under fast bone motion"
    theory was WRONG** — the solver was never unstable (a readback watchdog
    showed perfectly finite, smooth positions throughout). It was two wiring
    bugs, invisible on a never-animated skeleton and fatal on the first real
    pose write:
    1. **Render mount double-transform** (`update_gpu_debug_hair_lines_3d_
       transform`): the ribbon mesh was parented to a root-bone
       `BoneAttachment3D`, but sim output is in SKELETON MODEL SPACE — bone
       motion already lives in the skinning matrices. A never-dirtied skeleton
       never updates its attachments (accidentally correct); the first pose
       write snapped the anchor to the root joint's saved pose and teleported
       all hair ("vanished"). Fix: mount = the Skeleton3D's global transform.
    2. **Stale bone matrices** (`EI_Scene::GetWorldSpaceSkeletonMats`): the
       skinning-matrix cache was keyed on `Skeleton3D::get_version()`, which
       only bumps on STRUCTURAL changes, never on pose changes — so startup
       matrices were served forever and animation never reached the sim
       (roots didn't follow the head). Fix: recompute every call (~100
       matrices/frame, trivial).
    Diagnostics added during this hunt, kept on purpose: a one-shot NaN/
    teleport watchdog on the readback path (prints "TressFX WATCHDOG" once if
    positions/bones ever go bad — tripwire for A3.2), `debug_force_fixed_dt`
    on TressFXCharacter (forces dt=1/60 with real bones, isolates dt effects),
    and `clamp_position_delta` on TressFXCharacter (AMD's g_ClampPositionDelta
    was hardcoded 20 — sane in AMD's cm-scale world, never fires at our meter
    scale; now plumbed, default 20 = unchanged behavior, available as a
    stabilizer if violent motion ever needs it).
    Still open for A3.1 polish (non-blocking): scene ships `shaking = false`;
    real AnimationPlayer clip playback hasn't been exercised yet (only the
    procedural shake) — worth a quick test when convenient.
  - **A3.2 — SDF collision (pivoted from capsules, maintainer decision 2026-07-17;
    branch `a3.2-sdf-collision`).** The capsule route was implemented first and is
    PARKED, not lost: the kernel port (gate-green, audit note F13) sits on
    `a3-animation-collision` @ 1222d3c, the node/UBO wiring on
    `wip-a3.2-manual-capsule-authoring`. Parked because the kernel/UBO change
    triggers a Vulkan DEVICE-LOST (GPU hang → Windows TDR → Godot deliberate
    abort, logged as 0xc000001d in godot.exe) on the Intel machine in NORMAL mode
    only — capture mode runs fine, so the regression gate could not see it. Root
    cause never isolated (UBO growth vs. kernel loop). Do not merge those branches
    without solving that on Intel first.
    **SDF DONE (2026-07-17, commits efc894c→a91cb20).** Four new kernels ported
    (audit notes S1–S3 in `AUDIT_KERNELS.md`): `TressFXBoneSkinning.BoneSkinning`
    (collision mesh skinned to skeleton, GPU/CPU cross-check verified),
    `TressFXSDFCollision` Initialize/Construct/Finalize (grid build, uint
    atomics + FloatFlip encoding), `CollideHairVerticesWithSdf_forward` (the
    entry AMD actually dispatches — despite its PSO's name suggesting otherwise;
    runs AFTER the full 6-kernel Simulate, before the render copy, covers guide
    AND follow vertices). All separate passes — the six audited sim kernels
    untouched, which is why the ENTIRE road incl. atomics runs clean on the
    Intel machine that device-losts on the capsule kernel change. `.tfxmesh`
    parser is ~100 isolated lines in `src/SDF.cpp` (everything downstream
    consumes plain arrays — Phase B swaps in a from-Godot-mesh loader, nothing
    else changes; the SDF input is the BODY mesh and has nothing to do with
    hair file formats). `sdf_collision_enabled` on TressFXCharacter (C++
    default false; demo scene ships it ON; forced off in gate capture — the
    baseline predates SDF). `show_sdf_debug` on TressFXCollisionNode = voxel
    ghost of the live grid (green surface → red core, ~1s async-readback
    cadence; ships off; format-agnostic, will be the Phase B/C "visible
    collision shapes" authoring aid). Grid tuned ON the voxel view:
    `numCellsInXAxis=32` + `sdf_padding_cells=10` (new property; AMD hardcoded
    40) → 199k cells @1.55 cm, was 2.8M @1 cm — 14× cheaper, maintainer-verified
    coverage AND perf on the Intel iGPU. Also fixed during subtask 4: EI
    backend descriptor set-index bug (GenerateSDF/ApplySDF layouts both
    defaulted to set 0; now pinned 0/1 in `Simulation::Initialize`). One-shot
    runtime checks kept as tripwires: `SKIN CHECK` (GPU vs CPU skinning),
    `SDF CHECK` (grid stats) — they print only when SDF is enabled.
    Maintainer will separately author a PhysicalBone3D physical skeleton
    (ragdoll validation; also future capsule-authoring source if capsules
    ever return).
  **Gate A3: PASSED 2026-07-17.** Violent head-shake, roots glued, fur visibly kept
  out of the body (maintainer visual check, SDF on); `compare_dump.py` green with
  collision off (run at subtask 4; capture mode forces all four SDF kernels off).
- **B — Blender pipeline.** Editor-only C++ importer: parse Alembic hair curves →
  resample to fixed vertices-per-strand (8/16/32/64) → root-bind to the glTF body mesh
  (nearest scalp triangle, barycentric bone weights) → cook rest lengths, follow
  offsets, movability flags. **Gate B (two parts):** (1) binding math run on Ratboy's
  own mesh reproduces the `.tfxbone` weights — the answer key; (2) my own rigged
  Blender character imports, binds, and simulates. Only after Gate B may the `.tfx`
  parser be removed (or kept as a bonus format — my call then).
  **Authoring convention DECIDED 2026-07-18 (feasibility experiment, commit
  960db05 — `test_assets/` + `tools/inspect_abc.py`, run headless in Blender).**
  Measured on Blender 5.0 stock exporters: strand geometry travels perfectly
  (new curves system, uniform 8 control points/strand), per-point width
  travels natively (Alembic `radius`), and the `.abc`/`.glb` coordinate
  spaces MATCH exactly (sphere registration, 0.0000 deviation — no axis/scale
  correction needed). Custom named curve attributes are DROPPED by the
  exporter (proven: attributes present on the evaluated object in the .blend,
  absent from the .abc), and Blender's tilt/orientation data doesn't cross
  either. Decision (maintainer, no-add-on constraint): **stock exports only**
  — hair as plain `.abc` (new curves system, NOT legacy particles: deprecated
  and attribute-less), body as `.glb`; root frames DERIVED at bind time (the
  bound scalp triangle's normal + strand initial direction), twist/width
  ramps and texture/material modes authored per-hair-node IN GODOT, per-strand
  variation via hash jitter. "Blender delivers geometry, Godot owns the look."
  A sidecar-attribute Blender add-on remains a compatible escape hatch if
  hand-authored per-strand data is ever wanted (would override derived values;
  nothing in the design blocks it). `test_assets/hair_test.abc` + `.glb` are
  the importer's reference inputs.
  **SUPERSEDED SAME DAY — headless extraction chosen as PRIMARY (2026-07-18,
  maintainer decision; prototype proven, commit 343739d).** Instead of parsing
  exported files, the importer invokes the user's installed Blender HEADLESS
  (same mechanism Godot itself uses for .blend import; same "Blender
  installed" requirement Godot already imposes, editor-time only) running a
  small bpy script that ships inside OUR Godot addon — nothing to install in
  Blender, nothing for users beyond the addon itself.
  `tools/extract_hair_prototype.py` proves it: positions, per-curve AND
  per-point custom attributes (which every file exporter drops), and
  per-strand root `surface_uv_coordinate` (binding gold) all extracted from
  `hair_test.blend` in one headless run, written to a JSON intermediate.
  Consequences: hand-authored per-strand data (twist etc.) is BACK on the
  table without any add-on; the C++ side consumes our own simple intermediate
  format (the ".tfx parser swap" slot); `.abc` parsing is demoted to an
  optional fallback for no-Blender pipelines (e.g. Houdini-authored grooms) —
  decide at Phase C whether it's worth building at all. Derived root frames +
  Godot-side ramps remain the DEFAULTS; authored attributes override them
  when present.
  **B2 VERTICAL SLICE DONE (2026-07-19, commits acfa16b + 216e3b7,
  maintainer-verified both scenes).** Blender groom → Godot simulation works
  end to end: `tools/blender_hair_extract.py` (headless, uniform arc-length
  resample to --vps, exact root/tip, chunked `.ghair` v1 written; spec lives
  in `tools/verify_ghair.py`, the stdlib verifier) → `src/GhairLoader.cpp`
  fills TressFXAsset exactly like the .tfx loader (incl. AMD's always-pad-to-
  next-64 strand rule — expect a cosmetic tuft where padding strands stack on
  the last real strand) → existing cooking (GenerateFollowHairs/ProcessAsset)
  unchanged. `ghair_file` + `skeleton_node_path` properties on TressFXHairNode
  (hair-only characters can now resolve a skeleton without a collision node).
  Test scene `demo/blender_hair_test.tscn` (F6): 327-strand groom on a unit
  sphere, single-bone identity skeleton, sims with roots planted. RatBoy .tfx
  path untouched and re-verified. REMAINING for Phase B: real bone binding
  from the glTF body (barycentric weights via root surface-UV/nearest
  triangle — Gate B part 1 answer key = reproduce Ratboy .tfxbone), editor
  automation (invoke Blender from the Godot importer instead of manual CLI),
  hide padding strands (cosmetic), maintainer's own rigged character
  (Gate B part 2).
- **C — packaging.** Release-template builds (not just debug), clean node API +
  parameter presets, docs, AMD license included. **Gate C:** a fresh Godot project can
  install the addon and put hair on a character following only the docs.

## Follow-ups (non-blocking; pick up between milestones or when asked)

- **FPS baseline logging (maintainer request, 2026-07-17).** Add an average-FPS
  print/log to the demo (e.g. during gate captures or a fixed 10 s window) so
  future gates have a recorded perf reference instead of "feels better".
  Historical context: A2's 60 fps check was waived on the RX 7900 XTX; the SDF
  tuning (2.8M→199k cells) was judged by feel on the Intel iGPU.
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
- **Visual quality backlog** (from a maintainer-provided proposal doc, sorted
  2026-07-16; goal is "the most beautiful hairs," maintainer will show this to
  other people eventually). Items already done are listed above under "Visual
  quality pass" — not repeated here. Remaining, roughly in the order they're
  worth doing:
  - **Fake multiple scattering** (light-colored hair glows instead of flat
    gray). No payoff on RatBoy's brown fur — do this once a lighter-colored
    test asset exists (Phase B, maintainer's own Blender character), alongside
    the TT/backlight lobe (currently only R + TRT lobes are implemented).
  - **Curl-noise wind** (replace directional wind with 3D turbulence for
    livelier idle motion). This is a **simulation change** (kernel input), so
    it needs an audit note + green regression gate — do it after A3 lands, not
    while A3.1's stability bug is still open (an unstable solver is a bad time
    to add more motion energy to debug against).
  - **Screen-space contact shadows** (micro-shadows where hair meets skin,
    Bend Studio technique, no ray tracing needed). Plausible as a Godot
    CompositorEffect. First confirm the basics the maintainer already
    questioned: does RatBoy's body actually cast a shadow onto the hair, and
    does the hair self-shadow? Cheap checks before building custom shadow tech.
  - **CAS sharpening.** Not a hair feature — a project-level post-process
    setting. Demo-scene tweak at most; lowest priority on this list.
  - **XPBD (compliance term for constraint stiffness).** Rejected for now:
    this rewrites the constraint math inside the six audited GLSL kernels,
    which are line-by-line faithful ports of AMD's reference — that fidelity
    is the point of `AUDIT_KERNELS.md` and the regression baseline. XPBD's
    benefit (tuning stability independent of iteration count/timestep) is a
    convenience, not a visual win, and it invalidates both the audit and the
    baseline. Only reconsider if A3.1's stability work proves the AMD solver
    itself can't be made to cope with fast bone motion.
  - **Velocity grid (hair-vs-hair interaction / cohesion).** A whole new
    simulation subsystem (3D grid build + scatter/gather passes) that AMD's
    own TressFX never shipped. Real payoff mostly on long flowing hair, minor
    on short fur like RatBoy's. Revisit after Phase B if ever, not before.
    UPDATE 2026-07-18: Jolt Physics 5.6 shipped a GPU hair sim USING exactly
    this technique (velocity grid for hair-to-hair) — evidence it's practical;
    upgraded from "if ever" to "probably eventually, post-B". Jolt's Cosserat
    rods also natively carry orientation frames — read their approach before
    building the feather frame-transport pass (borrowable math, not solver).
    Jolt hair is physics-only (no rendering/pipeline) and not integrated in
    Godot; not a competitor to this project's end-to-end scope.
  - **ShortCut OIT** (proper transparency sort for dense semi-transparent
    strands). AMD's vendored code exists but needs custom render passes that
    fight Godot's pipeline — big lift for a benefit that only shows in extreme
    close-ups on light hair. Current MSAA + `alpha_to_coverage` is the
    standard budget answer; revisit at Phase C if close-ups look wrong.
  - **SDF collision** (precise body-shaped collision vs. capsules). The
    roadmap already scopes this correctly under A3.2: capsules first, SDF only
    if capsules visibly fail scalp penetration.
- **Second visual pass — gap analysis + long-term vision (2026-07-16, from a
  maintainer feature-review doc).** Re-audited against the shader as it
  actually stands after the first visual pass above (the review doc itself
  was written against an older, pre-pass shader — most of its "Goal 1
  realistic hair" list already existed: pixel-width clamp, tangent-based
  Kajiya-Kay, tinted two-lobe specular, root/tip darkening, per-strand
  variation. Confirmed no `EMISSION` anywhere in `hair_ribbon.gdshader` or
  set from C++).
  - **Reported "too shiny/plastic, pulsing highlight" — diagnosed, not a bug.**
    A highlight band traveling root<->tip as the light angle changes is
    Kajiya-Kay's tangent-based specular working correctly (`babylon.tscn`'s
    `pointLight1` has `orbit_light.gd` on it) — real hair does this under a
    moving light. Verify by disabling the orbit script: a static light should
    give a static highlight; if it still pulses, that's a real bug. If the
    look is simply too glossy/bright once confirmed, that's lobe-strength
    tuning (`specular_strength_primary/secondary` in `hair_ribbon.gdshader`),
    not an architecture problem. Note `SlimeLight` in the scene is a green
    omni at `light_energy = 9.0` — will bloom any specular it touches.
  - **Shadow verification — DONE (2026-07-18, static-light session).**
    Body-on-hair cast shadow VERIFIED WORKING: spotlight throws a crisp,
    correct shadow line onto the fur ("perfect" per maintainer); the ribbon
    material receives shadows correctly (`ATTENUATION` applied to diffuse and
    both spec lobes — checked in `hair_ribbon.gdshader` light()). The omni
    light initially did NOT shadow the hair — root cause was the LIGHT NODE
    ITSELF: `pointLight1` carried a mirrored basis (uniform NEGATIVE scale
    −0.092, a Maya/Babylon export leftover) which breaks omni shadow-map
    rendering while leaving illumination looking normal. Fixed by resetting
    the basis to identity. LESSON: imported lights must have clean transforms;
    a negatively-scaled light half-works, and only hair exposes it (body
    backsides go dark from facing alone, so they can't reveal a dead shadow
    map — strands have no facing and depend entirely on the shadow term).
    Remaining gap, unchanged: fine hair-on-hair self-shadow (strands too thin
    for shadow maps → interior roots stay lit from any unshadowed direction) —
    that is the existing "fake multiple scattering / depth darkening" backlog
    item, now with maintainer-observed evidence (lit back-roots under a front
    light) even on dark fur.
  - **Maintainer's long-term vision (stated 2026-07-16): stylized ribbons and
    feathers, authored in Blender, mixed on one character (e.g. a harpy with
    realistic hair + feathers, a chimera with fur + feathers), still riding
    the TressFX sim.** Feasibility verdict: **yes, the architecture supports
    this** — TressFX simulates guide polylines only; the sim has no idea
    whether we render a hairline-thin strip, a wide painted ribbon, or a
    feather vane around each guide, so nothing about the sim changes for any
    of this.
    - **Texture mapping modes (tile vs. fit), NEW, not yet built.** One flag
      on `hair_ribbon.gdshader`: `tile` repeats a painted-strand-lines texture
      along the ribbon (stylized/anime hair); `fit` stretches one texture
      root-to-tip with no repeat (feathers, leaves). The UV coordinates
      already exist in the shader (`UV.y` across the ribbon,
      `v_fraction_of_strand` along it) — this is a real but small addition:
      an albedo/alpha (and optionally normal/height) texture sample plus the
      mode switch. Feasible today, even on the existing RatBoy `.tfx` assets.
    - **Feather orientation, NEW, needs Phase B.** A feather must NOT face the
      camera like a hair ribbon does — it needs an orientation frame that (1)
      starts from an authored root normal+tangent bound to the scalp/skin,
      (2) transports along the simulated polyline as it bends (parallel
      transport), (3) carries the artist's authored twist. This is the one
      genuinely new render-side subsystem: a small compute pass after the sim
      writing a per-vertex "frame texture" (root normal transported +
      accumulated twist) next to the existing position texture, then the
      vertex shader orients from that instead of the camera. Needs authored
      per-strand frame/twist data the `.tfx` format doesn't carry — real
      feather mode has to wait for Phase B's importer (see the B roadmap
      bullet above, "scope expanded"). A camera-facing approximation could be
      hacked earlier but would need redoing once B lands real frames — not
      worth it.
    - **Mixed characters (harpy/chimera) — already structurally fine.**
      `TressFXCharacter` already holds a *list* of hair objects
      (`m_hairDescriptions`), each its own `TressFXHairNode` — "fur + feathers
      + hair" is just three hair nodes with different materials/modes on one
      character, no new plumbing needed for that part. One real gap: sim
      tuning (stiffness/damping/etc.) is currently per-CHARACTER
      (`m_gravity_magnitude` and siblings on `TressFXCharacter`), but feathers
      plausibly want different stiffness than hair on the same character —
      this needs to move to per-hair-node. Already flagged as issue H6 in
      `NOTES.md` for unrelated reasons (VSP/iteration params not in the
      Inspector); the fix is the same piece of work.
    - **Known limits, not blockers:** a feather vane rides its simulated shaft
      as a rigid surface — it won't flex independently or catch wind on its
      own; feathers/wide ribbons don't collide with each other or stack
      (needs the deferred velocity-grid idea, still not worth building); wide
      ribbons will interpenetrate the body until A3.2's capsules land, and
      capsules matter MORE once ribbons are wide.
  - **Revised order given all of the above** (delta from the first visual
    backlog's ordering above): shine-tuning + shadow verification first
    (small, directly answers the reported issues) → A3.2 capsule collision
    (unchanged position, and feathers raise its importance) → distance LOD
    (render-only, small, AMD's own `TressFXSettings.h` LOD fields give the
    design for free — fade a distance-driven fraction of strands while
    widening survivors to hold coverage, no kernel/buffer changes) → texture
    modes v1 (tile/fit + alpha/normal, still camera-facing) BEFORE Phase B, so
    the shader's texture plumbing is proven on existing assets before feathers
    need it → Phase B with the expanded schema, where feather mode actually
    lands (frame-texture compute pass + oriented expansion).

## Where knowledge lives

- `NOTES.md` — the technical spec (formats, layouts, conventions, measured behavior).
- `AUDIT_KERNELS.md` — kernel audit findings F1–F12.
- `thirdparty/tressfx/README.md` — what's vendored and why; do not crawl AMD trees.
- Old TODO.md / IMPLEMENTATION_PLAN.md history lives in git if ever needed.
