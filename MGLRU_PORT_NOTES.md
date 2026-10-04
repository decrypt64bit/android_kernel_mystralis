# MGLRU backport — WIP status on branch `mystralis-mglru`

Base: `mystralis` @ c9c7eb1ba (branch created off it).
Source series: Yu Zhao's official android-4.19 MGLRU backport, mirrored in
DeckAndroid/android_kernel_valve_jupiter:

  ce62ece  UPSTREAM: Revert "include/linux/mm_inline.h: fold __update_lru_size()
           into its sole caller"        -> NOT NEEDED, tree is already post-revert
  f4d4c46  BACKPORT: mm: multi-gen LRU: groundwork
  53af55e  BACKPORT: mm: multi-gen LRU: minimal implementation

Local copies: mglru_1_groundwork.patch, mglru_jupiter.patch (patch 0 not needed).

## Why it does not apply cleanly
This is a Qualcomm/OPPO-vendored 4.9 whose MM is a partial 4.19 backport:
 - has `struct lruvec` + `enum lru_list` (4.19-era)  -> GOOD for MGLRU
 - has NO `LRU_REFS` and NO `struct workingset_struct` (pre-5.3) -> needs the
   older backport (correctly chosen)
 - `include/linux/page-flags-layout.h` is an OLD layout (uses `NODES_SHIFT`,
   no `KASAN_TAG_WIDTH` / `LAST_CPUPID_WIDTH`) vs jupiter's 4.19 layout
 - `mm/vmscan.c`, `mm/swap.c`, `mm/rmap.c` carry OPPO patches (MGLRU touches all)

## DONE
- [x] branch `mystralis-mglru` created off `mystralis`
- [x] groundwork: `kernel/bounds.c` applied cleanly (LRU_GEN_WIDTH)
- [x] groundwork: `mm/memcontrol.c`, `mm/mmzone.c` applied (partial hunks)
- [x] `include/linux/page-flags-layout.h` resolved by hand
      (LRU_GEN_WIDTH added to both space checks + `#define LRU_REFS_WIDTH 0`)
- [x] `include/linux/mmzone.h` resolved by hand
      (MIN/MAX_NR_GENS, LRU_GEN/REFS masks, struct lru_gen_struct,
       init fns + !CONFIG_LRU_GEN stubs, `lrugen` added to struct lruvec)

## REMAINING (13 reject files)
  include/linux/mm_inline.h   (214 lines)  biggest, core lru add/del wrappers
  include/linux/mm.h          (10)
  include/linux/page-flags.h  (19)         page_lru_gen()
  include/linux/sched.h       (12)         task_struct usage counters
  mm/vmscan.c                 (83)         OPPUS-modified, hardest
  mm/memory.c                 (47)
  mm/swap.c                   (32)         OPPUS-modified
  mm/mm_init.c                (20)
  mm/Kconfig                  (16)         LRU_GEN / LRU_GEN_STATS
  fs/fuse/dev.c               (11)
  mm/huge_memory.c            (11)
  mm/memcontrol.c             (9)
then apply mglru_jupiter.patch (8 files, own rejects), and add
`CONFIG_LRU_GEN=y` to arch/arm64/configs/sdm670-perf_defconfig.

## Compile gate
No local kernel toolchain (build >> 30s command cap), so CI is the gate.
`gh` is authed as `decrypt64bit` with repo+workflow scopes.
`.github/workflows/build.yml` triggers on `workflow_dispatch` ONLY, so pushing
this branch cannot trigger a release build.

Loop: commit -> `gh workflow run build.yml --ref mystralis-mglru`
      -> `gh run watch` -> read compile errors -> fix -> repeat.

## RISK
Never flash this until it builds AND boots. A subtle LRU bug corrupts the page
cache / can hang or OOM at boot. Keep `mystralis` clean; this branch is
experimental and must not be merged or released as-is.