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

## Performance work — status 2026-10-06

The green CI run on commit `57793ff` did **not** build MGLRU: the build
configuration omitted `CONFIG_LRU_GEN`, so `olddefconfig` selected `n`. Do not
use that artifact to assess MGLRU or phone smoothness.

The previous worktree had only the page-list groundwork and did not enable it.
The current worktree now wires generation aging into kswapd, generation-based
eviction into reclaim, and the reclaim feedback/refault accounting needed to
consume those lists. It adapts the upstream minimal implementation to this
4.9/OPPO tree's node-level LRU lock and older `shrink_page_list()` and
`mem_cgroup_lruvec()` APIs. It also fixes memcg LRU size accounting, enables
`CONFIG_LRU_GEN` in the RMX1971 defconfig, and makes the build workflow fail if
either produced variant silently drops that option. The SLMK workflow variant
also asserts that Simple LMK remains enabled with its existing 128 MB target.

This is still unbuilt and unbooted. The separate working-tree fixes reserve
page-flag bits for MGLRU tiers and repair two activation-time source errors
(duplicate statistics fields and a malformed generation-loop macro); those
edits depend on the `kernel/bounds.c` width definition. CI compilation is the
next gate. The resulting image is not a phone candidate until it compiles,
boots, and survives the device checks below.

### SLMK facts to use when tuning

- `CONFIG_ANDROID_SIMPLE_LMK_MINFREE=128` is a **target amount of process
  memory to free after reclaim triggers**, not a 128 MB free-memory floor.
- Simple LMK is woken by sustained `vmpressure == 100`: three consecutive
  notifications, or pressure remaining at 100 beyond its 2 s window.
- The existing defaults include a 90 s boot grace period. They do not prove
  that Simple LMK killed TikTok; Android userspace `lmkd` may also kill it.
- Do not lower the 128 MB target or change the pressure debounce by guesswork.
  A lower target can reduce kills while extending foreground reclaim stalls.
  Capture the actual kill source and reclaim counters under the TikTok plus
  background-app workload first.

### Historical on-device baseline from the OpenCode “Greeting” session

These are the old session's readings, not measurements taken during this
continuation. Device reported as RMX1971, Android 16, Mystralis/Ascension
4.9.337. `MemTotal` was 3,692,824 kB. ZRAM disk size was
2,684,354,560 bytes (2.5 GiB); swap was 2,621,436 kB with 2,580,576 kB used
(about 98.4%). `MemFree` was 46,772 kB, `MemAvailable` 341,508 kB, and
`swappiness` 160. CMA was 204,800 kB total with only 216–232 kB free in the
baseline samples.

A cold Settings launch reported `WaitTime: 3136 ms`. The sampled launch window
had CPU counter deltas of user +372, nice +107, system +568, idle +1275 and
iowait +172 ticks. This is meaningful CPU work, not “almost entirely idle”;
the five listed counters alone yield about 42% busy. The sampling windows were not perfectly
aligned and other CPU counters were omitted, so treat this as approximate.
Over the launch window, `pgscan_direct` rose by 217,623 pages (about 850 MiB
at 4 KiB/page), `pgscan_kswapd` by 14,330 pages (about 56 MiB),
`compact_stall` by 5,236 and `pgmajfault` by 28,348. At +2 seconds, cumulative
deltas were 358,018 direct-scan pages (about 1.37 GiB), 17,266 kswapd pages
(about 67 MiB), and 8,627 compact stalls; CMA free was 0. The direct reclaim
scan count was about 20.7 times the kswapd count over that longer sample.

At +2 s, policy0 was 1,708,800 kHz (its maximum), while policy6 was 825,600
kHz, well below its 2.3 GHz maximum; governor was schedutil. GPU frequency was
not captured for this launch. This trace supports investigating direct
reclaim/compaction during foreground launch, alongside real CPU work. It does
not establish that reclaim is the sole cause, that CPU frequency is
irrelevant, or that changing zram/CMA limits will help. Earlier swappiness
sweeps (100, 60, 30, 10) reportedly showed no meaningful `MemFree` delta;
`drop_caches` was also tried. Interpret that sweep cautiously: with
`CONFIG_OPLUS_MM_HACKS`, non-kswapd reclaim uses `direct_vm_swappiness`, while
the normal `vm.swappiness` sysctl is not the direct-reclaim knob. The vendor
reclaim path also avoids anon reclaim when free swap is at or below one sixty-
fourth of total swap. The recorded free swap was 40,860 kB, just below the
roughly 40,960 kB threshold for that 2.5 GiB swap area, so the direct reclaim
trace likely ran with anon reclaim suppressed. The MGLRU path must preserve
this existing vendor policy; a later tuning comparison should log both
swappiness values and free swap. Stopping `lmkd` caused a watchdog soft reboot
and must not be repeated. Historical Simple LMK kill counts varied by sample
and do not by themselves attribute TikTok's termination.

### Required sequence

1. The working tree now has aging, eviction, reclaim integration, page-flag
   layout and initialization fixes. Review the adaptation against Yu Zhao's
   minimal implementation and use the branch build as the next compile gate.
2. `CONFIG_LRU_GEN=y` is enabled in the RMX1971 defconfig. The workflow now
   checks the produced configs for `LRU_GEN=y` and the expected LMK settings
   before compiling both variants without publishing.
3. Keep the already working Mystralis kernel as the recovery image. Boot the
   candidate before any tuning, confirm Wi-Fi, camera, fingerprint, VPN/TUN,
   zram and app lifecycle, and collect kernel logs for MGLRU list/accounting
   warnings and OOM events.
4. Compare the same workload on baseline and candidate: leave the phone idle,
   unlock and measure first-frame latency; run TikTok with one background app;
   switch between them for several minutes. Capture process-kill attribution,
   `MemAvailable`, zram use, CMA, `pgscan_direct`, `pgscan_kswapd`, compaction,
   and CPU/GPU frequencies during the actual stalls.
5. Tune one variable at a time, starting with measured SLMK target/debounce
   behavior. Adjust CPU/GPU policy only if traces show the relevant hardware
   was not reaching expected frequencies. Keep MGLRU and SLMK enabled together
   for the primary comparison, with the non-SLMK build only as a diagnostic
   control.

No device was attached to ADB during the 2026-10-06 continuation, so these
historical readings were not refreshed or independently remeasured. No kernel
was flashed and no release was published.
