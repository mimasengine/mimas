# Local SaturnRingLib patches

`SaturnRingLib/` is a submodule of **upstream `ReyeMe/SaturnRingLib`** (no push
access), so these small but **load-bearing** local edits cannot be committed to
the submodule and pinned — a local-only SHA would break fresh clones. They are
captured here instead and must be **re-applied whenever the submodule is reset
or re-initialized** (`git submodule update --force`, a fresh clone, etc.).

## Re-apply

```sh
cd SaturnRingLib
git apply ../patches/saturnringlib.patch     # check first: git apply --check ...
```

If `--check` reports the patch is already applied, the tree is up to date.

## What's in the patch

- **`saturnringlib/srl_core.hpp`** — gate the boot-time
  `SRL::Sound::Hardware::Initialize()` behind `SAT_DEFER_SOUND_INIT` (defined in
  our `Makefile`). Its `CDC_CdInit` hangs for MINUTES under Ymir at boot, with
  the TV blanked (→ black screen, no overlay). We defer the SGL sound init to
  `I_InitSound`, run only when CDDA is the active backend. **Without this patch
  the `-DSAT_DEFER_SOUND_INIT` flag does nothing and slow boot returns.** See the
  `fix(boot): defer SGL sound init out of Core::Initialize` commit.
- **`saturnringlib/shared.mk`** — drop a stray `@` in the `CONVERT_AUDIO_TO_RAW`
  shell function (a shell-syntax fix in SRL's audio-to-raw helper).

## Refresh this file after changing the submodule

```sh
(cd SaturnRingLib && git diff saturnringlib/srl_core.hpp saturnringlib/shared.mk) \
  > patches/saturnringlib.patch
```
