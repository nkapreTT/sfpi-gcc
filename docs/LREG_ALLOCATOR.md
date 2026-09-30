# The SFPU LREG allocator (`rtl-rvtt-lp-alloc.cc`)

This document describes the pre-IRA SFPU vector-register (LREG)
allocator implemented in `rtl-rvtt-lp-alloc.cc` and
`rtl-rvtt-lp-alloc-color.cc`.  The SFPU has eight LREGs and no memory
spill path for them; the allocator makes the SFPU pseudo stream
colorable in eight registers before IRA, spilling through the Dst tile
file when it can prove the round trip exact.

## What it is

A Chaitin-style build/color/spill loop, run immediately before IRA
(after `pass_rvtt_dst_ownership`, `pass_rvtt_lp_schedule_prera` and
`pass_rvtt_lreg_livein`), under the default-off flag
`-mtt-tensix-optimize-lreg-alloc`:

1. **Trigger.** Function-wide peak simultaneous SFPU pressure (XTT32SI
   pseudos + SFPU hard regs, DF-simulated) is computed first.  Peak
   <= 8 is a no-op: nothing is built, nothing changes, and the
   compilation is byte-identical with the flag on or off
   (`lreg-alloc-within-file-bh.C`).

2. **Interference graph** over XTT32SI pseudo webs (web == pseudo; a
   deliberate, conservative approximation — no du-chain web splitting).
   Hard LREGs may appear only as raw-LREG reservation intervals (a
   recognized zero-length sentinel read, a bare USE endpoint, or a bare
   XTT32SI CLOBBER of the physical register), which
   `pass_rvtt_lreg_livein` emits for raw LLK register accesses; they
   become fixed-color nodes that are never spill candidates.
   Precolors come from the singleton-class constraints of the
   `rvtt_sfpreadlregN` / `rvtt_sfpwritelregN` metadata patterns;
   conflicting precolors on one web fail closed.  Interference is
   live-at-def over a backward DF simulation, with the move-source
   exemption.

3. **DSATUR coloring** over the 8 colors of the LREG file
   (deterministic tie-breaks: saturation desc, degree desc, node index
   asc).  Success = colorability certificate; the RTL is handed to IRA
   **unchanged** — assignment is deliberately delegated so IRA keeps
   its coalescing and untouched functions allocate exactly as without
   the pass.  Only the pinned-chain binding (item 6) assigns hard
   registers before IRA.

4. **Spill** when coloring blocks: the cheapest spillable web in the
   blocked neighborhood (cost = occurrences / (degree+1)) is spilled
   through a Dst scratch-row round trip — `SFPSTORE mod0 4 (INT32)`
   after each def, `SFPLOAD mod0 4` before each reading insn, fresh
   pseudo per insn, no-increment address mode (WH 3 / BH 7, the
   `rvtt_no_increment_address_mode ()` capability).  The INT32 pair is
   a bit-exact 32-bit round trip on both WH and BH (Dst rows are read
   and written through the `encode_fp32/decode_fp32` involution).
   FP32 (mod0 3) is not used as the spill format — the BH store
   flushes denormals; SM32 (mod0 12) is not used either — the WH store
   maps `0x80000000` to sign-magnitude zero.  (Kernel accesses carrying
   mod0 3 or 12 are still accepted into the 32-bit aliasing class —
   there is no refusal path for them; the spill simply always uses
   mod0 4.)

5. **Transactionality.** Every emitted insn and operand rewrite is
   logged; any refusal discovered after mutation rolls the stream back
   to the pre-allocation shape, so the over-pressure failure is
   reported on the original stream
   (`lreg-alloc-rows-exhausted-rollback-bh.C`).

6. **Dual-bank pinned chains.** When the function contains a
   relational pin site (an insn whose alternatives pin XTT32SI operands
   to exact LREGs, such as `rvtt_sfpswap_indexed_int`), a deterministic
   search chooses consistent alternatives, an independent DF oracle
   proves the binding sound, and only the pin-derived webs are
   rewritten to their hard LREGs before IRA; everything else stays a
   pseudo.  Refusals (`dualbank-*`) leave the stream unchanged.

## Refusals

Every refusal fails closed and leaves the `lreg-pressure-exceeded`
failure in place.

