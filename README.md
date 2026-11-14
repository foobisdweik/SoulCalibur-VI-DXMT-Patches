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
| `toolchains/` | LLVM, llvm-mingw and Wine toolchains for the DXMT build (about 5.5 GB). `dxmt/toolchains` links here | no |
| `reference/crossover-sources/` | The source code of the installed CrossOver Preview, for reading only | no |
| `data/` | Links to folders outside the repository: `bottle`, `game`, `sc6vrr-shaders` (the shader dump) and `app-support` (staged builds, backups and logs) | no |

The game, the CrossOver bottle and the data folder stay where CrossOver and the hook DLL expect them. The links in `data/` only make them easy to reach.

## Build DXMT

The build needs the llvm-mingw tools on `PATH`:

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
- Remotes: `sc6vrr` (`foobisdweik/SC6-VRR`), `dxmt-sc6` (`foobisdweik/dxmt-SC6`) and `mullion` (`dttdrv/mullion`, the upstream project). Do not push this combined history to the old remotes without a decision first.
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
