# Phase 0 — GLSL Kernel Audit vs AMD TressFXSimulation.hlsl

Audited 2026-07-04 against `TressFX/src/Shaders/TressFXSimulation.hlsl` (TressFX 4.1) and
`TressFX/src/TressFX/TressFXSimulation.cpp` (dispatch order / workgroup math), per the
conventions documented in `src/GodotScene.cpp` (lines 26–59).

Scope (per agreed Phase 0 decisions): 5 kernels. `UpdateFollowHairVertices.comp.glsl`
exists but is deferred (follow hairs become a rendering concern, CLAUDE.md decision 6).

## Overall verdict

**All five kernels are faithful ports.** No correctness bug was found in any kernel.
The uncommitted working-tree change to `CalculateStrandLevelData` (finding F1) is
**correct and should be kept/committed**. No kernel patches are required for Gate 0.

Severity legend: **KEEP** = correct, no action · **NOTE** = benign deviation, carry over
consciously · **RULE** = constraint the GDScript rewrite must respect.

---

## F1 — `CalculateStrandLevelData`: uncommitted `MatToQuat` addition — KEEP (commit it)

The committed version of `ApplyVertexBoneSkinning` returned a placeholder identity
quaternion (`vec4(0,0,0,1)`), meaning "assume the head never rotates". The HLSL
computes the real rotation with `MakeQuaternion(bone_matrix)` and stores it in
`g_StrandLevelData[...].skinningQuat`, which `LocalShapeConstraints` then uses to
rotate each strand's rest shape along with the skeleton.

The uncommitted +45-line diff adds `MatToQuat(mat4)`. Verified element-by-element
against HLSL `MakeQuaternion(column_major float4x4)` (HLSL lines 247–288), accounting
for the memory convention: the CPU packs XMMATRIX row-major with Godot basis columns
as rows; GLSL reads the same bytes column-major, so `m_glsl[a][b] == m_hlsl[a][b]`
numerically. All four branches (trace-positive + three diagonal-dominant cases) map
exactly. Two intentional micro-deviations, both benign:

- GLSL normalizes the resulting quaternion; HLSL does not. Blended bone matrices are
  only approximately orthonormal, so normalizing is a mild robustness improvement.
- Branch selection differs on exact ties between diagonal elements (e.g. `m00 == m11`).
  Any branch yields a valid quaternion; no functional impact.

Without this fix, hair "local shape" ignores skeleton rotation (matches the known bug
noted in IMPLEMENTATION_PLAN.md Phase 1). **Recommendation: commit as part of Gate 0.**

## F2 — All kernels: `AMD_TRESSFX_MAX_NUM_BONES` 128 (GLSL) vs 512 (C++) — NOTE + RULE

C++ (`AMD_TressFX.h`) reserves 512 bone matrices in the constant buffer; the GLSL
blocks declare 128. The C++ uploads a larger buffer than the shader reads — valid in
Vulkan (buffer ≥ shader block), and safe while the skeleton has < 128 bones
(RatBoy: 106). **RULE for rewrite:** pick one value (128 suggested) and use it in both
`_pack_sim_params()` and the kernels, or hair breaks silently past bone 127.

## F3 — `IntegrationAndGlobalShapeConstraints`: extra shared-memory write on reset — NOTE

In the `g_ResetPositions != 0` branch, GLSL also writes `sharedPos = initialPos`;
HLSL does not. Harmless: for movable vertices the value is immediately overwritten by
`Integrate`, and the w component is identical either way.

## F4 — `IntegrationAndGlobalShapeConstraints`: range test `> 0.0` vs truthiness — NOTE

HLSL: `if (stiffness > 0 && globalShapeMatchingEffectiveRange)`. GLSL uses
`range > 0.0`. Identical for all sane (non-negative) parameter values.

## F5 — `IntegrationAndGlobalShapeConstraints`: placeholder `bone_quat` — NOTE

This kernel's copy of `ApplyVertexBoneSkinning` still returns the identity quaternion.
Harmless **in this kernel**: the HLSL computes the quaternion and discards it here
(only positions are used). The real quaternion is produced where it matters, in
`CalculateStrandLevelData` (F1). The two copies of the helper intentionally differ;
the rewrite should keep the cheap version here or unify via an include.

## F6 — All kernels: defensive guards absent from HLSL — NOTE

`max(strandsPerGroup, 1u)`, `clamp(boneIndex, 0, 127)`, `weight_sum > 1e-6` before
divide. These only change behavior on malformed input (where HLSL would divide by zero
or read out of bounds). Keep.

## F7 — `CalculateStrandLevelData`: acceleration uses `.xyz` vs HLSL float4 — NOTE

HLSL computes `length(pos_new[1] - 2*pos_old[1] + pos_old_old[1])` on float4; GLSL on
xyz. Identical because the w components are the same vertex's movability flag across
history and cancel exactly (w − 2w + w = 0).

## F8 — `LocalShapeConstraints`: dead `invBone` dropped — KEEP

HLSL computes `float4 invBone = InverseQuaternion(boneQuat)` and never uses it
(AMD dead code). GLSL correctly omits it. Everything else matches line-for-line,
including the `0.5 * min(stiffness, 0.95)` stability clamp and the vertex 1..n−2 loop.

## F9 — `LengthConstriantsWindAndCollision`: capsule collision compiled out — KEEP (intentional)

