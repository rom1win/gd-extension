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

---

## SDF collision kernel ports (A3.2, 2026-07-17)

New kernel, approved by the architect as an addition (the six audited
`TressFXSimulation.*.comp.glsl` kernels above are untouched by this work).

### S1 — `TressFXBoneSkinning.BoneSkinning` — ported

Ported from `thirdparty/tressfx/src/Shaders/TressFXBoneSkinning.hlsl`, entry
`BoneSkinning` (lines 57–94) to
`demo/shaders/glsl/TressFXBoneSkinning.BoneSkinning.comp.glsl`. One thread per
collision-mesh vertex; up to 4 weighted bone matrices applied to both
position and normal, exactly as HLSL: accumulate `bone_matrix * weight` for
each of the 4 bone slots with `weight > 0`, divide by `weight_sum`, then
`pos = (bone_matrix * vec4(pos,1)).xyz`, `n = (bone_matrix * vec4(n,0)).xyz`.

Deviations, both mechanical (HLSL→GLSL), not behavioral:
- HLSL indexes `g_BoneSkinningMatrix[...]` directly by `boneIndex[k]`; the
  GLSL clamps each index to `[0, AMD_TRESSFX_MAX_NUM_BONES-1]` before
  indexing. This mirrors the existing defensive-clamp convention already used
  by `TressFXSimulation.IntegrationAndGlobalShapeConstraints.comp.glsl`'s
  `ApplyVertexBoneSkinning` (GLSL has no HLSL-style implicit bounds
  behavior to rely on); never fires in practice since every boneIndex is a
  valid skeleton bone index.
- `AMD_TRESSFX_MAX_NUM_BONES` is 128 here (not AMD's 512), matching the value
  already used by every `TressFXSimulation.*.comp.glsl` kernel and by
  `kBoneCount` in `src/tressfx_character.cpp`. RatBoy's skeleton (and the
  86-bone `Ratboy_body.tfxmesh`) stays well under this cap.
- The matrix-packing convention is identical to the six sim kernels (see the
  authoritative comment in `src/GodotScene.cpp`): the CPU packs a row-major
  skinning matrix; GLSL reads the same bytes as column-major and multiplies
  as `(M * v)`, reproducing HLSL's `mul(v, row_major_M)`.
- The HLSL file's visualization entry points (`BoneSkinningVisualizationVS`/
  `PS`) are NOT ported — no visualization pass exists or was requested this
  subtask.

### Host-side note (not a kernel, but part of this port)

`TressFXBoneSkinning.cpp`'s host class is not compiled into this build (only
`TressFXAsset.cpp`/`TressFXSimulation.cpp`/`TressFXHairObject.cpp`/
`TressFXLayouts.cpp` are, per `Sconstruct`'s explicit keep list). Its
Update()/Initialize() flow (bone-matrix UBO fill, dispatch sizing) was
reimplemented directly in `src/SDF.cpp` (`CollisionMesh::EnsureSkinningPSOCreated`/
`UpdateSkinning`), reusing the still-compiled `GetBoneSkinningMeshLayout()`
from `TressFXLayouts.cpp` unchanged (its binding numbers — u0/t1/t2/b3 —
are exactly what the new GLSL kernel declares).

### S2 — `TressFXSDFCollision` Initialize/Construct/Finalize — ported

Ported from `thirdparty/tressfx/src/Shaders/TressFXSDFCollision.hlsl`:
`InitializeSignedDistanceField` (line ~302), `ConstructSignedDistanceField`
(~330, plus its geometry helpers `GetSdfCoordinates`/`GetSdfCellPosition`/
`GetSdfCellIndex` ~102–129, `DistancePointToEdge` ~147,
`SignedDistancePointToTriangle` ~163), `FinalizeSignedDistanceField` (~401),
to three separate files (one entry point per file, per this subtask's
brief): `demo/shaders/glsl/TressFXSDFCollision.{InitializeSignedDistanceField,
ConstructSignedDistanceField,FinalizeSignedDistanceField}.comp.glsl`.
`CollideHairVerticesWithSdf{,_forward}` (~532/~600) and the unused
`SignedDistancePointToTriangle2`/`GetLocalCellPositionFromIndex` helpers are
NOT ported this subtask — collision response is subtask 4.

**FloatFlip encoding** (HLSL lines 83–100): there is no 32-bit float
`atomic_min` on either backend, so the SDF grid buffer stores `uint`, and a
float distance is written via `FloatFlip3(fl) = (asuint(fl) << 1) |
(asuint(fl) >> 31)` before `InterlockedMin`/`atomicMin`, then read back via
the inverse `IFloatFlip3` after all triangles have splatted their distances.
This bit-rotation makes the *unsigned* integer ordering match the *signed*
float ordering (AMD's comment: "prefers positive values... results in a SDF
with higher quality" vs. the sign-preferring `FloatFlip2`, not ported —
unused by any entry point we call). Ported byte-for-byte:
`floatBitsToUint`/`uintBitsToFloat` replace `asuint`/`asfloat`; GLSL's
`atomicMin` on a `uint` storage-buffer element replaces `InterlockedMin`
directly (both are real device-wide atomics, not workgroup-shared).

