# mlc — A Minecraft Launcher for Linux

**English** | [简体中文](README.zh-CN.md)

**MinecraftLauncherCLI (MLC)** is an independently developed Minecraft launcher written in C++/Qt, built with reference to the public implementation ideas of [Plain Craft Launcher (PCL)](https://github.com/Hex-Dragon/PCL2). `mlc` is its command-line frontend: no graphical interface needed, just a few MB of memory, with support for modpack import, multi-version management, and game launching.

## Features

- **Modpack import**: CurseForge / Modrinth / MCBBS / MultiMC / HMCL / launcher-shell packs / compressed `.minecraft` / plain mod packs — game files, modloaders (Forge/NeoForge/Fabric), and mods downloaded automatically
- **Multi-instance management**: isolated directory per instance; batch removal supported
- **Vanilla download**: one-command install of any MC version, with verify-and-repair
- **Java management**: detects system Java, picks per MC version automatically, downloads from Adoptium when missing
- **Player profiles**: multiple offline player profiles with skin types (slim/wide)
- **External login**: authlib-injector accounts (e.g. LittleSkin), session encrypted & persisted, auto-refreshed at launch
- **Local server hosting**: one-command install & foreground start for vanilla/Forge/Fabric/NeoForge servers, console attached, mods/config copyable from instances
- **Resume & verify**: every download is SHA1-verified; re-running skips existing files
- **Chinese/English UI**: switch with `set-lang zh`

## Requirements

- Linux (x86_64 / aarch64)
- A Java runtime to play (don't worry — it's downloaded automatically on launch if missing)

## Install

### One-liner (recommended)

Downloads the official prebuilt package — **the full experience** (CI-built, with an embedded CurseForge key so mod downloads use the official API):

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/latest/download/install.sh | bash
```

**In China**: the source falls back automatically — GitHub first, Gitee mirror on failure (rate limit/timeout). No flags needed:

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/latest/download/install.sh | bash
```

For **betas** (add `--beta`; same auto-fallback):

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/download/v0.1.3-beta/install.sh | bash -s -- --beta
```

> The Gitee mirror ships prebuilt binaries only (no source archives, no source builds). To build from source, clone from GitHub.

To install the latest **pre-release** (beta): `latest` never points at pre-releases, so fetch install.sh from the tag URL and pass `--beta`:

```bash
curl -fsSL https://github.com/recallrw80-afk/MLC/releases/download/v0.1.4-rc/install.sh | bash -s -- --beta
```

Prebuilt packages for x86_64 (amd64) and macOS (Apple Silicon); Linux aarch64 users build from source (Qt has no official 6.11 Linux ARM64 packages). Then run from anywhere:

```bash
mlc help
```

### Manual prebuilt install

Download `mlc-linux-<arch>.tar.xz` from [Releases](https://github.com/recallrw80-afk/MLC/releases/latest), put it next to `install.sh`, then:

```bash
bash install.sh mlc-linux-x86_64.tar.xz
```

### Build from source

For hacking/custom builds. **Note**: locally built packages do not embed a CF key (CurseForge falls back to the MCIM mirror — works, but not the best path); for the full experience use the official package above.

```bash
git clone https://github.com/recallrw80-afk/MLC.git
cd MLC
make install        # build Release → zero-dependency package → install
```

Requirements: Qt 6.11+ (Core/Network), CMake 3.16+, Ninja, a C++20 compiler, nlohmann-json 3.11+, ZLIB. Qt prefix defaults to `$HOME/Qt/6.11.1/gcc_64`; override with `make install QT_PREFIX=/path/to/Qt/6.x/gcc_64`.

To install a package built on another machine: `make package-tar` there, copy `cli/dist/mlc-linux-<arch>.tar.xz` over, then `bash cli/install.sh mlc-linux-<arch>.tar.xz`.

### Update & uninstall

```bash
mlc update         # check GitHub Releases and update in place (install.sh-installed copies only)
mlc uninstall      # uninstall (clears the game folder)
mlc uninstall -r   # uninstall but keep game folder contents
```

### Common build targets

```bash
make cli            # build mlc + libmlccore.so (Debug)
make package-tar    # zero-dependency tarball at cli/dist/mlc-linux-<arch>.tar.xz
make run            # build & launch the QML GUI (dev on hold; source-only, binary name mlc-gui)
```

## Quick start

```bash
# 1. Set the game folder (defaults to ./mc/ next to the binary; optional)
mlc set-folder /home/yourname/mc

# 2. Import a modpack
mlc inpack ~/Downloads/some-modpack.zip

# 3. See what instances you have
mlc list

# 4. Launch!
mlc launch
# or pick directly:
mlc launch <instance-name>
```

No modpack? Install vanilla first:

```bash
mlc mc-install          # latest release
mlc mc-install 1.20.1   # specific version
mlc launch 1.20.1
```

## Local server hosting

No GUI needed — two commands to a joinable server.

### 1. Install a server

```bash
# Vanilla
mlc server-install 1.20.1

# Loaders (Forge / Fabric / NeoForge) — bare flag picks the latest loader version, or pin one
mlc server-install 1.20.1 --forge
mlc server-install 1.20.1 --fabric 0.16.9
```

Files land in `{game folder}/servers/<id>/`: the vanilla id is just the version (`1.20.1`); loader ids carry a suffix (`1.20.1-forge-47.3.0`).

To bring the modpack you play onto the server, add `--from <instance>` — this copies the instance's `mods/`, `config/`, and `defaultconfigs/` into the server directory:

```bash
mlc server-install 1.20.1 --forge --from my-modpack
```

> **Note**: client-only mods (rendering/UI ones like Sodium) crash dedicated servers. If startup fails, remove them from the server's `mods/` first.

### 2. Start

```bash
mlc server-start 1.20.1-forge-47.3.0
```

- **First start** requires accepting the [Minecraft EULA](https://aka.ms/MinecraftEULA): answer the interactive prompt, or pass `--eula` (asked once, written to `eula.txt`)
- A minimal `server.properties` is created on first start (with `online-mode=false`, required for offline/authlib players to join; an existing file is never overwritten)
- The console is attached to your terminal: type `/stop`, `/op <name>`, `/whitelist ...` directly
- Java is auto-selected per version; memory defaults to 2G (change with `set-mem`)

### 3. Let others connect

- Same LAN: connect to `<your LAN IP>:25565`
- Public internet: a public address + router port forwarding (TCP 25565), or a tunnel (SakuraFrp / playit.gg / frp on a VPS)

### 4. Security note (important)

A public server with `online-mode=false` lets **anyone join with any name, including impersonating you**. Enable the whitelist right away:

```
whitelist on
whitelist add your-friend
```

## Examples

End-to-end command flows for four common scenarios.

### Scenario 1: Play a modpack

```bash
mlc set-folder ~/mc                          # set game folder (optional, defaults to ./mc/ next to the binary)
mlc inpack ~/Downloads/ATM9.zip              # import modpack (game files/Forge/mods auto-downloaded)
mlc list                                     # show instances
mlc mods "All the Mods 9"                    # list the instance's mods + enabled state
mlc launch "All the Mods 9"                  # launch (omit the name for an interactive picker)
```

### Scenario 2: Quick vanilla

```bash
mlc mc-install 1.20.1                        # download 1.20.1 (no arg = latest release)
mlc launch 1.20.1                            # launch
```

### Scenario 3: Host a server for friends

```bash
mlc server-install 1.20.1 --forge            # Forge server (drop --forge for vanilla)
mlc server-start 1.20.1-forge-47.3.0         # asks for the EULA on first run (or pass --eula)

# once the server is up, type console commands right into the terminal:
whitelist on                                  # enable whitelist (a must on public internet)
whitelist add Alex                            # allow a friend
op Alex                                       # grant operator
stop                                          # shut down
```

Friends add the server under "Multiplayer → Add Server" with `<your-ip>:25565`.

### Scenario 4: External login (LittleSkin etc.)

```bash
mlc login                                    # wizard: server address → email → password (LittleSkin by default)
mlc launch my-modpack                        # launches with the external account (auto-refreshed)
mlc config                                   # show current login state
mlc logout                                   # log out, back to the offline player
```

### Scenario 5: Maintenance

```bash
mlc update                                   # check for and apply mlc updates
mlc test                                     # full system self-check (run this first when things break)
mlc report crash on 1.20.1                   # prefilled issue link (env + logs attached, sanitized)
mlc uninstall -r                             # uninstall but keep game contents
```

## Command reference

Every command accepts `-h` (or `--help`) to print detailed parameter help, e.g. `mlc inpack -h`.

### Instances

| Command | Description |
|---|---|
| `list` | List all instances |
| `mods <name>` | List an instance's mods (file size + enabled state) |
| `list-rm [name\|*]` | Remove instances (interactive picker + confirmation without args; `*` removes all — quote it) |
| `mc-list` | List downloaded vanilla MC versions |
| `launch [name]` | Launch the game; without a name, an **arrow-key picker** appears (instances + vanilla/loader versions; falls back to numeric input on non-TTY) |

### Download & import

| Command | Description |
|---|---|
| `inpack <file> [--r <name>] [--to <instance>] [--folder <path>]` | Import a modpack; `--r` renames the instance; mod packs need `--to` for the target instance |
| `mc-install [version]` | Download a vanilla MC version (latest release without args) |
| `java-install <major>` | Download and install Java (Adoptium JRE) |
| `server-install [version] [--forge\|--fabric\|--neoforge [ver]] [--from inst]` | Install a server (latest release without args; bare loader flag = auto-latest; `--from` copies instance mods/config) |
| `server-start <id> [--eula]` | Run a local server in the foreground (console attached, `/stop` to halt; `--eula` accepts the EULA; id like `1.20.1` or `1.20.1-forge-47.4.10`) |

### Java

| Command | Description |
|---|---|
| `list-javas` | List detected Java installations |

### Players

| Command | Description |
|---|---|
| `player-add [name] [--avatar <path>] [--skin <slim\|wide\|default>]` | Add a player; interactive wizard without args (name → skin → avatar → advanced custom UUID) |
| `player-edit [uuid\|index]` | Edit a player (wizard, Enter keeps current value; interactive picker without args) |
| `player-rm [uuid\|index]` | Remove a player (interactive picker + confirmation without args) |
| `player-list` | List players (numbered, `*` marks the selected one) |
| `player-select <uuid\|index>` | Select the active player (index accepted; interactive picker without args) |

### Config & misc

| Command | Description |
|---|---|
| `set-folder <path>` | Set the default game folder |
| `set-lang <en\|zh>` | Set the UI language (English by default) |
| `set-mem <MB\|auto>` | Set max game memory; `auto` (default) allocates 50% of available RAM (capped at 16G) |
| `set-cf-key <key\|--clear>` | Set/clear a custom CurseForge API key (no arg shows the current source; overrides the embedded one) |
| `login [server] [email]` | External authlib-injector login (e.g. LittleSkin; token encrypted & persisted, auto-refreshed at launch) |
| `logout` | Log out the external account, falling back to the offline player |
| `update [-beta] [-cn]` | Check for updates and update in place (install.sh-installed copies only; `-beta` includes pre-releases, `-cn` uses the Gitee mirror) |
| `uninstall [-r]` | Uninstall the launcher; `-r` keeps game folder contents |
| `config` | Show current config |
| `report [description]` | Generate a prefilled GitHub Issue link (environment info + recent launch log attached, sanitized) |
| `test` | Full system self-check |
| `help` / `version` | Help / version |

## Where the game files live

By default in `mc/` next to the launcher (change with `set-folder`):

```
mc/
├── instances/      # each imported modpack instance (isolated)
├── versions/       # downloaded MC versions
├── libraries/      # game libraries (shared across instances)
├── assets/         # game assets (shared across instances)
├── javas/          # auto-downloaded Java runtimes
├── servers/        # local servers (server-install output; one dir per version/loader)
└── logs/           # launcher logs (mlc-launch-*.log, last 10 kept)
```

The game's own log is at `<instance>/logs/latest.log` (standard MC/mod location); each `launch` also writes a full session log (launch command line + all game output + exit code) to `mc/logs/mlc-launch-<timestamp>.log` — check that one for launch problems.

## FAQ

**Do I need an API key to import CurseForge modpacks?**
No. Official prebuilt packages embed a key (fastest, official API); source builds ship without one and automatically use the MCIM mirror. If a key expires or is revoked, requests automatically fall back to the mirror — nothing to handle manually.

**How do I switch the UI to Chinese?**
`mlc set-lang zh` (persistent, one-time).

**Is there a GUI?**
The QML GUI (`gui/`, binary `mlc-gui`) is **on hold** as of 2026-11-14 — focus is the CLI and SDK.

- **Not shipped**: `make package-tar` only packages the command-line `mlc`; prebuilt packages contain no `mlc-gui`, so the installer does not register that command.
- **Source still works**: developers can build and run it locally with `make run` (requires a full Qt 6.11 install).
- All code is retained. Resuming requires solving the GUI's zero-dependency distribution first (Qt platform plugins, QML modules, etc.) — see the "GUI 开发暂停" section in the repo-root `CLAUDE.md`.

For everyday use, stick to the CLI: run `mlc help` for all commands.

**What happens when an import fails?**
Any failed download stage (game files / modloader / mods) rolls back everything — no half-installed instance is left behind; just retry.

**Can I host a server (even a modded one) without a GUI?**
Yes — see "Local server hosting" above: `server-install` + `server-start`; add `--forge/--fabric/--neoforge` for loader servers, `--from` to copy an instance's mods.

## License & notice

Licensed under [GNU GPL v3](../LICENSE). This is independently developed open-source software: not affiliated with Mojang / Microsoft ("Minecraft" is a trademark of Mojang Synergies AB) and not affiliated with or authorized by the official [PCL](https://github.com/Hex-Dragon/PCL2). Provided "as is" — no liability accepted (see sections 15–16 of the license).
