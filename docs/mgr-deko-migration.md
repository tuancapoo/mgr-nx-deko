# MGR reNut/deko3d migration

This is an independent working copy (`mgr-nx-deko`). The earlier native-renderer port
(`mgr-nx`) remains the reference project.

## Decision

Use Xbox 360 Direct3D interception from reNut, as adapted by sa2-nx, with plume's
deko3d backend. This bypasses the existing PM4 encoder/decoder and general EDRAM
conversion renderer. A new deko3d backend underneath the current Vulkan renderer
would retain much of the measured CPU work and require larger interface changes.

Reference source: sa2-nx v1.0.3.1,
`adf3a4eee524dcca5cd7c1b42454ddb8a09124a1`, checked out under
`upstream/sa2-deko`. Preserve its third-party notices and component licenses.
Public Jak migration code is useful for API examples, but is not evidence of a
completed reNut-based renderer or a performance prediction for MGR.

## Implementation sequence

1. Snapshot MGR sources into the new project; isolate builds, configuration,
   caches and validation saves. Consume existing game assets read-only.
2. Map MGR XDK 2.0.21173 D3D entry points and private device fields from its
   generated PowerPC listings. Replace command submission while preserving
   original CPU state/resource work and callback/fence ordering.
3. Import reNut/plume and the patched runtime UAM compiler. Build the SDK without
   NVK for the new Switch target. Retain MGR's optimized CPU code and Wwise leaf.
4. Recompile MGR shader containers with reNut's shader ABI. Carry over the
   MGR-specific translator repairs. Validate SPIR-V, converted GLSL and UAM
   compilation; do not reuse the old `.mesp` ABI as if it were interchangeable.
5. Add real half-resolution (360p) encoding on both host and shader sides.
6. Validate actual MGR frames on the PC reNut backend, including menus and the
   supplied saves when practical. Compare rendering with the retained renderer.
7. Build and inspect the ARM64 ELF/NRO, package a separate `switch/mgr-nx-deko`
   folder with matching shader data, normal configuration, provenance and hashes.

## Acceptance evidence

Compilation alone does not establish rendering correctness or a 30 FPS result.
The final validation record must distinguish shader/tool checks, PC frame checks,
Switch cross-build checks, emulator checks (if available), and physical Switch
checks. Physical Switch frame times and correctness require a run on the device.
No SA2 timing figure is treated as an MGR speedup measurement.

The existing `.mesp`, pipeline seed and driver cache are reference-renderer data.
The new renderer writes its own GLSL/DKSH and pipeline caches in its own folder.
No user save is overwritten by a validation run.