**Mechanical HLSL→GLSL changes only, no behavioral change:**
- `RWStructuredBuffer<uint>`/`StructuredBuffer<uint>` become GLSL
  `buffer`/`readonly buffer` blocks with a single unsized `uint[]` member;
  `g_TrimeshVertexIndices.GetDimensions(...)` (used to derive triangle count)
  becomes GLSL's `.length()` on that same unsized array — same value, no
  separate triangle-count uniform needed.
- All three files declare the SAME 4 bindings (`g_TrimeshVertexIndices`,
  `g_SignedDistanceField`, `collMeshVertexPositions`, `ConstBuffer_SDF`) even
  where a given entry point doesn't touch one, so all three PSOs' compiled
  SPIR-V keeps a matching descriptor-set interface for the single shared
  `EI_BindSet` (`src/SDF.cpp`'s `m_sdfBindSet`) — this mirrors AMD's own
  scheme, where every entry point in this family is compiled from the same
  `.hlsl` file and its file-scope declarations regardless of which ones it
  uses.
- `int3`/`float3` become `ivec3`/`vec3`; `InterlockedMin` becomes `atomicMin`;
  `dot`/`length`/`cross`/`min`/`max`/`clamp` are used identically (HLSL and
  GLSL share these intrinsics); the triple-nested cell loop, the
  `GRID_MARGIN`/`MARGIN` padding, and the barycentric/edge-distance math in
  `SignedDistancePointToTriangle` are copied statement-for-statement.

**Grid scheme — read carefully, this is the part the task flagged as
potentially ambiguous.** AMD's `TressFXSDFCollision` (host class,
`thirdparty/tressfx/src/TressFX/TressFXSDFCollision.{h,cpp}`, not compiled
into this build — reimplemented in `src/SDF.cpp`, consistent with S1's
host-side note) computes cell size and grid dimensions **once**, in its
constructor, from the mesh's **rest-pose** AABB
(`GetInitialBoundingBox()`) plus a fixed padding (`0.8 * numCellsInXAxis`
cells on every axis) and a 1.4x cell-count allocation multiplier (headroom;
the kernels themselves only ever touch indices below the *exact*
`NumCellsX*Y*Z`, computed from the UBO, so the extra buffer capacity is
simply unused, never read). Every subsequent `Update()` call recomputes
**only the grid origin**, via `UpdateSDFGrid(mesh->GetBoundingBox())` — and
critically, AMD's own shipped mesh implementation
(`TressFXBoneSkinning::GetBoundingBox()`,
`thirdparty/tressfx/src/TressFX/TressFXBoneSkinning.cpp` lines 439–452) does
**not** read back skinned vertices to get this box. It translates the SAME
rest-pose box by a single rigid delta: it applies the **follow bone**'s
current world-space skinning matrix to the rest-pose box's center, and
adds `(transformed_center - center)` to both the min and max corners. Cell
size and grid dimensions never change again for the mesh's lifetime.

`src/SDF.cpp`'s `CollisionMesh::EnsureSDFPSOCreated()` /
`CollisionMesh::UpdateSDF()` reproduce this exactly, substituting our
already-existing rest-pose tight AABB (`m_aabbMin`/`m_aabbMax`, computed by
`LoadTfxMesh()` in subtask 1) for AMD's rest-pose bounding-sphere-derived
box (AMD's constructor builds a sphere from all rest vertices, then encloses
it in an AABB; the tight AABB we already had is a reasonable, tighter
substitute for the same purpose — it doesn't change the recentering
mechanism, only makes the initial box a closer fit) and using the
already-resolved follow-bone index (`m_followBoneIndex`, via the same
`GetBoneIdByName` mechanism used for per-vertex bone indices) with the same
bone-matrix-snapshot bytes already passed to `UpdateSkinning()` this tick —
no new GPU bounding-box computation exists in either AMD's sample or our
port, and none was invented for this port.

**Ratboy's grid, for reference** (AABB `(-0.248,0,-0.108)`..
`(0.248,0.913,0.344)`, `numCellsInXAxis=50`, `collisionMargin=0.0`):
`cellSize=0.00992` m, padding `=40*cellSize=0.3968` m on every axis,
`numCellsX=130`, `numCellsY=172`, `numCellsZ=125` (exact cells = 2,795,000),
`numTotalCells` (1.4x allocation) `=3,913,000` uint32s (~14.9 MiB).