Upstream ships with `TRESSFX_COLLISION_CAPSULES 0`; the GLSL faithfully reproduces the
disabled state (`bAnyColDetected = false` kept so the history-rewrite branch matches).
The Phase 5 port target is HLSL lines 449–522 (`CapsuleCollision`) + 831–863
(`ResolveCapsuleCollisions`) + the three cbuffer fields guarded by the same define.
Barrier placement (5 sync points) matches HLSL exactly, with the correct
`GroupMemoryBarrierWithGroupSync` → `memoryBarrierShared(); barrier();` translation.

## F10 — `LengthConstriantsWindAndCollision`: AMD velocity-clamp quirk carried over — NOTE

When a vertex moves farther than `g_ClampPositionDelta` in one step, AMD scales the
delta by `clamp²/speed²` (not `clamp/speed`), i.e. the clamp is quadratic, not exact.
The GLSL reproduces this faithfully — correct for parity. Flag for later tuning only.
Related host fact: the C++ hardcodes `g_ClampPositionDelta = 20.0` with a comment
saying it should be `maxVelocity * timestep` (see NOTES.md).

## F11 — Wind gate `dot(w,w) > 0` vs component-wise `!= 0` — NOTE

Equivalent for any representable nonzero wind. The quirky four-vector blend
`a·W0 + (1−a)·W1 + a·W2 + (1−a)·W3` with `a = (strand % 20)/20` matches HLSL exactly
(including that it sums to ~2× the average wind — an upstream characteristic, not a
porting error). HLSL's shadowed dead `float4 force` is correctly omitted.

## F12 — `VelocityShockPropagation` — KEEP

Exact port. `mix(a, b, t)` ≡ HLSL `(1−t)·a + t·b`; the `localVertexIndex < 2` early-out
and xyz-only writebacks match.

## F13 — A3.2 capsule collision port (2026-07-16) — ADDED, shipped inert

Ported from `thirdparty/tressfx/src/Shaders/TressFXSimulation.hlsl` into
`TressFXSimulation.LengthConstriantsWindAndCollision.comp.glsl`:

- `CollisionCapsule` struct (HLSL 449–453), `CapsuleCollision()` (HLSL 462–522),
  `ResolveCapsuleCollisions()` (HLSL 831–862) — line-for-line; HLSL default arg
  `friction = 0.4f` made explicit in GLSL and passed as `0.4` where AMD's defaults
  resolve. Reuses the kernel's existing `IsMovable()`.
- Call site (HLSL 940–975): replaces the previous disabled placeholder; runs on
  `sharedPos[indexForSharedMem]` against `oldPos =` prev-frame position, followed by
  the kernel's existing `group_sync()` (≡ `GroupMemoryBarrierWithGroupSync`). The
  pre-existing `if (bAnyColDetected)` history-rewrite block downstream matches AMD's
  and was not modified.
- UBO extension in ALL SIX kernels (layout must match `TressFXSimulationParams`):
  appended after `g_BoneSkinningMatrix[]` — `vec4 g_centerAndRadius0[8]`,
  `vec4 g_centerAndRadius1[8]`, `ivec4 g_numCollisionCapsules`
  (`TRESSFX_MAX_NUM_COLLISION_CAPSULES = 8`). New UBO total 8624 bytes (was 8352);
  NOTES.md §2 table updated. C++ side sizes the buffer via `sizeof()` (templated
  `ConstantBuffer<T>`), so it grew automatically.
- C++ enablement: `TRESSFX_COLLISION_CAPSULES=1` defined in `Sconstruct`
  (`TressFXCommon.h`'s `#define 0` wrapped in `#ifndef` to allow the override).
  Upstream bug found in the never-compiled capsule block of `TressFXHairObject.cpp`
  (~431): AMD writes `m_SimCB.m_numCollisionCapsules` but `m_SimCB` is the
  double-buffered array — fixed to `m_SimCB[m_SimulationFrame % 2]->…`; invisible
  upstream because the guard defaulted to 0.
- Shipped INERT: `m_numCollisionCapsules.x = 0` forced every frame (no Godot-side
  wiring yet — that is A3.2 subtask B). Regression gate 2026-07-16: PASS, frame 1
  RMS 0.000000 (bit-identical within readback tolerance).

---

## Cross-checks performed

- **Bindings**: set 0 = {4 initial, 5 restLength, 6 strandType, 7 followRootOffset,
  12 boneSkinning, 13 simParams UBO}; set 1 = {0 pos, 1 posPrev, 2 posPrevPrev,
  3 tangents, 4 strandLevelData}. Matches `TressFXLayouts.cpp` and the HLSL
  `[[vk::binding]]` annotations one-for-one.
- **UBO layout (std140)**: field order matches `TressFXSimulationParams` in
  `TressFXConstantBuffers.h` exactly; the four scalar floats pack tightly (16 bytes)
  before the mat4 array in both C++ and std140.
- **Indexing**: all kernels use the "Master" index variants (strand index × (followHairs+1)),
  i.e. only guide strands are simulated in an interleaved guide+follow buffer. Matches HLSL.
- **Workgroup size** 64 = `TRESSFX_SIM_THREAD_GROUP_SIZE`; dispatch counts
  `totalVertices/64` (vertex kernels) and `totalStrands/64` (strand kernels), local-shape
  kernel optionally repeated `CPULocalShapeIterations` times — matches `TressFXSimulation.cpp`.

## How you verify this audit (no code changes involved)

1. `git diff demo/shaders/glsl/TressFXSimulation.CalculateStrandLevelData.comp.glsl`
   shows only the `MatToQuat` addition described in F1.
2. Run the demo (F5): all six `[TressFX] ... compiled OK` lines, hair settles under
   gravity and follows the skeleton — consistent with "no kernel bug" and F1 active.
3. Every finding above cites the HLSL line numbers; spot-check any of them side by side.
