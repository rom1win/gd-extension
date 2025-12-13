# TressFX Godot Integration - Cauldron Removal Plan

## Context
The TressFX library currently depends on "Cauldron" (AMD's sample framework) for its `SceneGLTFImpl.h` and `EngineInterface.h` implementations. To integrate TressFX into Godot, we need to remove this dependency and provide our own implementations that bridge TressFX to Godot's engine.

## Tasks

### 1. Analysis & Preparation
- [x] Analyze `TressFX/src/SceneGLTFImpl.h` to identify all classes and methods used by TressFX core (specifically `EI_Scene`).
    - **Findings**: `EI_Scene` is used by `TressFXBoneSkinning` and `TressFXSDFMarchingCubes`.
    - **Required Methods**:
        - `int GetBoneIdByName(int skinNumber, const char * name)`
        - `std::vector<XMMATRIX> GetWorldSpaceSkeletonMats(int skinNumber)`
        - `AMD::float4x4 GetMV()`
        - `AMD::float4x4 GetMVP()`
    - **Dependencies to Drop**: `GLTFCommon`, `EI_GLTFTexturesAndBuffers`, `EI_GltfPbrPass`, `EI_GltfDepthPass` are Cauldron-specific and should not be in the Godot implementation.
- [x] Analyze `TressFX/src/EngineInterface.h` and `TressFX/src/VK/VKEngineInterfaceImpl.h` (or DX12) to ensure our `GodotEngineInterfaceImpl.h` is complete.
    - **Findings**: `GodotEngineInterfaceImpl.h` must define `EI_Device`, `EI_CommandContext`, `EI_Resource`, `EI_BindLayout`, `EI_BindSet`, `EI_PSO`, `EI_RenderTargetSet`, `EI_Marker`.
    - **Note**: `EI_Device` needs to provide factory methods for resources and PSOs.
- [x] I have understood that future code will use GOdot RenderingDevice and godot linked code to implement what `TressFX/src/VK/VKEngineInterfaceImpl.h` (or DX12) ise doing.
- [x] Analyze "imgui.h" usage, this is an example of code that is cauldron/demo related and will be useless in a Godot extension, I will list this kind of polluants.
    - **Findings**: `imgui.h` is used in `TressFXSample.cpp` and `VKEngineInterfaceImpl.h/cpp`.
    - **Action**: We must ensure `TressFXSample.cpp` is excluded from the build (it is already excluded by our SConstruct globs). We are replacing `VKEngineInterfaceImpl` so that usage is also gone.
- [x] Update the Analysis and Preparation report in the following section

####### Analysis report.
The TressFX core library (files in `TressFX/src/TressFX/`) has a clean separation from the sample framework (Cauldron), *except* for the `EI_Scene` class and the `EngineInterface` implementation.
- `EI_Scene` is defined in `SceneGLTFImpl.h` which pulls in Cauldron GLTF headers. We must provide a replacement `GodotScene` class that implements the subset of methods used by TressFX (`GetBoneIdByName`, `GetWorldSpaceSkeletonMats`, etc.) without the Cauldron baggage.
- `EngineInterface.h` includes the implementation header (`VKEngineInterfaceImpl.h` or `DX12...`). We must switch this to include `GodotEngineInterfaceImpl.h`.
- `TressFXSample.cpp` is the sample app and depends on `imgui` and Cauldron. It should not be compiled.
- The build system must define `TRESSFX_GODOT` to trigger the header switches.
#######

### 2. Implementation of Godot Interfaces
- [ ] Create `src/GodotTressFXMath.h`.
    - [x] Define `XMMATRIX` (can be a struct wrapping 16 floats or `godot::Projection`).
    - [x] Implement necessary operators for `XMMATRIX` (multiplication by float, addition, multiplication by matrix) as used in `TressFXBoneSkinning.cpp`.
    - [x] This replaces the need for `<DirectXMath.h>`, ensuring cross-platform compatibility and avoiding forbidden DirectX usage.
- [ ] Create `src/GodotScene.h`.
    - [x] Define `class EI_Scene`.
    - [x] Implement the following methods (can be stubs for now, but signatures must match):
        - `int GetBoneIdByName(int skinNumber, const char * name)`
        - `std::vector<XMMATRIX> GetWorldSpaceSkeletonMats(int skinNumber)`
        - `AMD::float4x4 GetMV()`
        - `AMD::float4x4 GetMVP()`
    - [x] Include `"GodotTressFXMath.h"` instead of `<DirectXMath.h>`.
    - [x] Ensure NO Cauldron headers are included.
- [x] Update `src/GodotEngineInterfaceImpl.h`.
    - [x] Define the following classes:
        - `EI_Device` (Factory for resources/PSOs)
        - `EI_CommandContext` (Command recording)
        - `EI_Resource` (Buffer/Texture wrapper)
        - `EI_BindLayout` (Descriptor set layout)
        - `EI_BindSet` (Descriptor set)
        - `EI_PSO` (Pipeline State Object)
        - `EI_RenderTargetSet` (Framebuffer/RenderPass)
        - `EI_Marker` (Debug markers)
    - [x] Define typedef `EI_ResourceFormat`.
    - [x] Ensure `EI_Device` methods return `std::unique_ptr` as expected by TressFX.
    - [x] Ensure `EI_CommandContext` has methods like `SubmitBarrier`, `BindPSO`, `Dispatch`, `UpdateBuffer`, etc.
    - [x] **Verification**: The current implementation in `src/GodotEngineInterfaceImpl.h` contains stubs for all these classes. We will need to fill them in with actual Godot `RenderingDevice` calls later, but for compilation, they are sufficient.
- [x] Create `src/imgui.h` (Dummy).
    - [x] Create an empty file `src/imgui.h` to satisfy `TressFXSettings.h` dependency without pulling in the actual ImGui library.
- [x] Create `src/Simulation.h` and `src/Simulation.cpp`.
    - [x] **Explanation for creating a new file**: We cannot use the original `TressFX/src/Simulation.cpp` because it depends on `HairStrands.h/cpp`. `HairStrands.cpp` has a hard-coded dependency on `<DirectXMath.h>` (it includes it and uses `using namespace DirectX`). Since we must avoid DirectX dependencies for cross-platform compatibility (and cannot easily mock the entire DirectXMath library to satisfy the system include), we must provide our own implementation.
    - [x] Implement a Godot-compatible `Simulation` class that replaces the TressFX sample's `Simulation` class.
    - [x] It should wrap `TressFXSimulation` (the core class).
    - [x] It should NOT depend on Cauldron or DirectX.
    - [x] It should provide the `Initialize`, `Update`, `Draw` methods expected by the usage in `tressfx_character.cpp`.
    - [x] **Note**: `TressFXSimulation` (core) only depends on `EngineInterface.h` and `TressFXCommon.h`, so it is safe to use.
- [x] Update `src/tressfx_character.h`.
    - [x] Include `"Simulation.h"`.
    - [x] Add `std::unique_ptr<Simulation> m_pSimulation;` as a private member.
### 3. Integration Wiring
- [x] Modify `TressFX/src/EngineInterface.h`.
    - [x] Use `insert_edit` or manual replacement to wrap the `SceneGLTFImpl.h` include:
      ```cpp
      #ifndef TRESSFX_GODOT
      #include "SceneGLTFImpl.h"
      #endif
      ```
    - [x] Add the include for our new files inside the existing `#elif defined(TRESSFX_GODOT)` block (or create it):
      ```cpp
      #elif defined(TRESSFX_GODOT)
          #include "GodotEngineInterfaceImpl.h"
          #include "GodotScene.h"
      ```

### 4. Build System Updates
- [x] Update `SConstruct`.
    - [x] Add `env.Append(CPPDEFINES=["TRESSFX_GODOT"])`.
    - [x] Ensure `CPPPATH` includes `src/` (where our Godot implementations live).
    - [x] EXCLUDE `TressFX/src/HairStrands.cpp`, `TressFX/src/Simulation.cpp`, `TressFX/src/SDF.cpp` from the build. These are sample-specific wrappers that depend on Cauldron/DirectX. We will implement our own logic in `src/`.
    - [x] Ensure `TressFXSample.cpp` is NOT compiled.

### 5. Compilation & Verification
- [x] Run `scons` to compile.
- [x] If errors persist about missing types or methods, check `EngineInterface.h` and `SceneGLTFImpl.h` again and update our Godot implementations.
- [x] Once it compiles, we have successfully removed the Cauldron dependency for the build.

### 6. Runtime Initialization & Rendering Setup
- [ ] Update `src/tressfx_character.h` to include rendering components.
    - [ ] Include `"TressFX/TressFXPPLL.h"` and `"TressFX/TressFXShortCut.h"`.
    - [ ] Add `std::unique_ptr<TressFXPPLL> m_pPPLL;` and `std::unique_ptr<TressFXShortCut> m_pShortCut;` as private members.
- [ ] Update `src/tressfx_character.cpp` to initialize components.
    - [ ] In `load_all_assets()` (or a new `initialize_tressfx()` method), instantiate the components:
      ```cpp
      m_pPPLL.reset(new TressFXPPLL);
      m_pShortCut.reset(new TressFXShortCut);
      m_pSimulation.reset(new Simulation);
      ```
    - [ ] Ensure `GetDevice()` can be called. We may need to instantiate a global `EI_Device` in `GodotEngineInterfaceImpl.cpp` and have `GetDevice()` return it.
- [ ] Implement `EI_Device` and `EI_CommandContext` using Godot's `RenderingDevice`.
    - [ ] This is the heavy lifting: mapping TressFX resource creation and command recording to Godot's RD API.
- [ ] Implement `Simulation::Initialize`.
    - [ ] Call `m_tressFXSimulation->Initialize(GetDevice())`.
- [ ] Implement `HairStrands` loading loop.
    - [ ] Port the loop from `TressFXSample::LoadScene` to `TressFXCharacter::load_all_assets`.
    - [ ] This will require creating `HairStrands` objects (which we need to implement/port since we excluded the sample one).
    - [ ] **Correction**: We excluded `HairStrands.cpp` because of DirectXMath. We need to create `src/GodotHairStrands.h/cpp` that implements the `HairStrands` logic using our `GodotTressFXMath.h`.

### 7. Shader Management
- [ ] Plan for Shader compilation/loading (HLSL to SPIR-V or Godot Shaders). TressFX uses HLSL, Godot uses SPIR-V/GLSL. We will need to handle this in `EI_Device::CreateComputeShaderPSO`.
