# NOTES.md — Extracted spec for the GDScript rewrite

Source of truth extracted in Phase 0 from the working C++ host (`src/`), AMD reference
code (`TressFX/src/TressFX/`), and the RatBoy assets. Everything the GDScript host
(`tfx_asset.gd`, `hair_simulator.gd`) must reproduce lives here. Companion document:
`AUDIT_KERNELS.md` (kernel findings F1–F12).

---

## 1. Matrix / packing convention (THE convention — everything depends on it)

From `src/GodotScene.cpp:26-59`, verified against the GLSL kernels:

- Per-bone skinning matrix = `bone_global_pose * bone_global_rest.affine_inverse()`
  (pose × rest⁻¹). Identity when the skeleton is in rest pose — skinning is then a no-op.
- Packed as 16 floats in this memory order: rows 0–2 = Godot basis **columns** x,y,z
  (each with trailing 0), row 3 = origin (with trailing 1).
  ```
  [ x.x x.y x.z 0 ][ y.x y.y y.z 0 ][ z.x z.y z.z 0 ][ o.x o.y o.z 1 ]
  ```
- GLSL reads those bytes as a column-major `mat4` and multiplies `M * vec4(v, 1)`.
  That combination equals Godot's `basis * v + origin`. Do not transpose anywhere.
- GDScript packing: for each bone, write the 4 basis-column vec4s in the order above
  into the `PackedByteArray`/`PackedFloat32Array` for the UBO tail.

## 2. Simulation constant buffer (std140 UBO, set 0 binding 13)