- `lreg-spill-inexact-dst-mode`: an explicit 16-bit-format Dst read
  (FP16A/FP16B/INT8/UINT16/INT16/INT8_COMP/LO16_ONLY/HI16_ONLY — it
  shows the rows are 16-bit, so it refuses even under the
  declaration), a non-constant mode operand, or `dst-layout-undeclared`
  (mod0-0 / Dst-untouched body without `-mtt-tensix-dst-layout-32b`).
- `lreg-spill-no-free-dst`: a non-constant Dst address; a load in
  mod0 10 (INT32_ALL masks the RWC base); an rwc-window/cross-epoch row
  (unproven counter position); a layout boundary (config/address-
  modifier write); an unproven minted-epoch step or trip bound; scratch
  rows exhausted; unproven no-increment mode.
- `lreg-spill-laneconfig-unproven`: a function-local SFPCONFIG write
  to dest 15 (LaneConfig): column-exchange / lane-block bits silently
  redirect or drop round-trip lanes.
- `cc-enable-unproved`: a spill point the point-wise CC lattice cannot
  prove all-lanes: a pure def under narrowed/unproved CC, or a read
  feeding a consumer that is not lane-gated (SFPSTORE/SFPLOAD move only
  CC-enabled lanes; no all-lanes store variant exists; the lattice is
  the `rtl-rvtt-dst-ownership.cc` one -- PUSHC/POPC stack, COMPC/typed
  writes narrow, the proven all-lanes SFPENCC restores).
- `dst-rwc-effect-unproved`: any opaque insn (call, asm, unaudited
  pattern), RWC boundary, or non-LaneConfig layout boundary.
- `lreg-alloc-unknown-use`: a bare USE of the web being spilled (the
  USEs ending raw-LREG reservations are skipped; reservations are never
  candidates).
- `lreg-spill-rewrite-refused`: an insn that does not admit the
  operand rewrite.
- `lreg-spill-no-candidate`: only reservations / reload temporaries in
  the blocked neighborhood (e.g. more pinned values than registers at
  one point — architecturally unallocatable).
- `hard-sfpu-reg-pre-ira`: an SFPU hard register referenced in pre-IRA
  RTL other than as a raw-LREG reservation (graph-collection
  fail-closed).
- `wide-sfpu-mode-unproven`: an XTT64/XTT128-mode pseudo present
  (multi-register webs unmodeled; graph-collection fail-closed).
- `precolor-conflict`: one web pinned to two different LREGs by its
  metadata constraints (graph-collection fail-closed).

## Scratch-row derivation

A Dst address is base-relative: `(imm + RWC_Dst + MATH_Offset +
REGW_Base) & 0x3FF`, and the physical 32-bit row map
(`dst32b_adjust_row`) aliases two logical rows only when they are
congruent within ±3 modulo 256 (the same physical-row model
`gimple-rvtt-transp-involution.cc` uses).  A scratch offset is
therefore proven free iff it keeps that distance from every kernel row
across the epoch's whole bounded sweep; offsets are taken 4-aligned
descending from 252 (then 1008..256 for offset-heavy functions).
`lreg-alloc-dst32-scratch-derivation-bh.C` shows the derivation moving
off a kernel-claimed row.

## Dst layout, RWC epochs and lane state

- **`-mtt-tensix-dst-layout-32b`**: runtime-resolved SRCB (mod0 0)
  accesses -- every plain SFPI `dst_reg[]` access -- and Dst-untouched
  bodies are accepted only under this explicit integration-layer
  declaration (in-function 32-bit accesses are not layout proof; they
  can only cause a refusal).  An explicit 16-bit-format Dst read
  refuses even under the declaration; a 16-bit-format store is an
  output conversion (geometry-32 under the declared layout) and is
  row-accounted.  **Declaring the flag falsely on a 16-bit-layout
  kernel makes a spilled compilation produce silent wrong output.**
- **Epoch/offset lattice**: the Dst counter is tracked symbolically;
  audited INC/FACE deltas accumulate within an epoch, disagreeing
  joins mint the block's stable token (a net-zero own-cycle pred is
  self-consistent and does not mint; a moving one forces the mint and
  records the per-iteration step).  Spill immediates are compensated
  per point (S - off); RMW defs get distinct store/reload immediates.
