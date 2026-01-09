# Shader pack (RenderingDevice)

Godot's `RenderingDevice` ultimately consumes **SPIR-V** (`shader_create_from_spirv`).
This project therefore prefers loading SPIR-V from `res://shaders/spirv/`, and falls back to compiling **GLSL** sources at runtime when SPIR-V is missing.

## Folder layout

- `shaders/spirv/` contains SPIR-V bytecode files.
- `shaders/glsl/` contains GLSL sources (fallback), compiled at runtime using `shader_compile_spirv_from_source()`.

## Naming convention

TressFX calls PSO creation with `(shaderName, entryPoint)` (example: `TressFXSimulation.hlsl` + `VelocityShockPropagation`).
Vulkan GLSL uses `main()` as the entry point, so we use **one GLSL file per TressFX entryPoint**.

GLSL (fallback):

- Compute: `res://shaders/glsl/<Base>.<EntryPoint>.comp.glsl`
  - Example: `res://shaders/glsl/TressFXSimulation.VelocityShockPropagation.comp.glsl`
- Graphics (current placeholder pipeline):
  - Vertex:  `res://shaders/glsl/<Base>.vert.glsl`
  - Fragment:`res://shaders/glsl/<Base>.frag.glsl`

SPIR-V (fallback):

- Compute: `res://shaders/spirv/<name>.comp.spv`
- Vertex:  `res://shaders/spirv/<name>.vert.spv`
- Fragment:`res://shaders/spirv/<name>.frag.spv`

The C++ side loads these via `FileAccess` inside `EI_Device::Create*PSO`.

For GLSL, the C++ side uses:

- `RenderingDevice::shader_compile_spirv_from_source()`
- `RenderingDevice::shader_create_from_spirv()`

## Optional: compiling GLSL -> SPIR-V

### Option A: Vulkan SDK (glslangValidator)

Install the Vulkan SDK and ensure `glslangValidator` is in your PATH.

Examples:

- Compute:
  - `glslangValidator -V -S comp shaders/glsl/write_r32ui.comp.glsl -o shaders/spirv/write_r32ui.comp.spv`
- Vertex:
  - `glslangValidator -V -S vert shaders/glsl/lines.vert.glsl -o shaders/spirv/lines.vert.spv`
- Fragment:
  - `glslangValidator -V -S frag shaders/glsl/lines.frag.glsl -o shaders/spirv/lines.frag.spv`

### Option B: DXC (HLSL -> SPIR-V)

Later, when we port the real TressFX HLSL shaders, we will use DXC to compile HLSL to SPIR-V.
We’ll document the exact command lines and target profiles once shader selection is finalized.

## Notes

- Runtime GLSL compilation keeps the workflow cross-platform and avoids committing generated SPIR-V.
- If you do use SPIR-V, missing `.spv` files will show warnings in the Godot output.