Field order (must match `TressFXConstantBuffers.h::TressFXSimulationParams` and every
kernel's block declaration — this is the `_pack_sim_params()` spec):

| offset | field | contents |
|---|---|---|
| 0 | `g_Wind` (vec4) | wind pyramid corner 0 (xyz = dir·mag, w unused=0) |
| 16 | `g_Wind1` (vec4) | corner 1 |
| 32 | `g_Wind2` (vec4) | corner 2 |
| 48 | `g_Wind3` (vec4) | corner 3 |
| 64 | `g_Shape` (vec4) | x damping, y localStiffness, z globalStiffness, w globalRange |
| 80 | `g_GravTimeTip` (vec4) | x gravityMagnitude, y timeStep, z tipSeparation, w unused |
| 96 | `g_SimInts` (ivec4) | x lengthIterations, y localIterations, z collisionFlag(0), w unused |
| 112 | `g_Counts` (ivec4) | x strandsPerThreadGroup = 64/vps, y followHairsPerGuide, z vps, w unused |
| 128 | `g_VSP` (vec4) | x vspCoeff, y vspAccelThreshold, zw unused |
| 144 | `g_ResetPositions` (float) | 1.0 on the first two simulated frames, else 0.0 |
| 148 | `g_ClampPositionDelta` (float) | 20.0 (hardcoded upstream; "should be maxVelocity·dt") |
| 152 | `g_pad1`, `g_pad2` (2 floats) | 0 |
| 160 | `g_BoneSkinningMatrix[N]` (mat4 array) | N = 128 in the GLSL kernels (see AUDIT F2) |

Total with N=128: 160 + 128·64 = **8352 bytes**. The C++ side allocates for N=512 and
that mismatch works only because the buffer is bigger than the shader block; the
rewrite must use one N on both sides (128).

Notes:
- Wind pyramid: the 4 corners are the wind direction rotated ±40° around two axes
  (`TressFXHairObject.cpp::SetWind`). Magnitude is pulsed over time:
  `wM = windMag * (sin(frame*0.01)^2 + 0.5)`. Wind is OFF (all zeros) when the
  `wind_velocity` property is zero — which is the current demo default; the motion you
  see in the demo is gravity + inertia + skinning, not wind.
- Reset protocol: upstream keeps TWO UBOs ping-ponged by frame parity and sets
  `g_ResetPositions=1` in both during frames 0–1, cleared afterward. A single UBO
  updated per frame with `reset = (frame < 2)` is equivalent and simpler.
- `SetVerticesPerStrand(n)` derives `g_Counts.x = 64/n` — vps must divide 64
  (valid: 4, 8, 16, 32, 64). RatBoy: vps=32 → 2 strands per thread group.

## 3. Buffers and bind sets

Set 0 (static per-object + UBO), from `TressFXLayouts.cpp::CreateSimLayout`:

| binding | buffer | type | size |
|---|---|---|---|
| 4 | `g_InitialHairPositions` | storage RO | vec4 × numTotalVertices (w = movability) |
| 5 | `g_HairRestLengthSRV` | storage RO | float × numTotalVertices (last vtx of strand = 0) |
| 6 | `g_HairStrandType` | storage RO | int × numTotalStrands (all 0, unused) |
| 7 | `g_FollowHairRootOffset` | storage RO | vec4 × numTotalStrands (w = guide strand index) |
| 12 | `g_BoneSkinningData` | storage RO | 2×vec4 × numTotalStrands (indices as floats + weights) |
| 13 | `tressfxSimParameters` | uniform | see §2 |

Set 1 (read-write sim state), from `CreateSimPosTanLayout`:

| binding | buffer | size | init |
|---|---|---|---|
| 0 | `g_HairVertexPositions` | vec4 × numTotalVertices | copy of initial positions |
| 1 | `g_HairVertexPositionsPrev` | same | same |
| 2 | `g_HairVertexPositionsPrevPrev` | same | same |
| 3 | `g_HairVertexTangents` | vec4 × numTotalVertices | asset tangents |
| 4 | `g_StrandLevelData` | 3×vec4 × numTotalStrands | zeros |

## 4. Dispatch order and workgroup math (per frame)

From `TressFXSimulation.cpp::Simulate`, THREAD_GROUP_SIZE = 64:

1. Upload/refresh the sim UBO (bones + params).
2. `IntegrationAndGlobalShapeConstraints` — groups = numTotalVertices / 64
3. `CalculateStrandLevelData` — groups = numTotalStrands / 64
4. `VelocityShockPropagation` — groups = numTotalVertices / 64
5. `LocalShapeConstraints` — groups = numTotalStrands / 64, dispatched
   `CPULocalShapeIterations` times back-to-back (see §6)
6. `LengthConstriantsWindAndCollision` — groups = numTotalVertices / 64
7. (`UpdateFollowHairVertices` — deferred; follow hairs move to the render shader)

A GPU barrier is required between consecutive kernels (upstream calls UAVBarrier after
each). On Godot ≥ 4.3 main RD, dispatches in one compute list on the same buffers are
dependency-tracked; keep the order above and add `compute_list_add_barrier` equivalents
only if artifacts appear. Group counts divide exactly because the asset pads strand
counts to multiples of 64 (§5).

## 5. Asset formats (spec for `tfx_asset.gd`)

### .tfx (from `TressFXFileFormat.h` header + `TressFXAsset.cpp::LoadHairData`)

Header (little-endian): `float version; uint numHairStrands; uint numVerticesPerStrand;
uint offsetVertexPosition; uint offsetStrandUV; uint offsetVertexUV;
uint offsetStrandThickness; uint offsetVertexColor; uint reserved[32]`.
Positions at `offsetVertexPosition`: float4 × (numHairStrands × vps).
**w carries movability: w=0 pinned, w=1 free. Verified on RatBoy mohawk: the first TWO
vertices of each strand are pinned** (version 4.0, 2228 strands, vps 32, offset 0xA0).

Padding rule (reproduce EXACTLY): guide count is rounded up with
`padded = (n - n % 64) + 64` — note this **always adds** a full group even when n is
already a multiple of 64. Padded strands duplicate the last real strand's vertices.
RatBoy: 2228 → 2240 guides.

### Follow-hair generation (`TressFXAsset.cpp::GenerateFollowHairs`)

Strand buffers become interleaved: guide g occupies strand slot `g*(follow+1)`, its
follow hairs the next `follow` slots. Follow root offset = random offset in the plane
perpendicular to the root segment, radius `maxRadiusAroundGuideHair`; offset.w = guide
strand index. Follow vertex k = guide vertex k + offset · (tipSeparation·k/vps + 1).
Follow vertices copy the guide's w. (Deferred for the rewrite — guides-only buffers;
the ribbon shader reproduces exactly this formula.)

### .tfxbone (what `LoadBoneData` ACTUALLY reads)

⚠️ The `TressFXTFXBoneFileHeader` struct in `TressFXFileFormat.h` does **not** match
the loader. The real layout (verified against RatBoy: first int32 = 105 = bone count):

```
int32 numBones
repeat numBones: { int32 boneIndex; int32 nameLen (incl. NUL); char name[nameLen] }
int32 numStrands
repeat numStrands: { int32 strandIndex(unused);
                     4 × { int32 boneIndex; float weight } }
```

Loader behavior to reproduce: map bone names → engine bone ids at load; boneIndex −1 →
0 (its weight is 0); entries are stored at interleaved slot `i*(follow+1)` (so
LoadBoneData must run AFTER follow-hair generation); guides beyond numStrands (padding)
reuse the LAST strand's skin data. Weights are normalized on the GPU, not at load.
Bone indices are stored **as floats** in the GPU buffer.

### ProcessAsset derived data

