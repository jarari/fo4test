# RTX40MFG-Unlock integration

This directory contains code derived from Michael Robles' **RTX40MFG-Unlock**
project:

- Upstream repository: <https://github.com/dashdogy/RTX40MFG-Unlock>
- Imported revision: **v1.2.1**, `7296840a6f0c0a7dbb436958486bd2a77aa216be`
- Upstream license: MIT; see [LICENSE](LICENSE)

Credit for the DLSS-G wrapper/NGX patch signatures, provider validation, and
Ada midpoint correction belongs to the RTX40MFG-Unlock project and its author.

## Local adaptation

`midpoint_fix.*`, `dlssg_provider_policy.*`, and `universal_wrapper_profile.h`
are vendored without source changes (apart from line endings) from that revision.
This includes the validated 310.1–310.9 provider profiles, payload/layout checks,
DLSS-G versus DirectSR identity checks, and 1/3/5 generated-frame wrapper limits.
The v1.2.1 hotfix admits exactly DLSS-G 310.9.1 using the existing verified
310.9 temporal profile; unknown 310.9.2 remains rejected. The host integration
and loader adaptation are unchanged from the v1.2 import.
`integration.*` and `loader_discovery.cpp` adapt discovery and fail-closed pattern
patching from upstream `patcher.cpp` to this Fallout 4 F4SE plugin.

The upstream standalone/ReShade UI, configuration/status IPC, worker thread,
`DllMain`, executable IAT hooks, FPS telemetry, and Streamline tag interception
are intentionally excluded. This plugin already owns the Streamline function
pointers, DLSS-G options, state queries, and HUD-less/UI resource tags directly.

Loader-return IAT discovery starts before `slInit` and follows NVIDIA modules,
including opaque-name Streamline plugins. It inspects newly loaded providers
before the loader returns to its caller, and retries after D3D12 device creation.
The original import is published before replacing its IAT slot; conflicting
import chains are left unchanged. Data-file handles are ignored and Win32 last
error is preserved. This is not a process-wide loader hook or DLL notification.

The actual `slDLSSGSetOptions` function obtained by our host selects the active
wrapper, rather than an arbitrary patched module. Its compiled maximum also
caps the advertised state. Inspected plugin/provider DLL references are retained
for plugin lifetime to prevent cached patch pointers from becoming stale.

Unlike upstream's standalone backend, this host adaptation does not install
NGX CreateFeature entry/resolver detours or its Vulkan/control-route machinery.
Provider readiness therefore requires exactly one discovered implementation;
multiple providers fail closed to 2X, rather than guessing which is active.
This intentionally does not claim full upstream multi-provider routing support.

`Streamline::Initialize` clears both `eAllowOTA` and `eLoadDownloadedPlugins`.
Upstream's conditional OTA-enabling and selective-wrapper redirect policies
are intentionally not imported. These SDK flags are not a guarantee against
independent NVIDIA App/driver profile overrides.

Unknown or ambiguous wrapper patterns and unverified midpoint layouts fail
closed. The upstream NGX device-support signature patch is independent of the
stricter midpoint provider version/payload validation.
All wrapper, NGX, and midpoint patches are gated on the upstream CUDA compute
capability 8.9 check, so they remain inactive on non-Ada adapters. This includes
RTX 40-series GPUs and can also include Ada-based professional GPUs; it is not a
consumer-model-name or PCI-device-ID allowlist.
