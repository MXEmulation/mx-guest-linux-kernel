<!-- REUSE-IgnoreStart -->
# Contributing to mx-guest-linux-kernel

Read [README.md](README.md) first. This repository holds the two kernel modules and their build and DKMS machinery. Protocol encoding belongs in mx-guest-core, and the kernel-to-userspace ABI definitions belong in mx-guest-linux-common.

## Working with the submodules

- `deps/core` and `deps/linux-common` are pinned copies of MX's own repositories. Do not edit files under them here. Make the change in the owning repository, then move the pin in this one.
- Move a pin in its own commit, and say in the message which commits it moves between and why.
- Do not declare protocol or ABI version constants in this repository. Read them from Core and from linux-common.

## Kernel conventions

- Follow the kernel's coding style. The planned CI will run `sparse` and `checkpatch.pl`; neither is implemented yet, so run them yourself.
- Both modules must declare `MODULE_LICENSE("GPL")`. Do not change it; see the README for why.
- Anything that controls how a module is compiled or installed (Kbuild, `Makefile`, `Kconfig`, `dkms.conf`, the DKMS staging script) is part of the modules' complete corresponding source. Keep it in this repository, under version control, with a licence header.

## Developer Certificate of Origin

Contributions are accepted under the Developer Certificate of Origin, version 1.1: https://developercertificate.org

Read the full text before signing off. Adding a sign-off to a commit is your certification of that text for that commit.

Sign off every commit with:

```
git commit -s
```

This appends a trailer of exactly this form to the commit message:

```
Signed-off-by: Name <email>
```

The name and email in the trailer must match the commit's author name and author email exactly, including case. The DCO check (`.github/workflows/dco.yml`) runs on every pull request, examines every non-merge commit in it, and fails the pull request if any commit lacks a `Signed-off-by` trailer equal to that commit's `Author Name <author email>`. The check runs only on pull requests. Maintainers who push directly to a branch must still sign off every commit; the requirement is the same whether or not the check runs.

`git commit -s` writes the trailer from your configured `user.name` and `user.email`. If the commit's author is someone else, for example when you commit a change on another person's behalf, the author must add their own sign-off. To add missing sign-offs to your own commits on a branch, use `git rebase --signoff <base>` or, for the last commit only, `git commit --amend -s --no-edit`, then force-push the branch.

## Licence headers

Every new source file carries a two-line SPDX header as its first lines. Following kernel convention, `.c` files use a `//` comment on the first line:

```c
// SPDX-License-Identifier: GPL-2.0-only
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
```

Headers (`.h`) use `/* */` comments:

```c
/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Zak Noble-Clarke */
```

Files that use `#` comments, such as Kbuild, `Makefile`, `Kconfig`, `dkms.conf` and shell scripts:

```
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
```

If you hold copyright in your contribution to a file, add your own `SPDX-FileCopyrightText: <year> <name>` line below the existing ones. Never remove or alter an existing copyright or licence line.

Documentation and repository metadata that cannot carry a header are listed in `REUSE.toml`. Do not add a new header-less file without adding it there, and do not use `REUSE.toml` to avoid putting a header on a source file.

## REUSE compliance

`reuse lint` must pass. It runs in CI (`.github/workflows/reuse.yml`) on every push and pull request. Run it locally before pushing; the tool is described at https://reuse.software.

## Do not copy code from other projects

Write the code yourself. Do not paste or adapt code from other kernel drivers, from the Linux kernel itself, from libdrm, from Mesa or from any other project, even though much of that code is under a GPL-compatible licence. Use the kernel's exported interfaces; do not copy their implementations.

The reason is provenance. Every file here is published as MX's own GPL-2.0-only work, and its copyright line says who wrote it. Code pasted from elsewhere carries someone else's copyright and possibly someone else's licence terms, and once released it is hard to withdraw. A similarity gate that scans changes against a corpus of plausible third-party sources is planned for MX's MIT protocol code; nothing of that kind runs here, so the rule depends on contributors keeping it.

## Material under another licence

No material under a licence other than GPL-2.0-only may enter this repository without an entry in `THIRD-PARTY-NOTICES` in the same commit, naming the work, its SPDX identifier, its copyright holders and its source location, with the original notices kept intact. MIT files arriving through the `deps/` submodules are MX's own and are already recorded there. Raise any other case before opening a pull request.

<!-- REUSE-IgnoreEnd -->