- Tangents: per-vertex strand tangents (`ComputeStrandTangent`).
- Rest lengths: `restLength[i] = |pos[i+1] - pos[i]|` per segment; last vertex of each
  strand gets 0.
- Strand types: all zeros (unused).

## 6. Parameters — RatBoy defaults (from `src/Simulation.h` / `tressfx_character.h`)

| parameter | value | exposed in Inspector? |
|---|---|---|
| damping | 0.068 | yes |
| gravityMagnitude | 0.09 | yes |
| globalConstraintStiffness | 0.408 | yes (must be > 0 or hair detaches from skeleton) |
| globalConstraintsRange | 0.308 | yes (must be > 0, same reason) |
| localConstraintStiffness | 0.908 | yes |
| localConstraintsIterations | 3 | no (context default) |
| lengthConstraintsIterations | 3 | no |
| vspCoeff | 0.758 | no |
| vspAccelThreshold | 1.208 | no |
| tipSeparation | 1.0 (node) | per hair node |
| followHairsPerGuide | 1 (node) | per hair node |
| clampPositionDelta | 20.0 | hardcoded |
| timestep | `clamp(delta, 1/240, 1/15)`, 1/60 if delta ≤ 0 | n/a |

Local-shape iteration split (`TressFXHairObject.cpp:405`): if vps ≥ 64 the iteration
count goes into the UBO (`g_SimInts.y`) and the kernel is dispatched once; if vps < 64
(RatBoy: 32) the UBO gets 1 and the **host dispatches the kernel N times**. Keep this:
for vps=32 → 3 back-to-back dispatches of LocalShapeConstraints.

## 7. Host findings — carry over vs do not reproduce

| # | finding | verdict |
|---|---|---|
| H1 | Local RD + per-frame `submit()`/`sync()`/`FlushGPU` + `buffer_get_data` readback of all positions every frame (`Simulation.cpp`, `HairStrands::PackSimulatedGuidePositionsVec4`) | **do not reproduce** — decision 4: main RD, no submit/sync, async debug readback only |
| H2 | `GenerateFollowHairs(..., maxRadiusAroundGuideHair = 0.0)` in `src/HairStrands.cpp:191` — all follow offsets are zero, so each follow hair sits exactly on its guide (invisible duplicates; AMD's own sample uses 0.012). Doubles buffer sizes for zero visual gain | **do not reproduce** — guides-only sim buffers; real offsets belong to the render-side expansion (use ~0.012 m as starting value) |
| H3 | Two ping-ponged UBOs (`m_SimCB[frame%2]`) with reset flag set in both for frames 0–1 | **simplify** — one UBO, `reset = frame < 2` |
| H4 | Uniform sets created lazily at first bind because Godot needs the shader RID; buffer→slot matching is positional (bindSet.resources[i] ↔ layout.resources[i]); set indices assigned by PSO layout order | **do not reproduce** — GDScript creates uniform sets explicitly after shader load with explicit bindings |
| H5 | `SubmitBarrier` ignores its arguments and emits a generic compute barrier | acceptable on Godot RD; rewrite relies on Godot's dependency tracking (§4) |
| H6 | vsp/iteration parameters not plumbed to the Inspector (context defaults only) | rewrite: expose the full table in §6 as export vars / preset Resource |
| H7 | `g_ClampPositionDelta` hardcoded 20.0 with upstream TODO | carry over value; revisit during tuning |
| H8 | Wind magnitude pulses with `sin(frame·0.01)²+0.5` — frame-rate dependent (frame counter, not time) | carry over for parity now; flag for fixed-dt cinematic mode (Phase 8) |
| H9 | `.tfxbone` header struct in AMD's FileFormat.h ≠ actual loader format (§5) | **rule**: parse per §5, trust the loader not the header struct |
| H10 | `TestShader` entry point in HLSL writes g_HairVertexPositions[0] — never dispatched here | ignore |
| H11 | Demo `guidelines_viewer.gd` forces GPU mode (`set_debug_draw_hair_lines(false)`) and the 2D overlay is off by default; in-world blue lines are the primary debug visual | fine for bring-up; replaced in Phase 4 |

## 8. Frame flow of the current C++ demo (for side-by-side reference)

`TressFXCharacter._process(delta)` → snapshot inspector params into a
`SimulationContext` → first tick lazily creates `Simulation` + hair objects →
`StartSimulation`: per hair object {UpdateBones (CPU cache pose×rest⁻¹ per bone),
UpdateSimulationParameters (fills UBO CPU-side)}, then `Simulate` (UBO upload + 6
dispatches per §4), then transition barriers → local-RD submit+sync → CPU readback of
guide positions → rebuild in-world debug line mesh. The rewrite keeps this shape but
moves steps onto the main RD render thread and drops the readback (debug-only, async).
