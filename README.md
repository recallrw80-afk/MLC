# MLC — MinecraftLauncherCLI

**English** | [简体中文](README.zh-CN.md)

A cross-platform Minecraft launcher written in C++20 / Qt 6, centered on the lightweight command-line frontend `mlc` (RSS ~4MB) — suitable for desktops, servers, and headless environments.

## Repository layout (swapped 2026-11-14)

The repo is now **Rust-first** (Rust lives at the root); the C++ implementation moved into `cpp/` as reference / transition code:

```
MLC/
├── Cargo.toml crates/       # Rust workspace (main line): mlccore + mlc + mlccore-ffi
├── README.md release.sh     # root-level files for the Rust main line
└── cpp/                     # C++/Qt implementation (kept during migration, feature-frozen)
    ├── sdk/ cli/ gui/       # original C++ core, CLI, QML GUI
    └── CMakeLists.txt Makefile
```

> Building the C++ version requires `cd cpp` first; the Rust version builds from the repo root with `cargo build`.

## Features

- **CLI first**: modpack import, instance management, version download, and game launch all work in a terminal; interactive pickers and wizards on TTY (create-vite style)
- **Modpack import**: CurseForge / Modrinth / MultiMC / HMCL / PCL launcher packs / compressed `.minecraft` / plain mod packs; automatic download of game files, modloaders (Forge / NeoForge / Fabric), and mods
- **Isolated instances**: each instance gets its own directory; `.incomplete` marker + full rollback on failure — no half-installed instances
- **Vanilla download**: install any MC version with one command (latest release by default); re-running verifies and repairs
- **Java management**: detects system Java, picks per MC version compatibility matrix, auto-downloads JRE from Adoptium when missing
- **Player profiles**: multiple offline player profiles (name / avatar / skin type) with interactive add/edit/remove/select; authlib-injector external login (e.g. LittleSkin) with encrypted, persisted sessions auto-refreshed online at launch
- **Launch stability**: automatic memory sizing (50% of available RAM, capped at 16G), GC tiers, auto-fix for fcitx/ibus XIM crashes (GLFW 3.4 replacement), full launch logs on disk
- **Local server hosting**: install and run vanilla/Forge/Fabric/NeoForge servers with two commands (`server-install`/`server-start`), console attached, headless-friendly
- **QML GUI** (development paused — see below): source retained, not shipped in release packages

> **GUI development status: PAUSED (2026-11-14)**
>
> The graphical front-end (the QML app under `gui/`, binary name `mlc-gui`) is currently **on hold**. Focus is on the CLI and the SDK.
>
> - **Not part of the release pipeline**: `make package-tar` / `release.sh` only package the command-line `mlc`. Prebuilt packages do **not** contain `mlc-gui`, so regular users cannot install it.
> - **Source-only**: developers can still build and run it locally with `make run`. It is a test-version UI (basic scaffolding for Launch / Download / Settings / More pages).
> - **Paused, not abandoned**: all QML, components, and the C++ bridge layer stay in the repo and can be resumed at any time.
>
> Before resuming, the zero-dependency distribution problem must be solved first (see the "GUI 开发暂停" section in `CLAUDE.md`): `bundle-dist.sh` currently bundles only `plugins/tls/` — no Qt **platform plugins** (`platforms/`), no Wayland shell-integration plugins, and no QML modules. Packaging the GUI as-is would fail to start with "no Qt platform plugin could be initialized".
>
> For everyday use, stick to the CLI: run `mlc help` for all commands.

## Install

### One-liner (recommended)

Official prebuilt package (embedded CurseForge key — the full experience), for x86_64 (amd64) and macOS (Apple Silicon). Linux aarch64 users: build from source below (Qt has no official 6.11 Linux ARM64 packages, so no prebuilt for now):

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/latest/download/install.sh | bash
```

In China, the source falls back automatically — GitHub first, Gitee mirror on failure; no flags needed (add `--beta` for pre-releases).

> The Gitee mirror ships prebuilt binaries only (no source archives, no source builds). To build from source, clone from GitHub: `git clone https://github.com/recallrw80-afk/MLC.git`

To install the latest **pre-release** (Beta): `latest` never points at pre-releases, so fetch install.sh from the tag URL and pass `--beta`:

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/download/v0.1.3-beta/install.sh | bash -s -- --beta
```

(Replace `v0.1.3-beta` with the latest pre-release tag.)

Or grab the tarball from [Releases](https://github.com/recallrw80-afk/MLC/releases/latest), put it next to `install.sh`, and run `bash install.sh mlc-linux-<arch>.tar.xz`.

### Build from source

For hacking/custom builds. Note: locally built packages do not embed a CF key (CurseForge falls back to the MCIM mirror); use the official package for the full experience.

```bash
git clone https://github.com/recallrw80-afk/MLC.git
cd MLC/cpp
make install        # build Release → zero-dependency package → install to ~/.local
```

Requirements: Qt 6.11+ (Core/Network), CMake 3.16+, Ninja, a C++20 compiler, nlohmann-json 3.11+, ZLIB. Qt prefix defaults to `$HOME/Qt/6.11.1/gcc_64`; override with `make install QT_PREFIX=/path/to/Qt/6.x/gcc_64`.

To install a package built on another machine: `bash cpp/cli/install.sh mlc-linux-<arch>.tar.xz`.

### Update & uninstall

```bash
mlc update         # check GitHub Releases and update in place (install.sh-installed copies only)
mlc uninstall      # uninstall (clears the game folder)
mlc uninstall -r   # uninstall but keep game folder contents
```

### Common build targets

```bash
cd cpp              # the C++ project lives in cpp/ (the Rust main line builds from the repo root)
make cli            # build mlc + libmlccore.so (Debug)
make package-tar    # zero-dependency tarball at cpp/cli/dist/mlc-linux-<arch>.tar.xz
make run            # build & launch the QML GUI (dev on hold; source-only, binary name mlc-gui)
```

## Quick start

```bash
# Import a modpack (game folder defaults to ./mc/ next to the binary; change with set-folder)
mlc inpack ~/Downloads/some-modpack.zip

# List instances and pick one interactively to launch
mlc launch

# Or play vanilla
mlc mc-install        # latest release
mlc launch 1.20.1
```

See [cpp/cli/README.md](cpp/cli/README.md) for the full command reference. Every command accepts `-h` for detailed parameter help (e.g. `mlc inpack -h`); `mlc help` lists all commands.

## Docs

- [cpp/cli/README.md](cpp/cli/README.md) — CLI command reference, directory layout, FAQ
- [CONTRIBUTING.md](CONTRIBUTING.md) — development conventions and contribution guide

## Contributing

Issues and PRs are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting.

## Disclaimer

- This project is not an official Minecraft product and is not associated with, approved by, or endorsed by Mojang Studios / Microsoft. "Minecraft" is a trademark of Mojang Synergies AB.
- This project has no affiliation with or authorization from the official Plain Craft Launcher (PCL).
- This software is provided "as is"; the authors accept no liability for damages arising from its use (see sections 15–16 of the license).

## Credits

- [Plain Craft Launcher (PCL)](https://github.com/Hex-Dragon/PCL2) — implementation ideas reference
- [PCL Community Edition](https://github.com/PCL-Community/PCL2-CE) — selected solutions reference (e.g. the MCIM mirror)
- Qt, nlohmann-json, zlib, Adoptium, BMCLAPI/MCIM mirrors, and other upstream projects

## License

[GNU General Public License v3.0](LICENSE)

Copyright (C) 2026 MLC authors. This program is free software: you may redistribute and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3. Derivative works must be open-sourced under the same license.
