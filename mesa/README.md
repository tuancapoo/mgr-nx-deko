# The graphics driver (Mesa NVK for the Switch)

The Switch has no Vulkan driver that homebrew can use, so the NRO carries its own: **NVK**, the open source Vulkan
driver for NVIDIA GPUs from the **Mesa** project, with the **NAK** shader compiler, running on Horizon (the Switch's
operating system). It is linked into the NRO as one static library, `libvulkan.a`.

The initial MGR hardware build uses [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch),
commit `1a8c1a66d6f` (Mesa **26.2.1**). **No nfsmw driver patch is applied.** Optional optimized draw contracts
inherited from sa2-nx must fall back when the linked driver does not expose them. Hardware boot and performance
are still unverified.

The source archive is stored under `out/switch-deps/` with SHA256
`63bd467866a8a1e4cf4a31173c2ec2529a8bc0fd0bc25159a725f4c4cf255d82`.
`tools/switch/build_mgr_nvk_container.sh` builds native shader tools and then Horizon archives inside the dedicated
Docker container. Rust compilation explicitly selects the ELF AArch64 target; the SDK export checks that no x86
or COFF objects enter `libvulkan.a`.

The exported SDK is `out/switch-deps/nvk-1a8c1a66d6f/sdk`, the default of `tools/win/build_switch.ps1`.
`-CompileObjectsOnly` builds the game's ARM64 CPU objects while the driver is still building; a full build requires
the actual static archive and fails if it is absent.

The merged archive must be indexed with devkitA64 `gcc-ranlib`. The host GNU ar's older LLVM plugin omitted Rust
symbols from the index in the first build. A native ELF64 AArch64 link probe failed before reindexing and linked
successfully after it; `tools/switch/build_mgr_nvk_container.sh` now performs this step before export.

Point the build at the result with `NVK_SDK=<sdk>/opt/devkitpro/portlibs/switch` (`tools/win/build_switch.ps1`) or
`-DREXGLUE_SWITCH_NVK_SDK=` when you configure `app/` yourself ([docs/building.md](../docs/building.md)).

Licenses are in the pinned Mesa checkout's `docs/license.rst` and per-file notices.
