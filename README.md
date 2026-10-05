<!-- REUSE-IgnoreStart -->
# mx-guest-linux-kernel

The `mxgpu` DRM/KMS module for MX virtual GPUs. It implements PCI negotiation, DMA command queues, GEM buffers, atomic primary and cursor planes, aperture scanout, render submissions and optional DRM batches.

The module has been built and exercised on AArch64 Linux 7.0. Desktop and graphics API validation is ongoing. The `mxguest` agent transport module and DKMS package staging are not implemented in this repository.

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
```

The output is `mxgpu/mxgpu.ko`. The default kernel build directory is `/lib/modules/$(uname -r)/build`; set `KDIR` to select another. `CORE_DIR` and `LINUX_COMMON_DIR` can select development checkouts and default to the pinned submodules.

Installation and loading are separate from compilation. Check current DRM users and transport shutdown behavior before replacing a running module.

## Licence

GPL-2.0-only. See [LICENCE](LICENCE) and [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES). Compiled MIT dependencies retain their own licence notices. Contribution requirements are in [CONTRIBUTING.md](CONTRIBUTING.md).

<!-- REUSE-IgnoreEnd -->
