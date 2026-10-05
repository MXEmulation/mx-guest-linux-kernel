<!-- REUSE-IgnoreStart -->
# Contributing to mx-guest-linux-kernel

Read [README.md](README.md). Kernel implementation belongs here; device protocol belongs in Core and Linux ABI records belong in Linux Common.

- Change dependencies in their owning repositories before updating `deps/core` or `deps/linux-common`. Pin updates identify the old and new commits and their reason.
- Read protocol and ABI constants from those dependencies.
- Follow kernel coding style and run `sparse` and `checkpatch.pl` locally.
- Keep `MODULE_LICENSE("GPL")` and retain module build and install sources under version control with their licence headers.
- Check DMA ownership, bounded waits and transport shutdown before loading a changed module.

The repository licence is GPL-2.0-only. New C source files start with `// SPDX-License-Identifier: GPL-2.0-only`; headers use a block comment. The copyright notice follows in the file's comment syntax.

## Contributions

Read the [Developer Certificate of Origin 1.1](https://developercertificate.org) before signing off. Every commit requires a `Signed-off-by` trailer matching its author's name and email. Use `git commit -s`; the pull-request DCO workflow checks this requirement.

Write original implementation code. Do not paste or adapt code from other projects; use their supported interfaces. Record any introduced third-party material in `THIRD-PARTY-NOTICES`, retaining its original notices, licence identifier, copyright holders and source location. Discuss material under another licence before adding it.

## File notices and checks

New source files carry this repository's SPDX licence identifier and copyright notice in the file's comment syntax. Preserve existing notices; add a contributor's copyright when appropriate. Files that cannot carry comments are annotated in `REUSE.toml`.

Run the component checks described in [README.md](README.md) and `reuse lint` before submitting. REUSE runs on pushes and pull requests.

<!-- REUSE-IgnoreEnd -->
