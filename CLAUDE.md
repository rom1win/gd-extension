# Godot TressFX Hair — Project Brief for Claude Code

Read this fully before touching anything. It encodes decisions already made and validated
in prior analysis. Do not relitigate them unless you find a factual blocker.

## Who you are working with

I am a Python developer. I do not read C++ or GLSL fluently, and I am not a graphics
programmer. This project was bootstrapped with AI assistance. Consequences for you:

- Every piece of C++/GLSL/GDScript you write must come with a check **I** can run
  (a Python script, an F5 press in the editor, or a visual check with explicit
  pass/fail criteria). I verify outputs, not code.
- Explain changes in plain language. When graphics concepts are unavoidable, define them.
- I use git. One branch per phase, merge only on a green gate, tag every gate.
  The tag `cpp-reference` (created in Phase 0) must never be deleted or rewritten.

## Objective

A Godot 4.4 (Forward+) addon for real-time strand hair simulation on one hero character
in a small game, derived from AMD TressFX 4.1, plus a high-quality offline cinematic
rendered with Godot Movie Maker mode (`--write-movie`). Hair is authored in Blender.
Hard constraints: no Blender addon, no third-party standalone tools — everything ships
inside the Godot addon.

## Repository state (read before coding)

- `src/*.cpp|h` — a working C++ GDExtension: TressFX 4.1 core compiled against a custom
  Engine Interface (`GodotEngineInterfaceImpl`, `GodotScene`, etc.) mapped to Godot's
  RenderingDevice. It loads Ratboy `.tfx`/`.tfxbone`, runs the simulation on a **local**
  RenderingDevice with per-frame submit/sync and CPU readback, and visualizes via debug
  lines / a 2D guide-lines texture. It works. It serves as the visual reference during
  the rewrite and is removed only after the GDScript version matches it side by side.
- `demo/shaders/glsl/*.comp.glsl` — GLSL ports of 4 TressFX simulation kernels
  (IntegrationAndGlobalShapeConstraints, VelocityShockPropagation, LocalShapeConstraints,
  LengthConstriantsWindAndCollision). **These are the crown jewels and the future of the
  project.** They are unaudited AI ports — see Phase 0.
- `demo/` — Godot 4.4 project (Forward+), Ratboy assets, `babylon.tscn`, viewer scripts.
- `TressFX/` submodule — AMD TressFX 4.1, **reference only, never compiled into the
  final addon, never shipped**. The only files ever consulted:
  - `src/Shaders/TressFXSimulation.hlsl` (audit reference; un-ported capsule-collision
    block and `UpdateFollowHairVertices` kernel)
  - `src/Shaders/TressFXRender.hlsl` (shading reference for Phase 6)
  - `src/TressFX/TressFXAsset.cpp/.h` (binary spec of `.tfx`/`.tfxbone`, follow-hair
    generation math)
  - `src/TressFX/TressFXSimulation.cpp` (dispatch order, workgroup math, constant-buffer
    field semantics)

## Decisions already made, and why

