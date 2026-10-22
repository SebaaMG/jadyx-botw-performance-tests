# Experiment history, from the initial baseline through .34

This archive includes **all 25 retained source patches**, reconstructed as Git
commits, including the discarded .18–.21 experiments. The original upstream
history remains the parent history. Source exports retain the original SeriesX
commit subject and author date where an exact retained patch revision exists;
their commit messages record its original hash. They have new Git hashes because
they export only Jadyx, not the unrelated contents of the private repository.
The final .29/.30 work was still uncommitted there and is identified as such.

Check out `botw-test-1.0.0.N` for a source checkpoint. Several diagnostic versions
share source and differ in boot options, profiling, manifest version, or PGO
stage. Those tags do not claim identical executable bytes. Use the records in
[history/index.json](history/index.json) for commit/tree IDs, patch hashes,
dependencies, and the 33 aggregate historical evidence files.

| Diagnostic label | Retained source | What it tested / observed |
|---|---|---|
| .0 | Upstream 0.2.93.3, `0e5b8533b3` | Initial Windows diagnostic registration and BOTW; early memory failure. |
| .1 | Upstream 0.2.93.14, `068d86bc21` | Updated original control; memory failure still reproduced on the PC. |
| .2 | 0001 | GC scan/eviction budget and allocation diagnostics; did not resolve the early OOM. |
| .3 | +0002 | Compute BC1 array uploads from compressed source; large expanded staging avoided, other pressure remained. |
| .4 | +0003 | Separate resident/committed memory accounting; clarified why low working set did not exclude allocation failure. |
| .5 | +0004 | Higher PC process setting with the original logical cache budget; inventory and scope became functional in this contrast. |
| .6 | +0005–0006 | Descriptor table reuse and JIT page/FP comparison changes. |
| .7 | Same as .6 | Control with those options disabled. |
| .8 | Same as .6 | Repeated candidate comparison. |
| .9 | Same as .6 | Detailed profiling; diagnostic measurements kept separate from normal FPS. |
| .10 | +0007 | Persistent resources and buffer heaps; functional, but regressed in gameplay. |
| .11 | +0008–0009 | Cache lifetime corrections and selected upstream graphics fixes; partial recovery, not a return to the best performance. |
| .12 | No separately verified source record retained | Intermediate label appears in packaging tooling; no source tag or measured run is fabricated for it. |
| .13 | +0010 | Ordered copy reuse, copy plans, coherent uniforms, and native host optimization support. |
| .14 | Same as .13 | Intermediate preparation/diagnostic package; do not confuse it with the later .16 PGO training run. |
| .15 | +0011 | Incremental uniform bindings; GPU correctness passed, no useful net BOTW gain established. |
| .16 | Same as .15 | Native PGO training; intentionally slow instrumentation, not a performance candidate. |
| .17 | Same as .15 | PGO USE; became the retained performance reference. |
| .18 | .17 +0012 | Packet batching, partial texture refresh and binding dependency work; not retained on the current line. |
| .19 | .18 +0013 | Fixed overly aggressive pool recovery; churn reduced without a useful total frame-time improvement. |
| .20 | .17 +0014 | Free GPR moves/R13 JIT experiment; no material BOTW gain established. |
| .21 | .17 +0015 | Ordered reads and inline exclusives; no useful FPS gain established. |
| .22 | .17 +0016–0017 | Boost version selection correction and detailed JIT symbol/IR/code diagnosis. |
| .23 | +0018 | Zero-upper tracking, stable JIT frame and unwind; shorter sequences and synthetic gains did not establish a gameplay benefit. |
| .24 | +0019 | Reusable DrawPlan; high hit rate, no FPS gain in its same-executable comparison, left off. |
| .25 | +0020 | Pool and buffer pressure corrections; eliminated measured transfer-pool recreations. |
| .26 | +0021 | Readback reuse, copies, GC batches and guest memory recovery; observed mean 42.91 → 40.37 ms in its comparison. |
| .27 | +0022 | Frame-chain diagnosis linked guest waits to late native GPU submissions. |
| .28 | +0023 | Partial native submissions; observed 42.58 → 38.78 ms in its comparison. |
| .29 | +0024 | Continuous GPU supply and separately controlled fence-flush coalescing; combination reached the 30-FPS cap in this view. |
| .30 | +0025 | Event contracts, consumer completion and callback handling; same-mod ON–OFF–ON evidence in the current report. |

The main tested line after .17 uses **0001–0011, then 0016–0025**. Patches 0012
and 0013 form a side experiment; 0014 and 0015 each branch directly from .17.
Their tags keep them accessible without silently enabling them in .30.

## Original experiment commits

The [.34 configuration follow-up](BOTW-RECORDER-34.md) reuses the exact tested
.30 executable and its existing parallel recorder. It adds no source patch or
reconstructed source tag. Its paired configurations and numeric evidence are
published separately from the 33-file historical archive above. The unaccepted
.31–.33 experiments are not included in this public source checkpoint.

The [original commit timeline](history/COMMITS.md) keeps the SeriesX subjects,
dates and hashes together, with links to the corresponding public source
checkpoints. Result-only milestones such as the .17 PGO comparison are included
there too. The original private repository itself is not copied or published.

Results across different historical sessions are not a single deterministic
benchmark. Different scene state, host load, warming, profiling, and memory
settings affect interpretation. See each evidence file for its scope and
recorded limitations. Earlier pending statuses are preserved as historical
observations and may be superseded by later evidence. The latest summary is
[BOTW-PERFORMANCE.md](BOTW-PERFORMANCE.md).

The JSON archive removes personal filesystem paths and identifiers. Its index
records the original evidence checksum and the public copy's checksum, plus the
fields redacted. No private conversation, Game Vault recovery source, game
collection, firmware, keys, build artifacts, full game logs or generated PGO
database is published.