**Dispatch sequence + barriers**, matching
`TressFXSDFCollision::Update()` exactly: `InitializeSignedDistanceField`
(`ceil(numTotalCells/64)` groups) → UAV barrier → `ConstructSignedDistanceField`
(`ceil(numTriangles/64)` groups) → UAV barrier → `FinalizeSignedDistanceField`
(`ceil(numTotalCells/64)` groups) → UAV barrier. Called from
`Simulation::StartSimulation`, per collision mesh, immediately after
`UpdateSkinning()` (whose own trailing barrier makes the freshly skinned
vertices visible to `ConstructSignedDistanceField`), still gated by
`bUpdateCollMesh` (hardcoded `true` at the `tressfx_character.cpp` call site
until subtask 4 wires the real `sdf_collision_enabled` property). Nothing
consumes the built grid yet — `g_SignedDistanceField` is written and decoded
back to plain floats, but no hair-side kernel reads it until subtask 4's
`CollideHairVerticesWithSdf`.

### S3 — `TressFXSDFCollision.CollideHairVerticesWithSdf_forward` — ported

Ported from `thirdparty/tressfx/src/Shaders/TressFXSDFCollision.hlsl`, entry
point `CollideHairVerticesWithSdf_forward` (~532), plus the trilinear-sampling
helpers it calls: `GetSignedDistance` (~472, reusing `GetSdfCoordinates`/
`GetSdfCellPosition`/`GetSdfCellIndex` from S2) and `LinearInterpolate`/
`BilinearInterpolate`/`TrilinearInterpolate` (~414–469), to
`demo/shaders/glsl/TressFXSDFCollision.CollideHairVerticesWithSdf_forward.comp.glsl`.
One thread per hair vertex.

