<!-- REUSE-IgnoreStart -->
# mx-guest-linux-kernel

The `mxgpu` DRM/KMS module for MX virtual GPUs. It implements PCI negotiation, DMA command queues, GEM buffers, atomic primary and cursor planes, aperture scanout, render submissions and optional DRM batches. When the host offers them, the cursor plane uses the host cursor queue, page-flip events follow host vblank interrupts, and aperture updates copy only the damaged region.

The `mxgpu` module has been built and exercised on AArch64 Linux 7.0. Desktop and graphics API validation is ongoing.

The `mxguest` module is the agent transport. It binds the MX guest-agent PCI device (vendor 0x4d58, device 0x4147), negotiates the split-ring transport, and exposes `/dev/mxguest-agent` (mode 0600, one opener at a time). Each `write()` carries exactly one complete guest-to-host frame and each `read()` returns exactly one complete host-to-guest frame; outgoing frames are validated against the Core frame codec before they are posted. MSI-X is used when available, with a shared INTx fallback. After a device failure the next `open()` resets and reinitialises the device. DKMS package staging is not implemented in this repository.

## Dependencies

Two exact-commit submodules provide the shared protocol and Linux ABI:

- `deps/core`: [mx-guest-core](https://github.com/MXEmulation/mx-guest-core), MIT.
- `deps/linux-common`: [mx-guest-linux-common](https://github.com/MXEmulation/mx-guest-linux-common), MIT.

Changes to either dependency are made in its own repository before updating the pin here.

## Build

A C compiler, Make and headers for the target kernel are required.

```sh
git submodule update --init --recursive
make -C mxgpu
make -C mxguest
```

The outputs are `mxgpu/mxgpu.ko` and `mxguest/mxguest.ko`. The default kernel build directory is `/lib/modules/$(uname -r)/build`; set `KDIR` to select another. `CORE_DIR` and `LINUX_COMMON_DIR` can select development checkouts and default to the pinned submodules; `mxguest` uses only `CORE_DIR`.

Installation and loading are separate from compilation. Check current DRM users and transport shutdown behavior before replacing a running module.

## Licence

GPL-2.0-only. See [LICENCE](LICENCE) and [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES). Compiled MIT dependencies retain their own licence notices. Contribution requirements are in [CONTRIBUTING.md](CONTRIBUTING.md).

<!-- REUSE-IgnoreEnd -->
