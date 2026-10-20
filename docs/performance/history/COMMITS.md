# Original experiment commits

Original SeriesX subjects and author dates are retained here. Public source exports
have new hashes because private project files are excluded. Multiple retained
source patches from one original commit remain separate checkpoints. Links marked
'later source' point to the retained revision, not the earlier WIP bytes.

| Date (UTC−03) | Original commit | Subject | Public record |
|---|---|---|---|
| 2026-10-18 03:45 | `4d7c09c` | Investigate BOTW upload OOM and compare emulator optimizations | [783eb5b](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/783eb5b4e6c12d010a634c76ba186989c12fa695) |
| 2026-10-18 05:30 | `dcf638b` | Record built GC candidate and two BOTW allocation failures | [Results](jadyx-gc-candidate-2026-10-18.json) |
| 2026-10-18 06:19 | `3d17fc5` | Decode large BC1 arrays on GPU and record remaining BOTW memory failures | [9391273](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/939127358e6a394aef35fedc42f13ee2b11a6677) |
| 2026-10-18 07:09 | `2a98918` | Diagnose PC memory accounting and confirm BOTW transitions with separate caps | [650a48d](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/650a48d9418a8c133ca530c3102a16c4e4acf2ca), [7dd1353](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/7dd13537eec4cd2ba4eb067711795ecd2604075f) |
| 2026-10-18 08:24 | `1b38f03` | Reduce graphics descriptor copies and add opt-in A64 JIT fast paths | [f729d78](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/f729d7824579b72e87c47c1fe583a9b9038005a0), [91c03f6](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/91c03f62a8418941664b8a7d86c2f48020e48aee) |
| 2026-10-18 17:42 | `f94ce49` | Record resolved GPU profile and prioritize persistent resource bindings | [Results](jadyx-profile-2026-10-18.json) |
| 2026-10-18 18:40 | `79fa7e2` | Add experimental persistent D3D12 resources and record gameplay regression | [e7fc55a](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/e7fc55a55cf8f73b1ee449cd11723cdc985083f8) |
| 2026-10-18 19:15 | `8cc948b` | Fix persistent descriptor cache costs and integrate upstream graphics copies | [ccd6b66](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/ccd6b66ca2ad1b2ca12cbbecac1ba09a61520eaa), [4a2dec3](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/4a2dec384d6e3f06d793179432be7683ab795f0f) |
| 2026-10-18 19:20 | `9f21c8b` | Disable unsuccessful persistent renderer experiment in active runtime | [Results](jadyx-renderer-control-2026-10-18.json) |
| 2026-10-18 19:37 | `08dc7f2` | docs(jadyx): reattribute transfer-buffer costs and revise optimization priorities | [Results](jadyx-transfer-pressure-2026-10-18.json) |
| 2026-10-18 21:00 | `52f61f6` | perf(jadyx): reuse ordered GPU scratch and coherent uniforms | [059e6e9](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/059e6e996496aa0e32ebbaf50a394fe0bb3cf84a) |
| 2026-10-18 21:36 | `39bd0bb` | perf(jadyx): add optional incremental uniform bindings and measured controls | [326b54e](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/326b54ef80f9816d158c4b83cc03b08665fe48ea) |
| 2026-10-18 22:58 | `ba7fe7e` | perf(jadyx): build native PGO candidate from BOTW gameplay counts | [Results](jadyx-pgo-native-2026-10-18.json) |
| 2026-10-18 23:08 | `2b6321d` | perf(jadyx): record PGO comparison and restore optimized runtime | [Results](jadyx-pgo-native-2026-10-18.json) |
| 2026-10-18 23:27 | `7dd15b2` | docs(jadyx): prioritize structural bottlenecks after native PGO | [Results](jadyx-research-after-pgo-2026-10-18.json) |
| 2026-10-19 00:51 | `e082a17` | perf(jadyx): preserve structural candidates and fix pressure churn | [303b114](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/303b11475fd197ec7eb59c67bb7a7ba4d5c5a4ee), [c2592d2](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/c2592d24497b8d8d0d64a20863e8889577cdb9c8) |
| 2026-10-19 01:09 | `ce4e46d` | docs(jadyx): correct ON/OFF inference after reviewing individual windows | [Results](jadyx-structural-candidate-2026-10-19.json) |
| 2026-10-19 01:29 | `8d76bf6` | docs(jadyx): record original .17 sampling and prioritize emitted JIT code | [Results](jadyx-baseline17-sampling-2026-10-19.json) |
| 2026-10-19 02:00 | `a0e26d3` | perf(jadyx): reduce JIT GPR spills and allocate R13 when available | [4722543](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/47225435d0ab14f41a6c229a6d8f2690c70cc600) |
| 2026-10-19 02:16 | `a952bdb` | docs(jadyx): record verified UWP .20 delivery and regression results | [Results](jadyx-jit-registers-2026-10-19.json) |
| 2026-10-19 02:25 | `c2bbde1` | docs(jadyx): record .20 ON/OFF results and restore .17 reference | [Results](jadyx-jit-registers-2026-10-19.json) |
| 2026-10-19 02:53 | `65533d7` | perf(jadyx): inline page-table atomics and lighten ordered loads | [later source](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/691b9e4584ac9a771887fac7df2f2530d801f451) |
| 2026-10-19 03:05 | `530334c` | fix(jadyx): handle minimal mirrored tables and verify UWP .21 | [691b9e4](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/691b9e4584ac9a771887fac7df2f2530d801f451) |
| 2026-10-19 10:28 | `fc4213d` | docs(jadyx): record .21 capture and same-exe control | [Results](jadyx-jit-memory-order-2026-10-19.json) |
| 2026-10-19 11:01 | `43ef6f9` | docs(jadyx): audit translation bottlenecks and close .21 comparison | [631bfcb](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/631bfcb98319c787f6fdbf34038bede14775ff5f) |
| 2026-10-19 12:05 | `6b4252c` | feat(jadyx): recover sampled JIT instructions and audit .22 bottlenecks | [8258500](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/82585004d2743378f416b76cff740fce2e7de3f3) |
| 2026-10-19 12:40 | `991fc22` | perf(jadyx): eliminate redundant W conversions and per-block frames | [de335c4](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/de335c41fdf09f83145b82f0b9a0c00b7256edd2) |
| 2026-10-19 12:45 | `3a77cd4` | test(jadyx): record .23 fixed-view capture and open same-exe control | [Results](jadyx-jit-frame-2026-10-19.json) |
| 2026-10-19 12:51 | `c28d74e` | test(jadyx): record .23 control and prepare independent option captures | [Results](jadyx-jit-frame-2026-10-19.json) |
| 2026-10-19 12:58 | `a262202` | test(jadyx): record isolated zero-upper capture and memory-state confounds | [Results](jadyx-jit-frame-2026-10-19.json) |
| 2026-10-19 13:00 | `34b848f` | docs(jadyx): preserve frame-only activation and pending capture | [Results](jadyx-jit-frame-2026-10-19.json) |
| 2026-10-19 13:24 | `7db7445` | test(jadyx): complete four-way comparison and recover window percentiles | [Results](jadyx-jit-frame-2026-10-19.json) |
| 2026-10-19 21:50 | `5cd5201` | feat(jadyx): add experimental draw plans and record comparison | [4235416](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/42354163f56048582e6787e8f74fc9f4e6547ea7) |
| 2026-10-20 00:12 | `4a41151` | fix(jadyx): correct buffer pressure and record .25 measurements | [f03869b](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/f03869b4b6688e95a0419e7b5a9b8d7d42ee74ed) |
| 2026-10-20 01:27 | `1ff0fe9` | feat(jadyx): recover readback reuse and record .26 comparison | [31a415f](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/31a415f73976be45dd3d69d7171cffbdf89ffc0a) |
| 2026-10-20 02:07 | `44a2bf2` | feat(jadyx): trace frame dependencies and record .27 findings | [cfa104e](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/cfa104e112d7fc7c92335d0a6827167a5e527b3f) |
| 2026-10-20 03:08 | `4aad176` | feat(jadyx): add partial GPU submissions and expansion plan | [f85a412](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/f85a4122848d10fae9a324365dd5b1fa574996ad) |

The final .29 revision and .30 were still uncommitted in SeriesX at export time.
Their public commits preserve the verified current state:

- .29: [ceae712](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/ceae7120aa141a8598408bebe1436545eb126ebe).
- .30: [8734425](https://github.com/SebaaMG/jadyx-botw-performance-tests/commit/87344250c95410d8f3813e23c882788e60814b38).