**Which entry point AMD actually dispatches, and why only that one is
ported.** The HLSL file defines TWO collide entry points:
`CollideHairVerticesWithSdf_forward` (~532, forward-difference-only gradient,
4 trilinear lookups) and `CollideHairVerticesWithSdf` (~600, mixes forward and
backward differences to always stay within one cell — "much faster... but it
could also be less stable", AMD's own comment). `TressFXSDFCollisionSystem::
Initialize()` (`thirdparty/tressfx/src/TressFX/TressFXSDFCollision.h` line 49)
compiles `m_CollideHairVerticesWithSdfPSO` from the string literal
`"CollideHairVerticesWithSdf_forward"` — despite the PSO member's name
matching the *other* entry point, it is the `_forward` variant that
`TressFXSDFCollision::CollideWithHair()` (`TressFXSDFCollision.cpp` ~205)
actually binds and dispatches. That is the one ported here; the mixed-
difference `CollideHairVerticesWithSdf` is unused by AMD's own sample and not
ported (noted for completeness only).

**Where this pass runs relative to the six sim kernels.** No vendored sample
main-loop calls `CollideWithHair()` (`TressFXSDFCollision.{h,cpp}` is a
standalone class; nothing in this repo's vendored tree shows its call site),
so the ordering was derived from `TressFXHairObject.cpp`'s own data-flow
comment (lines 503–509), which is explicit and unambiguous: *"Simulate
updates mPositions and mPositionsPrev (UAVs)... **SDF updates mPositions and
mPositionsPrev (UAVs)**... Render with mPositions and mTangents (PS SRVs)."*
Collide runs strictly AFTER the full `Simulate()` call (all six audited
kernels, `UpdateFollowHairVertices` last) and BEFORE the sim→render UAV→SRV
transition. `Simulation::StartSimulation` (`src/Simulation.cpp`) places it
exactly there: right after `m_tressFXSimulation->Simulate(...)`, before the
`TransitionSimToRendering` loop. Because `CollideHairVerticesWithSdf_forward`
dispatches over `hairObject.GetNumTotalHairVertices()` — guide AND follow
vertices both live in the same interleaved position buffer — follow hairs do
**not** lag a pass: this tick's `UpdateFollowHairVertices` output is already
in the buffer this collide dispatch reads and writes.

**Root-vertex skip, trilinear sampling, gradient/projection — copied
statement-for-statement** (mechanical HLSL→GLSL only): `hairVertexLocalIndex
== 0 || == 1` returns immediately (roots are pinned to the skin mesh, never
collided); `GetSignedDistance` fetches the 8 corner cells of the containing
grid cube via `uintBitsToFloat` (S2's `FinalizeSignedDistanceField` already
undid the `FloatFlip3` encoding, so no bit-rotation is needed here, just a
bit-cast) and returns `INITIAL_DISTANCE` (1e10) if the query point is outside
the grid OR if any corner was never written by a triangle; the early-out
`dist > g_CollisionMargin` skips the (up to) 4x extra `GetSignedDistance`
calls for the gradient on vertices nowhere near the surface; the gradient
uses 3 forward-difference taps at `h = 0.1 * g_CellSize`; the vertex is
projected out along `normalize(sdfGradient)` by `g_CollisionMargin - dist`
and written into BOTH `g_HairVertices` and `g_PrevHairVertices` (zeroing the
one-step velocity the six sim kernels would otherwise compute from the
position delta, exactly as AMD's HLSL does).

**Bindings — new second bind set.** Set 0 reuses S2's `GenerateSDFLayout`
(`src/SDF.cpp`'s shared `m_sdfBindSet`; bindings 0/2 declared but unused by
this entry point, same descriptor-set-interface-consistency rationale as the
three S2 files). Set 1 is `CreateApplySDFLayout()` (`TressFXLayouts.cpp`,
vendored, previously uncompiled-against — `g_HairVertices`/
`g_PrevHairVertices`, both `RW`), bound once per hair object via the
per-object `EI_BindSet` `TressFXHairObject::CreateGPUResources` already
builds automatically over that object's own Positions/PositionsPrev buffers
(`TressFXDynamicState::GetApplySDFBindSet()`) — these are the SAME buffers
the six sim kernels read/write, so writes here are visible to next tick's sim
pass and to rendering with no extra plumbing. `src/SDF.cpp`'s
`CollisionMesh::EnsureCollidePSOCreated()` creates the PSO with
`layouts[] = { GetGenerateSDFLayout(), GetApplySDFLayout() }` (2 layouts,
assigning `ApplySDFLayout` set_index 1 the first time any PSO uses it —
matches the HLSL file's own binding comment, `// bindsets: 0 ->
GenerateSDFLayout, 1 -> ApplySDFLayout`).

**Per-hair-object constant-buffer re-upload.** AMD's own `CollideWithHair()`
(`TressFXSDFCollision.cpp` ~186–198) re-populates the SAME constant buffer
`Update()` (the grid-build pass) just wrote, adding the hair-specific fields
(`m_CollisionMargin * m_CellSize`, `NumHairVerticesPerStrand`,
`NumTotalHairVertices`) — because those fields differ per hair object while
grid origin/cellSize/dims don't change within the tick. `CollisionMesh::
CollideWithHair()` reproduces this: it reuses `m_cellSize`/`m_numCellsX/Y/Z`
(computed once) and `m_lastGridOrigin` (the value `UpdateSDF()` computed
THIS tick, cached in a member since the origin recentering math itself lives
inside `UpdateSDF()`, not duplicated), then re-uploads `m_sdfParamsUBO` with
those plus the hair-specific fields before each dispatch. Collision margin is
scaled to world space exactly like AMD: `collisionMargin = m_SDFCollMargin *
m_cellSize` (S2's `UpdateSDF()` had written this field unscaled — dead code
until this subtask, since nothing read it before now; this subtask's write
immediately supersedes it, same buffer, same tick, right before the collide
dispatch reads it).

**Wiring.** `Simulation::StartSimulation` nests `for (collisionMesh) for
(hairObject) c->CollideWithHair(commandContext, ho);` under
`bSDFCollisionResponse` (previously ignored via `(void)`, now consumed).
`tressfx_character.cpp` adds `sdf_collision_enabled` (bool, default `false`,
registered like `clamp_position_delta`) and replaces the subtask-2-era
hardcoded `bUpdateCollMesh=true` at the `_rt_sim_tick` call site with
`sdfCollisionActive = sdf_collision_enabled && !gate_capture_mode`, computed
on the MAIN thread (`_process`, packed into `params[9]`) and passed by value
to the render thread — never read as a live node property there, per the A1
threading contract. Both `bUpdateCollMesh` and `bSDFCollisionResponse` use
this same flag (no reason to skin/build the SDF grid if nothing will consume
it). Gate capture forces it off unconditionally, so the regression baseline
(which predates SDF collision) is unaffected regardless of the property's
Inspector value.

**Note for whoever runs the visual gate:** `demo/ratboy_node.tscn`'s
`Collision_body` node currently has `collisionMargin` at its default, `0.0`
(see S2's "Ratboy's grid" reference above) — meaning the collide pass only
pushes vertices that are *already inside* the mesh back out to the exact
surface (zero standoff). This is a legitimate, faithful default (AMD ships
the same default), not a bug, but it means the visual effect may look like
"stops penetration" rather than "keeps a visible gap" — raising
`collisionMargin` (a cell-count-style factor, world-space margin =
`collisionMargin * cellSize` ≈ `collisionMargin * 0.00992` m for Ratboy) is a
scene-authoring tweak, not a code change, if a bigger standoff is wanted.
