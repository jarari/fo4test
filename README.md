# Disclaimer: This is an AI slop generated with codex and love

# Fallout 4 Upscaling

F4SE plugin for Fallout 4 that experiments with modern upscaling and frame generation in the game renderer.

This repository is a native C++ plugin built with xmake and CommonLibF4. It hooks the Fallout 4 render pipeline, creates a D3D12 proxy swapchain when needed, and integrates:

- NVIDIA Streamline 2.11.1 for DLSS, DLSS-G, and Reflex.
- AMD FidelityFX SDK 2.2.0 for FSR upscaling and frame generation.
- Custom D3D11 compute shaders for depth, motion vector, and frame-generation input preparation.

The `package/` directory contains the loose files that are meant to be installed under the game `Data/` directory.

## Repository Layout

- `src/` - plugin source.
- `include/` - local headers and prebuilt Detours library.
- `lib/commonlibf4/` - CommonLibF4 submodule.
- `extern/Streamline/` - NVIDIA Streamline SDK checkout, tag `v2.11.1`.
- `extern/FidelityFX-SDK/` - AMD FidelityFX SDK checkout, tag `v2.2.0`.
- `package/` - runtime plugin files, shaders, configuration, and bundled runtime DLLs.
- `ref/` - reference material used while developing this port; not required to build.

## Requirements

- Windows x64.
- [F4SE Menu Framework 3](https://www.nexusmods.com/fallout4/mods/105090) for the native in-game settings page.
- Visual Studio 2022 with **Desktop development with C++**.
- Git.
- [xmake](https://xmake.io/).

xmake will fetch these package dependencies automatically:

- `directxtk`
- `directx-headers`
- `magic_enum`
- `simpleini`

## External SDK Setup

The build expects the SDKs at exact folder names under `extern/`.

From the repository root:

```powershell
New-Item -ItemType Directory -Force extern

git clone --branch v2.11.1 --depth 1 https://github.com/NVIDIA-RTX/Streamline extern/Streamline

git clone --branch v2.2.0 --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK extern/FidelityFX-SDK
```

The paths must resolve to:

```text
extern/Streamline/include
extern/FidelityFX-SDK/Kits/FidelityFX/api/include
extern/FidelityFX-SDK/Kits/FidelityFX/backend/dx12
extern/FidelityFX-SDK/Kits/FidelityFX/upscalers/include
extern/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include
extern/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include/dx12
extern/FidelityFX-SDK/Kits/FidelityFX/signedbin
```

The xmake target links against `amd_fidelityfx_loader_dx12.lib` from `extern/FidelityFX-SDK/Kits/FidelityFX/signedbin` and copies these runtime DLLs into the build output after a successful build:

- `amd_fidelityfx_loader_dx12.dll`
- `amd_fidelityfx_upscaler_dx12.dll`
- `amd_fidelityfx_framegeneration_dx12.dll`

## CommonLibF4 Submodule

Initialize the CommonLibF4 submodule before building:

```powershell
git submodule update --init --recursive
```

If this repository was copied without `.git`, make sure `lib/commonlibf4` exists and contains the CommonLibF4 xmake project.

## Build

```powershell
xmake f -m releasedbg
xmake build Upscaling
```

The plugin DLL is produced under:

```text
build/windows/x64/releasedbg/Upscaling.dll
```

## Runtime Files

Install the contents of `package/` into the game `Data/` folder, and install the built `Upscaling.dll` into:

```text
Data/F4SE/Plugins/Upscaling.dll
```

The package includes shader files under `Data/F4SE/Plugins/Upscaling/` and `Data/F4SE/Plugins/FrameGeneration/`. These are compiled at runtime by the plugin, so keep them with the installed mod.

The native settings page is registered under the **Upscaling** section of F4SE Menu Framework. Changes are staged while the menu is open and applied when it closes. Existing installations continue to use `Data/MCM/Settings/Upscaling.ini` as the settings store so user configuration migrates without conversion; the existing Fallout pause-menu close reload path is also retained.

## License

This project is licensed under GPL-3.0. See [LICENSE](LICENSE) and [EXCEPTIONS](EXCEPTIONS).
