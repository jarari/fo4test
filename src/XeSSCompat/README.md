# XeSS integration

Dependency: Intel XeSS SDK v3.0.2, commit `8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0`.
Run `powershell -File tools/Setup-XeSS.ps1`, then `xmake build Upscaling`.
The SDK checkout lives under ignored `extern/XeSS`; the setup script never resets an existing checkout.
Build output includes `libxess.dll`, `libxess_fg.dll`, `libxell.dll` and the Intel license.
Install the three DLLs beside `Upscaling.dll`, not in the game's executable directory.

## Settings

- `Settings/iUpscaleMethodPreference`: 0 off, 1 FSR, 2 DLSS (default), 3 XeSS.
- `Settings/iFrameGenerationProvider`: 0 DLSS-G (default), 1 FSR FG, 2 XeSS FG.
  Read before swapchain creation, including non-ENB. Subsequent edits require restarting the game.
  DLSS-G unavailable selects XeSS FG. If XeFG also fails, FG is disabled; it does not silently select FSR.
- `Settings/iXeSSGeneratedFrames`: index 0..6 requests 1..7 generated frames (2x..8x).
  Clamped to the provider's supported count. Changing this does not recreate the swapchain.
- Existing FG enable/menu policy and SR quality settings remain independent of provider selection.

XeSS-FG uses XeLL instead of Reflex. The shared D3D11-to-D3D12 inputs are copied into SDK-owned
storage by `RV_ONLY_NOW` commands on the existing submission, not retained by an asynchronous pacer.
SDK calls that fail may still record work, so these command lists are submitted and retired normally.
SR context destruction/reinitialization drains the existing interop queue only on configuration changes.
XeSS SR uses legacy scale factors to match the engine's existing quality resolutions, non-inverted
depth, UV motion scaled to input pixels, and the same negative engine jitter as NGX/FSR.
ReShade continues to run on the automatic final D3D12 runtime; no manual runtime is introduced.
Existing resource-provenance tracking remains responsible for depth on SDK presentation queues.

## Compatibility limits

The Coldwood MFG unlock and pacing code is gated by PE layout and instruction-byte checks.
SDK v3.0.2 bundles the matching XeFG 1.3.1.78 and XeLL 1.3.2.10 DLLs.
Unknown layouts retain stock SDK capabilities. No Intel file is patched on disk.
See [NOTICE.md](NOTICE.md) for attribution and license.

XeSS-FG 3.0.2 explicitly does **not** support FP16/scRGB swapchains. On such an output,
the user must select FSR FG and restart. This integration does not convert HDR to SDR or rewrite
RenoDX's output. XeSS SR does support FP16 color. Intel's HDR10 path requires matching HDR10
color input and swapchain; it is not equivalent to FP16/scRGB.

## Verification

`tests/build_xess_test.cmd` builds `build/tests/xess-sdk-smoke.exe`.
Run it with the absolute `extern/XeSS/bin` path as its sole argument.
It validates the compatibility patches, SR quality queries and RGBA8 GPU dispatch,
XeFG hidden-window swapchain creation, 2x..8x settings, GPU execution of `ONLY_NOW`
color/depth/motion copies, and clean SDK destruction.
Validated on RTX 4070 Ti; game presentation/pacing was not exercised by this test.
It does not display a window or load/modify Fallout 4.

Gameplay still needs testing with ENB on/off, each SR/FG combination, pause/loading/minimize,
quality/batch-count changes, ReShade depth, and actual generated-frame pacing. An accepted SDK
multiplier is not proof that every generated frame was displayed at the correct time.
