# RTX40MFG-Unlock integration

This directory contains code derived from Michael Robles' **RTX40MFG-Unlock**
project:

- Upstream repository: <https://github.com/dashdogy/RTX40MFG-Unlock>
- Imported revision: `4ab7b5e16941e065f81c665b6d7fe2c2e2ec843f`
- Upstream license: MIT; see [LICENSE](LICENSE)

Credit for the DLSS-G wrapper/NGX patch signatures, provider validation, and
Ada midpoint correction belongs to the RTX40MFG-Unlock project and its author.

## Local adaptation

`midpoint_fix.*` and `dlssg_provider_policy.*` are vendored from the upstream
revision. `integration.*` adapts the relevant module discovery and fail-closed
pattern patching from upstream `patcher.cpp` to this Fallout 4 F4SE plugin.

The upstream Cyber Engine Tweaks UI, configuration/status IPC, worker thread,
`DllMain`, executable IAT hooks, FPS telemetry, and Streamline tag interception
are intentionally excluded. This plugin already owns the Streamline function
pointers, DLSS-G options, state queries, and HUD-less/UI resource tags directly.

The adapted integration runs after Streamline loads its feature modules and is
retried after creation of the active D3D12 device. Provider versions and binary
layouts which are not explicitly recognized fail closed without being patched.
All wrapper, NGX, and midpoint patches are gated on the upstream CUDA compute
capability 8.9 check, so they remain inactive on non-Ada adapters. This includes
RTX 40-series GPUs and can also include Ada-based professional GPUs; it is not a
consumer-model-name or PCI-device-ID allowlist.