1. **Keep the TressFX algorithms, drop the TressFX C++ framework.** The value of TressFX
   is the 4 simulation kernels + asset formats + tuned parameters. The C++ runtime around
   them is engine-abstraction bookkeeping that solves AMD's problem, not ours, and I
   cannot maintain it. Final architecture: **GDScript host + GLSL compute kernels**.
   Godot exposes the full RenderingDevice API to GDScript; the host side is ~15 RD calls
   per frame (microseconds), all heavy work stays in GLSL. Port to C++ later only if
   profiling demands it (it won't, for one character).
2. **Testing strategy — deliberate and phased.** No snapshot/parity testing during the
   rewrite (a conscious decision: it confused more than it protected at this stage).
   The rewrite is validated by (a) running old C++ demo and new GDScript demo side by
   side and comparing visually, and (b) **physics invariants**, which need no reference
   data: NaN canary (any NaN in positions fails), rest-length conservation after settling
   (<1% error per segment), monotonic kinetic-energy decay with zero wind, symmetry
   preservation on a symmetric asset. Once the GDScript sim is accepted (Gate 2), capture
   **reference dumps from that GDScript version** (fixed dt=1/60, wind off, identity
   bones, positions at frames 0/30/120) plus a small `tools/compare_dump.py`; from then
   on, all tuning, collision work, and refactors are regression-tested against those
   dumps. Insurance: the pre-rewrite C++ commit is tagged `cpp-reference` and can
   regenerate comparison data later if visual validation ever proves insufficient.
3. **Kernel correctness is handled separately from the rewrite.** The 4 GLSL kernels are
   AI ports of AMD's HLSL and must be audited line by line against
   `TressFXSimulation.hlsl` in Phase 0, before the host rewrite builds on them. Classic
   hazards to check: `mul(v,M)` row-major vs `M*v` column-major (a documented convention
   exists in `src/GodotScene.cpp` comments — read it first), `GroupMemoryBarrierWithGroupSync`
   → `memoryBarrierShared(); barrier();`, int/uint implicit-conversion semantics,
   constant-buffer field order vs `TressFXSimulation.cpp`, and intentional omissions
   (capsule collision is deliberately disabled as upstream default) vs accidental ones.
   Deliver a discrepancy report before patching anything.
   **The same skepticism applies to the existing C++ host code and demo scripts** — they
   were also AI-written and I cannot vouch for them. They must be reviewed in Phase 0,
   not to refactor them (they are about to be replaced), but for two purposes:
   (a) extract the knowledge the GDScript rewrite depends on — matrix/packing
   conventions, RatBoy parameter defaults, constant-buffer field order, buffer set and
   dispatch order — into a `NOTES.md` that becomes the rewrite's spec; (b) flag anything
   suspect or wrong, so known bugs are consciously excluded from the rewrite instead of
   silently re-implemented from the old code.
4. **Simulation moves to the MAIN RenderingDevice.** The local-RD + submit/sync + readback
   pattern was correct for bring-up but is a per-frame GPU stall and strands the data on
   a device the renderer can't touch. New pattern: dispatches scheduled via
   `RenderingServer.call_on_render_thread()`, **no submit(), no sync()** (the engine owns
   the main-RD frame), readbacks only in debug paths via async requests. Never touch
   scene nodes (Skeleton3D) from the render thread — snapshot bone matrices on the main
   thread into a PackedByteArray, pass by value.
5. **Rendering = ribbon mesh + vertex pulling, through Godot's standard pipeline.**
   A small compute kernel copies post-sim positions/tangents into an RGBA32F texture on
   the main RD; a `Texture2DRD` exposes it; a static ArrayMesh (vps × 2 verts per strand,
   strand/vertex ids encoded in UV/CUSTOM0) is expanded into camera-facing ribbons in a
   spatial vertex shader via texelFetch; Kajiya-Kay fragment shading. This buys Godot's
   lights/shadows for free. **MSAA 4× + alpha-to-coverage, explicitly no TAA** (temporal
   AA smears sub-pixel strands; this was a deliberate decision). PPLL/ShortCut OIT and
   SDF collision are deferred luxuries — do not implement them unprompted.
6. **Follow hairs are a rendering concern.** Expand them in the ribbon vertex shader from
   `g_FollowHairRootOffset` + tip separation (guides-only simulation stays untouched);
   distance LOD adjusts follow count/thickness in-shader. Porting the
   `UpdateFollowHairVertices` kernel is the fallback, not the default.
7. **Blender pipeline (late phase).** Blender exports hair curves to Alembic natively (no
   addon). A **minimal editor-only C++ GDExtension import plugin** (the one piece of C++
   that survives) parses `.abc` → cooks a `HairAsset` resource: resample to fixed
   vertices-per-strand, rest lengths, follow offsets, root UV capture, and **root binding**
   (snap each root to nearest scalp triangle from the imported glTF character, barycentric
   coords, blend that triangle's bone weights). At runtime hair is bound to bones only —
   the mesh is used once, at import. Binding code is validated by running it on Ratboy's
   groom+mesh and comparing computed weights to the original `.tfxbone` (the answer key).
8. **Licensing.** The GLSL kernels are derivative of AMD's MIT-licensed HLSL: the addon
   must ship AMD's copyright notice + MIT text (`LICENSE.AMD-TressFX.md`). Verify the
   Ratboy art assets' license before any redistribution beyond the dev repo.

## Phase plan (gates are blocking; never start N+1 on a red gate)

- **Phase 0 — Tag, audit & code review.** `git tag cpp-reference`. Then two reviews per
  decision 3: (a) audit the 4 GLSL kernels against the HLSL, starting with
  `IntegrationAndGlobalShapeConstraints` (everything downstream consumes its output);
  (b) review the existing C++ host layer (`src/*.cpp|h`) and demo GDScript — produce
  `NOTES.md` (the extracted conventions/parameters/layouts the rewrite will follow) and
  a findings list of suspected bugs or oddities, each marked "carry over" or "do not
  reproduce". **Gate 0:** kernel discrepancy report + code review findings delivered and
  discussed with me; agreed kernel fixes applied; C++ demo still runs correctly after
  fixes; `NOTES.md` committed.
- **Phase 1 — `tfx_asset.gd`.** GDScript parser for `.tfx`/`.tfxbone` (spec:
  `TressFXAsset.cpp`), producing a `HairAsset` Resource (PackedFloat32Arrays + counts),
  including follow-root offsets, rest lengths, w-component immovable-root convention.
  **Gate 1:** strand count, vertices-per-strand, and a handful of sampled vertex
  positions printed by the GDScript parser match the same values printed by the C++
  loader (add temporary debug prints to both).
- **Phase 2 — `hair_simulator.gd`.** Buffers, std140 UBO packing (`_pack_sim_params()`
  in one place), kernel loading via RDShaderFile, dispatch loop in TressFX order on the
  main RD per decision 4. Include a fixed-timestep mode (forced dt = 1/60) and a
  debug-only async position readback. **Gate 2:** physics invariants pass (no NaN,
  rest lengths conserved <1% after settling, energy decays); side-by-side run vs the C++
  demo looks identical to my eye; no frame stall vs sim-disabled baseline. **On green:**
  capture reference dumps from this version into `reference/` + write
  `tools/compare_dump.py`; these protect everything below. Then remove the C++ layer
  from the working tree (it lives in `cpp-reference`).
- **Phase 3 — bones.** Main-thread skeleton snapshot (pose × rest⁻¹, packing convention
  from the `GodotScene.cpp` comment), `.tfxbone` name → bone index mapping at load.
  **Gate 3:** head-shake animation keeps roots glued; identity-pose run still matches
  the Phase 2 reference dumps (skinning with identity bones must be a no-op).
- **Phase 4 — visible hair.** Position-texture kernel, Texture2DRD (release RIDs on
  exit-tree/predelete), ribbon ArrayMesh builder, `hair_ribbon.gdshader`
  (cull_disabled, alpha-to-coverage, root→tip taper, Kajiya-Kay). **Gate 4:** lit,
  shadowed hair at 1080p ≥ 60 FPS with full guide count. *Minimal working scene.*
- **Phase 5 — density & collision.** Follow-hair expansion + LOD in shader; re-enable the
  capsule block (port from HLSL, ~40 lines); `hair_collider.gd` capsules on bones.
  **Gate 5:** no visible scalp penetration during violent head-shake at gameplay
  distance; reference-dump test still passes with the collision flag off.
- **Phase 6 — look dev.** Dual-lobe specular (ref `TressFXRender.hlsl`), per-strand
  seeded jitter, root/tip tint, fake self-shadow gradient, parameter preset Resources.
  Gate: subjective (mine).
- **Phase 7 — Blender import.** Editor-only C++ Alembic importer per decision 7.
  **Gate 7:** binding code reproduces Ratboy's `.tfxbone` weights; my own character's
  groom imports, binds, simulates.
- **Phase 8 — cinematic & game presets.** Movie Maker capture (fixed delta = determinism),
  substeps/iterations/MSAA-8× cinematic preset, gameplay perf budget ≈ 2 ms GPU.

## Working rules

- Smallest possible diffs; never refactor unrelated code; the C++ layer is read-only
  except for temporary debug prints agreed in Gates 0–2.
- GDScript by default; C++ only for the Phase 7 importer.
- Every phase delivers: the code, the check I run, a plain-language summary of what
  changed and how I verify it.
- If a gate fails, stop and report; do not stack speculative fixes.
- If you discover the repo contradicts this brief (file names, formats), trust the repo
  and tell me.

## Start now with Phase 0

First actions, in order: (1) read `demo/shaders/glsl/` and the matrix-convention comment
in `src/GodotScene.cpp`; (2) `git tag cpp-reference`; (3) audit
`TressFXSimulation.IntegrationAndGlobalShapeConstraints.comp.glsl` against the HLSL,
then the other three kernels; (4) review the C++ host layer and demo scripts, producing
`NOTES.md` + the findings list; (5) deliver both reports and wait for my go before
changing any kernel or starting Phase 1.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
