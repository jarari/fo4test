XeSS MFG compatibility code is derived from Coldwood1026/OptiScaler:
https://github.com/Coldwood1026/OptiScaler
Commit: 9eea95bba9fda7121f214d2eba358423be598d7e (GPL-3.0).

Original authors retain copyright. Modified for Upscaling on 2026-09-19:
host logging/configuration, guarded initialization, and a fixed 8x ceiling.
The original GPL-3.0 terms apply; see the project license.
These are unofficial, build-specific in-memory interoperability patches.
Unrecognized binaries retain stock XeSS behavior. No Intel DLL is patched on disk.

SDK dependency: https://github.com/intel/xess, tag v3.0.2,
commit 8fe81bd (full revision recorded in the dependency checkout).
Distribute Intel's SDK license with the unmodified runtime DLLs.
