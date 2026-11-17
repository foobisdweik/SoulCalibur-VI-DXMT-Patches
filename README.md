# SoulCalibur VI DXMT Patches

This repository holds all the work on running SoulCalibur VI (SC6) well on Apple silicon Macs through CrossOver Preview. It combines two earlier repositories, with their full histories:

- **SC6-VRR** (the files at the top level): the `dinput8.dll` hook DLL, test and profiling tools, configuration, notes and specifications.
- **dxmt-SC6** (in `dxmt/`): the SC6 fork of DXMT, which is based on Mullion. DXMT translates Direct3D 11 to Metal.

## Layout

| Path | Contents | In git |
| --- | --- | --- |
| `dll/` | The hook DLL (`dinput8.dll`), its `sc6vrr.ini` and the scripts that install them | yes |
| `tools/` | Profiling, cache warming, Game Mode, prewarm and `dxmt-swap.sh` (installs a staged DXMT build) | yes |
| `tests/` | The test harness (`tests/run.sh`) and shader conversion tests. Run output goes to `tests/runs/` | yes, except `tests/runs/` |
| `notes/`, `specs/`, `PLAN.md`, `handoff.md` | Findings, the renderer's frame structure and the work plan | yes |
| `config/`, `mods/`, `re/` | Engine settings, mod pak builds and reverse-engineering notes | yes, except built and machine-local files |
| `dxmt/` | The DXMT fork. Build folder: `dxmt/build-mm` | yes, except build output |
| `toolchains/` | LLVM, llvm-mingw and Wine toolchains for the DXMT build (about 5.5 GB). `dxmt/toolchains` links here | no; `deps.lock` pins them |
| `runtimes/` | Other Wine builds to test DXMT in, such as MetalSharp-Wine | no; `deps.lock` pins them |
| `reference/crossover-sources/` | The source code of the installed CrossOver Preview, for reading only | no |
| `data/` | Links to folders outside the repository: `bottle`, `game`, `sc6vrr-shaders` (the shader dump) and `app-support` (staged builds, backups and logs) | no |

The game, the CrossOver bottle and the data folder stay where CrossOver and the hook DLL expect them. The links in `data/` only make them easy to reach.

## Dependencies

The repository holds each dependency in one of three ways, by how the work uses it:

| How it is used | How it is held | Dependencies |
| --- | --- | --- |
| Changed here and shipped | In the repository with its history, as `dxmt/`. Upstream changes come in by merge (see below) | Mullion, and through it DXMT |
| Used unchanged and small | A git submodule, which pins one commit of another repository | `third_party/minhook`, `SC6-docs`, `dxmt/external/nvapi`, `dxmt/include/native/directx` |
| Large toolchains and Wine builds | Not in git. `deps.lock` pins each one's version and SHA-256 hash, and `tools/fetch-deps.sh` downloads it and checks the hash | llvm-mingw, Wine for ARM64EC, LLVM 15, MetalSharp-Wine, CrossOver Preview sources |

To get the submodules and the toolchains that the DXMT build needs after a fresh clone:

```bash
git submodule update --init --recursive
```

```bash
tools/fetch-deps.sh
```

`tools/fetch-deps.sh --check all` reports the state of every dependency without downloading anything. To fetch one that is not in the `build` group, give its name, for example `tools/fetch-deps.sh metalsharp-wine`. To move to a new version, change its line in `deps.lock`, then run the script with `--force`. The script moves the old folder aside, and you delete it when the new version works.

## Build DXMT

`tools/build-arm64ec.sh` makes a clean build in `dxmt/build-arm64ec` with the same options as upstream CI. It fetches missing toolchains first, and it builds LLVM 15 for arm64 if `toolchains/llvm-darwin-arm64` does not exist yet (about 20–30 minutes).

For day-to-day work, rebuild the existing `dxmt/build-mm` folder. It needs the llvm-mingw tools on `PATH`:

```bash
cd dxmt
PATH=$PATH:$PWD/toolchains/llvm-mingw-20260908-ucrt-macos-universal/bin ninja -C build-mm
```

To install a build in CrossOver Preview, stage it under `data/app-support/builds/<name>/` with a `SHA1SUMS` file, quit the game, and run:

```bash
tools/dxmt-swap.sh <name>
```

## Branches and remotes

- `main` is the working branch. It was SC6-VRR's `native-metal-capture` branch.
- `archive/vrr/*` and `archive/dxmt/*` keep the other branches of the two old repositories. `refs/archive/dxmt/stash` keeps the fork's old stash.
- Remotes:
  - `origin` (`foobisdweik/SoulCalibur-VI-DXMT-Patches`): this repository.
  - `mullion` (`dttdrv/mullion`): the upstream project.
  - `sc6vrr` (`foobisdweik/SC6-VRR`) and `dxmt-sc6` (`foobisdweik/dxmt-SC6`): the two old repositories. They are archived on GitHub, so they are read-only.
- To merge upstream Mullion into `dxmt/`:

```bash
git fetch mullion
```

```bash
git merge -X subtree=dxmt mullion/main
```

Upstream DXMT's `AGENTS.md` does not accept AI-generated contributions, so changes from this repository do not go upstream.

## Old paths

`~/Developer/SC6-VRR`, `~/Developer/dxmt-SC6` and `~/Developer/crossover-sources` are links to the new places, so old notes and commands still work. Mirror backups of the old repositories from before the move are in `data/app-support/backups/git-20261114/`.