- **Bounded loop range**: a minted epoch's scratch row moves through
  Dst across iterations while earlier iterations' output rows stay
  live for pack.  Spills there are accepted only with a proven step and a
  proven constant trip count (RTL simple-loop analysis, cap 96), and
  the chooser clears the alias window across the whole swept range
  (S - K - m*step, |m| <= trips).  A web live into a minted join
  (loop-carried across the RWC backedge) is never a candidate.
- **Point-wise CC**: spill stores need provably all-lanes CC; reloads
  are accepted under narrowed CC into lane-gated lane-local consumers
  (membership is the `xtt_lane_gated` attribute in `rvtt.md`, failing
  closed; cross-lane ops, plain all-lanes copies -- the `rvtt_sfpassign`
  SET pattern, i.e. the unconditional SFPMOV-mod-2 move, not the
  CC-gated predicated-assign whose SET_SRC is `UNSPECV_SFPASSIGN`,
  which is accepted -- SrcS stores and loadmacro forms are excluded);
  RMW defs by lane-gated insns are exact at any CC (the scratch keeps
  the old disabled lanes -- predicated-write semantics).

## Known limitations (fail-closed, named)

- **No scratch-row reuse**: each spilled web takes its own row; a wide
  straight-line chain of short webs (e.g. a GIMPLE-peeled ring at -O3)
  can exhaust the pool -> `lreg-spill-no-free-dst (scratch rows
  exhausted)`.
- Opaque insns (raw `.ttinsn` outside the audited pure-RWC class,
  calls, asm) refuse: SFPU bodies that should be eligible must be
  `noinline` functions so the typed body is its own allocation region,
  separate from raw LLK initialization.

## Ambient contracts the flag carries

Function-local analysis cannot see state established before the
function.  These are the same trust boundary as the ambient all-lanes
CC contract that the CC synthesis (`gimple-rvtt-cc.cc`) assumes:

1. **Dst data width**: with zero typed Dst accesses in the function,
   the flag asserts the surrounding kernel does not view the scratch
   rows through a 16-bit format.
2. **LaneConfig**: the architectural default (no column exchange, no
   block bits — the reset state, which LLK initialization leaves in
   place) is assumed active at the calc body.
3. **Concurrent Dst consumers**: scratch rows are proven free only
   against the function's own typed accesses; the kernel contract is
   that no concurrent consumer (packer, neighbouring thread) touches
   Dst rows the calc body does not itself address while it runs.

## Tests (`g++.target/riscv/tt/tensix/`)

- `lreg-alloc-fire-bh.C` / `-wh.C`: nine loop-carried computed values
  (more than the file holds) compile; round trips at row 252, mod0 4,
  no-inc mode.
- `lreg-alloc-constants-renamed-varied-bh.C`: the shared
  `const-remat-body.h` shape, renamed and varied.
- `lreg-alloc-reserved-sentinel-fire-bh.C`: raw producer holds L7
  through a 9-live loop; only unreserved webs spill.
- `lreg-alloc-dst32-scratch-derivation-bh.C`: kernel rows 0/252 push
  the scratch row to 248.
- `lreg-alloc-rowloop-*-bh.C`: spills in a row loop with a proven
  trip bound (8 trips) compile with cleared offsets.
- `lreg-alloc-bf16-refuse-bh.C`, `lreg-alloc-cc-refuse-bh.C`,
  `lreg-alloc-rows-exhausted-rollback-bh.C`: named refusals, error
  preserved (the rollback test checks transactionality).
- `lreg-alloc-dst-layout-undeclared-bh.C`: no declaration -> named
  refusal.
- the 64-trip row-loop variant: the trip bound is proven but every
  scratch offset falls inside the alias window -> named refusal.
- `lreg-alloc-dualbank-*-bh.C` / `-wh.C`: dual-bank pinned-chain
  binding, its no-op case and its refusals.
- `lreg-coalesce-*.C`: conservative coalescing fires and near misses.
- `lreg-alloc-within-file-bh.C`: 8 live values, allocator stands down
  ("allocation left to IRA as today"), zero stores.  Note: peak <= 8
  does not certify 8-colorability — the stand-down is the no-op
  condition, not a coloring claim; and the DSATUR success verdict on
  engaged functions is a graph-level certificate (pattern tie/matching
  constraints remain IRA's).

Under `-mtt-tensix-optimize-pressure-schedule` the pass also writes a
dump-only pressure audit (`colorability=unchecked` lines).