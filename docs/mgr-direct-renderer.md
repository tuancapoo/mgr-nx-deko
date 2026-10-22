# MGR direct renderer, 2026-10-16

The previous MGR build inherited the direct-state/flush cvars, but its Draw hooks
only called NoteDrawCall. It never entered BeginDrawDirect, so both counters were
zero. This change connects MGR's own entry points to the existing record transport.
It does not link SA2's native Direct3D functions or assume their guest addresses.

## Instruction evidence

The decompressed MGR image is `out/default_image.bin`, base 0x82000000.
The extracted generated listings are kept in
`out/experiments/direct_mgr_20261016/*.cpp.txt`.

| MGR function | Meaning | Verified layout |
| --- | --- | --- |
| 0x826BC928 | DrawVertices | device+48 last word, +52 hard limit, +56 soft limit |
| 0x826BCD40 | DrawIndexedVertices | same fields; shared dirty shadows/flush calls |
| 0x826CA7E8 | Constant flush | 16 words per MSB-first dirty bit, r5 register, r6 shadow, 16-byte payload alignment |
| 0x826CA450 | Register groups | one word per MSB-first dirty bit, r5 register, r6 shadow |
| 0x826CA690 | Fetch flush | six words per dirty bit, registers 0x4800, shadow device+1152 |

Fetch's fast tail stores 0x0002500000000000 twice. Decoding the first 0x25000
as a type-0 header yields register 0x5000 with values {0, 0x25000, 0}, exactly
the shared implementation. Do not confuse 0x826CA510 with the constant helper:
it has a different four-word-per-bit layout and extra packets; it stays original.

Both draw hooks start a direct scope, note the shader pairing in that scope,
call the original draw and end the scope. The three flush hooks use
FlushRunsDirect only inside a supported scope; all other calls and slow paths
run their original implementations. The full five guest function hashes are
checked before first use; a mismatch keeps the original PM4 path.

`tools/call_site_liveness.py` audited all direct calls to the flush helpers:
8 constant, 56 group and 4 fetch sites, with no volatile register read before
redefinition after a call. Native fast paths need not reproduce scratch clobbers.

The existing transport retains submission/segment guards, marker publication,
fallbacks for unsupported packets/full queues, original-packet self-checks and
record-loss detection. Native Clear/Resolve and surface-based EDRAM elimination
are not implemented here.

## Vertex uploads

`mgr_native_vertex_exact_reuse` checks address/size/endian/slot/synchronization
metadata first. Only a candidate reads memory, comparing every endian-converted
guest byte against the existing upload. A metadata miss does not compute a
fingerprint before copying. A mismatch is copied again. The path requires a
CPU-cached upload buffer and synchronous copies; asynchronous/uncached paths
keep their previous behavior. Repacked and rectangle-list inputs remain excluded.

Tests cover all four endian modes, 16 alignments, vector tails and large ranges,
mutations at the first/middle/last byte, and metadata/epoch guards. This changes
neither texture quality nor vertex contents. It does not trust sampled hashes.

## Verification and hardware measurement

MGR now registers `sa2_cpu_verify` itself: SA2's original declaration lives in a
module MGR does not link. Previously the renderer could only run its initial
4096 checks even when the command line requested continuous verification.

The final validation report and captures are in
`out/experiments/direct_mgr_20261016`. Continuous verification compares each
native flush's canonical operations with the original on a copied device, and
each converted record with the original packets. Verify mode is diagnostic and
must stay off for Switch performance measurements.

Use CPU 1785 MHz with identical GPU/RAM clocks, 360p, the same fight/camera and
warmed caches. Compare all-on against `sa2_cpu_direct_state=false`,
`sa2_cpu_direct_flush=false`, `mgr_native_vertex_exact_reuse=false` in the same
NRO. A state-only control keeps direct_state on and direct_flush off. Keep
mgr_fps_limit=0 while measuring throughput, and test a 30 cap separately with
gameplay speed verified. Stable 30 FPS and a fix for the Sundowner chase are not
certified by title-screen PC probes. PGO follows after code and gameplay stabilize.
