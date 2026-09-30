# The generic SFPLOADMACRO macro planner

This document describes the SFPLOADMACRO macro planner
(rtl-rvtt-macro-planner.cc and the rvtt-macro-* support files): how it
discovers repeated row computations, schedules them onto the
SFPLOADMACRO sub-units, synthesizes and verifies the descriptor words,
and forms the launch calendar.  All planner flags default off (see
"Flags" below); with them off, code generation is unchanged.

## 0. Design rules

The planner exists so that ordinary typed C++ compiles to SFPLOADMACRO
code competitive with handwritten LLK kernels through reusable,
generic mechanisms.  Compiler decision logic never keys on operation
names, exact opcode calendars, coefficient or immediate fingerprints,
or magic instruction words.  Raw encodings live only in the target
encoding and capability tables (`rvtt-macro-tables*`) and in test
expectations.  Every refusal is named in the dump and never mutates
code: a refused region is byte-identical to flags-off output.  Tests
for each mechanism cover a renamed-equivalent kernel, varied
constants, near-miss refusals, and at least two unrelated shapes.

## 1. The seven layers

The planner is a layered pipeline; every layer consumes only the
previous layers' vocabulary, never IR shape names.

1. **Typed effect attributes** (`rvtt-cost.md` attribute families,
   `rvtt-effects.{h,cc}`).  Every Tensix instruction pattern is either
   *audited* -- carrying subunit, LREG read/write operand positions, CC
   effect, configuration effect, and Dst/RWC counter effect -- or
   *opaque* (the refusing default).  `rvtt_insn_effects` is the only
   classification vocabulary the later layers may use; recog-code
   comparisons are permitted solely to reach an accepted instruction's
   operands.

2. **Region discovery** (`rvtt-macro-region.cc`).  A *row* is a
   dataflow-closed slice from its Dst loads to its Dst store (every
   member feeds the store; no external LREG live-ins).  Rows group into
   regions when pairwise isomorphic to the first row under a value map
   with a uniform typed Dst stride.  Pure-RWC counter effects (typed
   TTINCRWC / TTSETRWC / face advances) terminate rows and separate
   *runs* inside one region; pure CC writes between rows are ambient
   enables.  Configuration writers and unsupported CC-writing value
   events refuse at the event itself (`row-config-write`,
   `cc-template-unsupported`); opaque effects are hard boundaries.  A
   region may have a single row (whether one launch amortizes its
   configuration is a profitability question), and a self-looping
   block marks a loop-body region.

3. **DAG scheduling** (`rvtt-macro-sched.cc`).  Dst accesses with equal
   typed address operands share one issued carrier (launch).  The
   carrier-grouping search is a deterministic sequence of candidates
   (maximal sharing, then store demotion), and the first candidate
   whose descriptor proves is committed.  Launched sequence events are
   non-Dst value operations the tables can host; per-event delays come
   from the tables' proven sequence programs or from the derived
   calendar (Sec. 3).  The typed Dst stride is absorbed into the last
   carrier only when the tables' address-modifier machinery covers the
   delta.

4. **Capability tables** (`rvtt-macro-tables.{h,cc}`,
   `rvtt-macro-tables-{bh,wh}.def`).  The home of every raw
   SFPLOADMACRO word: launch/SETC16/SFPCONFIG field layouts,
   address-modifier slot programs, proven whole-word sequence programs,
   proven misc words, hidden template LREG writes, the architectural
   timing facts of the derived calendars, and reference descriptors
   (test data, never decision input).  QSR is intentionally absent
   from the tables (`target-macro-encoding-unproven`).

5. **Descriptor synthesis** (`rvtt-macro-desc.cc`).  Selection is keyed
   by derived event structure -- per-macro subunit lists, store
   placement, and the accepted source instructions' opcode bytes
   through the TT_OP encoding tables -- never by shape or operation
   names.  Template words are field-packed (`encode_template`) from
   the accepted source operands or from table data (opcode byte, fixed
   mod1, positional routing selector, imm12 from the typed source
   immediate with a hard range refusal).  Sequence and misc words come
   either from a matched proven program (by its table label) or from
   the derivation layer described in
   docs/TIMING_CALENDAR_DERIVATION.md.

6. **Profitability** (`rtl-rvtt-macro-planner-cost.cc`).  Derived from
   configuration and drain costs; there are no row-count thresholds.
   Straight-line: every run independently amortizes the full
   configuration prefix.  Loop bodies: the prefix is paid once in the
   preheader and weighed against the profile body/preheader count
   ratio, in count space and without rounding.  The tables' break-even
   row counts (`reference_breakeven_rows`) are regression expectations
   only; the planner never reads them.

7. **Verification and formation** (`rvtt-macro-verify*`,
   `rtl-rvtt-macro-planner.cc`).  A differential verifier re-derives
   the expectations from the region's explicit facts; any mismatch is a
   descriptor refusal.  Formation proves configuration ownership (Sec.
   2), that all planner-owned physical LREGs are dead after the region,
   the all-lanes lane proof, and stride absorption, then emits the
   configuration prefix (ambient enable, owned SETC16 address-modifier
   program, descriptor words through an owned LREG), the per-row launch
   calendar, and the drain.  Refusal paths never mutate.

## 2. Region shapes and formation proofs

* **Single-row regions.**  Discovery accepts one-row regions;
  straight-line single rows normally refuse `unprofitable`, since one
  launch rarely repays its configuration prefix.

* **Loop-body regions with preheader configuration.**  The structural
  preheader is the loop header's unique external predecessor with a
  single successor (which proves at least one trip, so hoisting the
  ambient enable is not a zero-trip CC change), and every Tensix issue
  in the body must belong to the region.  Refusals:
  `loop-preheader-unproven`, `zero-trip-preheader-unproven`,
  `loop-body-not-owned`.  A preheader that is the function entry block
  refuses rather than splitting the entry edge.

* **Lane proof.**  The all-lanes proof is required at the region's
  first row only: members cannot write CC, so the entry state holds.
  A loop-body region whose enable is written once outside the loop
  takes the proof from the dominating trailing enable on the
  preheader's unique-predecessor chain (only scalar code between it and
  the loop entry).  When the kernel's init is opaque, the first row's
  own typed all-lanes restore (proven word-exact through the shared
  SFPENCC derivation) is copied to the head of the configuration
  prefix (`lane-proof=materialized-enable`).  The license is the
  compiler's outermost-CC-depth contract: the CC pass already rewrites
  the outermost POPC (restore the incoming state) into ENCC (enable all
  lanes), which is sound because the kernel convention pins the
  outermost lane state to all lanes; the materialized word re-writes
  the state that contract already guarantees.  Rows without an in-row
  proven restore refuse `all-lanes-proof-missing`.

* **Configuration ownership.**  The proofs are tried in order:
  1. Function-global: no call, asm, or typed access to an owned
     configuration destination anywhere in the function.
  2. Loop-scoped window, for loop-body regions: the proven structural
     preheader plus a body scan proving every instruction is either
     region-owned or provably inert scalar code (no call, no asm, no
     Tensix issue, no volatile memory reference).  The configuration
     prefix materializes at the preheader's tail, which dominates every
     trip's launches, and no path from it to the final drain contains
     another writer of the owned state.  Success prints
     `Macro-planner config-ownership: loop-scoped window`.
  3. Region-scoped, for proven CC-template programs the window did not
     prove (including straight-line shapes): the prefix-to-region-end
     span is clean of calls, asm and owned-configuration accesses;
     foreign configuration before the prefix is dead because the
     prefix rewrites every consumed destination; foreign code after the
     region is tolerated under the LLK convention that every
     SFPLOADMACRO consumer programs its own descriptors.

  Every failure keeps the name `config-ownership-unproven` and the
  explicit code.

* **Inter-row drain.**  Within a run, rows are emitted back to back;
  the scheduler's VD policy makes that sound only under VD
  alternation.  When synthesis fixes a value carrier's VD (a proven
  program's fixed VD, or a name-encoded consumer, Sec. 4), every row
  re-targets the same register while the previous row's hosted events
  can still be pending up to `drain_slots` past its launch, so the
  next launch's VD write would race them.  Emission therefore places
  the full derived drain between consecutive rows of such regions.
  Store-only sacrificial VDs (written, never read), the alternating
  envelope, and the CC-template model (whose next-row obligations are
  the inter-row contract) keep back-to-back emission.

* **Four-face sharing.**  Typed TTSETRWC face transitions are pure-RWC
  run separators, so four faces discover as one region sharing one
  configuration, both unrolled (four runs, per-face drain, transitions
  preserved in place) and as a face loop (prefix hoisted to the
  preheader).  On Wormhole the address-modifier bank-base obligation
  is discharged by the SFPU platform contract (the LLK start/done
  bracket pins ADDR_MOD_SET_Base=1), under which the tables' owned
  single-slot SETC16 program is the launch's only reachable bank; the
  base-0 bank holds LLK's live ADDR_MOD_2 and must not be written.

## 3. Proven whole-word programs and derived calendars

The tables carry proven whole-word sequence and misc programs for a
small set of shapes (binary min/max, sign bit, cast-round, and the
select class).  Descriptor synthesis tries those first, so the shapes
they cover keep byte-identical emission and their envelopes (uniform
mode, proven store modes, the shift-mode pair) bound what they may be
applied to; structurally identical shapes outside an envelope refuse
`descriptor-program-unproven`.

Shapes no proven program covers use derived calendars: the sequence
word format, per-event delays, sub-unit legality, and adjacency rules
are architectural facts in the capability tables, and the derivation
core (`rvtt-macro-derive-core.h`) computes earliest-feasible delays and
packs the words.  The facts, the algorithm, and the re-derivation of
every proven word are in docs/TIMING_CALENDAR_DERIVATION.md.  A
schedule that names its own blocker never reaches descriptor
synthesis, except `event-delay-unproven`, which does not block
formation when the whole-word program is proven end to end.

## 4. CC-template rows (predicated select)

Rows whose dataflow carries CC-writing events -- predicate write,
lane-predicated merge, all-lanes restore, store -- form through proven
CC-template programs.

* **Discovery** accepts CC writers in exactly two structural roles: a
  predicate definition (a value event reading LREGs and writing CC
  with no LREG result, the SFPSETCC-on-register class; value-producing
  CC writers such as the SFPIADD CC modes keep
  `cc-template-unsupported`, since no proven dual-effect template
  exists) and, only after a definition, the in-row all-lanes restore
  (a pure CC write proven word-exact through the shared SFPENCC
  derivation).  The row slice follows CC edges as well as LREG edges;
  a CC need surviving to the row entry is the ambient-enable
  dependency.  Every other CC writer keeps its named refusal
  (`cc-template-unsupported`, `cc-enable-unproved`).

* **Scheduling** hosts the definition on its predicate-source load's
  carrier and the restore on a later load carrier.  The lane merge (a
  CC-reading value event that reads its own destination, takes one
  other input, both produced by this row's loads, result consumed by
  the store) is realized as `CC_COALESCED`: no issued word and no
  template slot; the shared launch VD receives every payload and the
  post-visibility load is the predicated write.  Any other
  CC-dependent value event in a predicate-writing row refuses
  `cc-template-unproved`.

* **The descriptor CC model** (`macro_cc_model`) derives the slots that
  make the coalescing sound from the matched program's delays and two
  table facts: `cc_visibility_lag` = 1 (a macro event's CC result is
  visible to issues one slot after it executes) and
  `store_lane_mask_live_at_execution` (the delayed store's lane
  predicate is the live CC state at the store's execution cycle; the
  ISA's launch-latched store overrides are exactly Addr, the Mod0
  source and the backdoor bit, and a CC write retiring in the store's
  own cycle is not yet visible to it).  The obligations are:
  pre-visibility payload < definition visible <= post-visibility
  payload; restore visible after the predicated payload and by the next
  row's first slot; the all-lanes restore retires strictly before the
  store executes (`restore_exec < store_exec`); and the store retires
  before the next row's predicate definition executes
  (`store_exec < def_exec + ii`).  A schedule violating the
  restore/store ordering refuses `cc-restore-store-race`: with the
  restore retiring in the store's own cycle, the store executes under
  the SFPSETCC complement mask and leaves the true-branch lanes
  unwritten.  The template predicate sense is complemented exactly when
  the post-visibility payload carries the merge's live operand (the
  SFPSETCC mod1 complement mapping is table data).  Any other failed
  obligation refuses `cc-template-unproved`.  The verifier re-derives
  the whole model and re-checks every inequality from the exchanged
  slots.

* **Calendars.**  The compact calendar (three issue slots per row) is
  tried first: the restore is hosted on the earliest non-definition
  load carrier, the trailing payload load stays explicit and absorbs
  the row stride through its own auto-increment address mode (the
  owned SETC16 program; the typed separator is deleted), and the
  store's data mode rides its carrying launch's Mod0 (the proven misc
  word `select-launch-mod0`).  The interval compresses to three
  because the earlier-hosted restore is visible in the next row's first
  slot.  Additional obligations: the definition carrier's load mode
  must equal the store mode, and the absorbing explicit load must occupy
  the row's last issue slot.  Rows outside that envelope fall through
  to the four-slot calendar (two launches plus one explicit payload
  load, restore on the last load carrier, separator kept); under the
  derivation rules every such kept-separator schedule retires the
  restore in the store's cycle (restore exec 3 == store exec 3), so its
  descriptor refuses `cc-restore-store-race` and the region falls back
  to the explicit lowering.  The refusal is keyed on slots, not on
  structure: a kept-separator schedule that satisfied the constraint
  would form (exercised at the unit level in
  rvtt-macro-verify-test.cc).  The misc word of the select class is
  field-derived (0x700 | store mode, `encode_misc_select`).

## 5. Cross-tile configuration epoch

When a formed CC calendar's configuration preheader itself sits inside
an enclosing issue loop (the tile loop), the descriptor words program
SFPU state that survives from the previous iteration if nothing in
between writes an SFPCONFIG destination.  `rvtt-macro-epoch.cc` proves
that and hoists them.

* **The proof.**  Every instruction of the enclosing loop outside the
  region must be a proven non-writer of the planner's SFPCONFIG
  destinations: typed effects must carry no owned configuration access
  and no foreign SFPU dataflow; raw `.ttinsn` words (the constant
  single-input asm form, plus the audited scalar templates) must not be
  an owned-destination SFPCONFIG, with opcode and field layout taken
  from the capability tables; and every volatile store is treated as a
  potential RISC instruction push whose stored word must resolve to a
  32-bit interval with a provably non-SFPCONFIG opcode byte (or a
  provably unowned destination).  The value resolver is a
  demand-driven memoized reaching-value walk over post-RA hard
  registers -- constants, lui/addi chains, shift/and/disjoint-or
  composition, predecessor joins, and monotone self-loop inductions
  bounded by an equality exit; anything else refuses, as do calls,
  unrecognized assembly and opaque Tensix issues.  Refusals:
  `prefix-epoch-invalidated` (an intervening writer),
  `prefix-epoch-unproven` (an unresolvable word or opaque issue),
  `prefix-hoist-preheader-unproven` (no unique external entry).  A
  refusal keeps the per-iteration prefix and never blocks formation.

* **Placement.**  The descriptor words hoist to the enclosing loop's
  structural preheader; when the unique entry edge's source is shared
  control flow, the edge is split at commit time only, after every
  proof has passed, and the split block executes exactly when the loop
  is entered, which discharges the zero-trip obligation.  A copy of the
  proven all-lanes enable precedes the lane-predicated LREG
  materialization in the hoisted block, under the same
  outermost-CC-depth license as the materialized enable.

* **What stays per iteration.**  The ambient enable (the LLK bracket
  resets the lane configuration between tiles) and the owned SETC16
  address-modifier program (SETC16-visible state is reachable from
  data-plane MMIO writes the value proof cannot bound).

* **Scope.**  CC-template calendars only, so other formed shapes are
  unchanged; a second formed region in the same enclosing loop refuses
  automatically (its typed configuration writes are intervening
  writers).  Success prints `Macro-planner prefix-epoch: ...` and the
  formed line gains `prefix-epoch=hoisted`.

## 6. Multi-sub-unit integer rows

Integer rows (loads, sign-magnitude casts, 24-bit partial products,
shifts and accumulates, store) need several sub-units per macro.

1. **Generalized hosting** (`rvtt-macro-sched.cc`).  A value event is a
   hosting candidate when the SFPLOADMACRO override function realizes
   it exactly: its single written physical register is a carrier
   load's destination (a scheduled template's result is always the
   launch VD or LReg16), or it is the store's sole data producer and
   rides the store's carrier.  MAD-class events are accepted.  One
   event per (carrier, sub-unit); overflow and probe-refused events
   stay explicit issues.  Hosting is capacity-aware: distinct template
   tuples count against the four InstructionTemplate destinations, and
   the store's sole producer is placed first.

2. **Derived template classes** (`rvtt-macro-desc.cc`,
   `derived_value_template_fields`).  Beyond the constant-register
   SFPSWAP family: SFPCAST (audited modes; in-place VC:=VD route or a
   name-encoded surviving VC), register-form SFPIADD (the accumulator
   must be the launch VD so VB:=VD is the identity), immediate SFPIADD
   (the immediate rides the template imm12 field), the in-place
   immediate SFPSHFT realized as the SHFT2 immediate template on Round,
   and Blackhole SFPMUL24 (plain LOWER/UPPER modes; one factor is the
   launch VD, VC is the architectural zero register L9, VA is encoded
   in the template's imm12 region).  Both SFPMUL24 modes are symmetric
   in their factors, so when the allocator ties the product in place
   onto the other factor the commuted word realizes the same value; the
   established order is tried first.  An encoded VC of L0 is
   indistinguishable from the source-unused convention and refuses.

3. **Template sharing.**  Bit-identical derived field tuples encode to
   bit-identical template words and share one InstructionTemplate
   destination.  Ownership covers the full LoadMacroConfig class
   {0..8} (InstructionTemplate[0..3], Sequence[0..3], Misc are one
   architectural state class).  `encode_template` accepts the
   architectural destination selectors 0xc..0xf (VD = 12 + i).

4. **Fixed launch VDs.**  When a surviving name-encoded read consumes a
   value carrier's loaded register, alternating VDs would move the
   value away from the name its consumers use, so every value carrier
   keeps its own physical load destination, under a lifetime
   obligation: every VD-consuming event executes before the next row's
   launch rewrites the register.

5. **Explicit-issue hazard model** (`rvtt-macro-derive-core.h`).  Rows
   whose hosted events share registers with explicit members carry WAR
   floors (an event's write retires strictly after every earlier
   explicit reader), consumer and overwrite deadlines (exec + latency -
   1 <= reader slot; exec <= overwriter slot; scheduled events retire
   before same-cycle issues), and same-cycle anti-dependence edges.  The
   fixpoint computes earliest-feasible cycles, so a violated deadline is
   a genuine infeasibility and refuses `sequence-derivation-hazard`.

6. **Store-only sacrificial VD.**  The launch's VDLo field encodes VD
   0..3 (VDHi aliases the address LSB and stays 0 for even Dst
   addresses).  When every low register is row-live, a
   proven-clobberable internal temporary serves instead: its first row
   access is a write, it is not a fixed carrier VD, and no launched name
   field reads it.

7. **Expectation parity.**  `rvtt_macro_build_expectations` mirrors
   synthesis' uniform-mode program filter and falls through to
   derived-calendar expectations, so the verifier keys the same program
   synthesis realized; the verifier dump prints got-versus-expected
   launch words.

## 7. Drain placement

`core_drain_slots` (`rvtt-macro-sched-core.h`) derives the greatest
event writeback distance past a run's last issue slot from the
descriptor's own SequenceBits delay fields.  Without
`-mtt-tensix-optimize-drain-schedule`, emission pays that many SFPNOPs
after every run, so a region split into R runs by a boundary
instruction (a face advance) executes the drain R times.

With the flag, `rvtt_macro_drain_boundary_elidable`
(`rtl-rvtt-schedule.cc`) decides per intra-region boundary, before any
mutation, using these hardware facts:

* Launch-latched (safe to change while events are in flight): the store
  event's Dst row, the store format, and the Dst layout its decode
  reads.  A pure Dst/RWC counter write therefore cannot disturb an
  in-flight event.
* Live at execution (changing them inside the horizon races): the lane
  predicate, the LREG contents staged events read or write, the
  LoadMacroConfig words, and Dst rows an in-flight store writes.
* Horizon arithmetic: writeback slot = carrier issue slot + programmed
  delay, and at most one instruction issues per cycle, so a follower
  word's position in the issue stream lower-bounds its cycle distance
  from the boundary; a dynamic stall only moves follower accesses
  later, which is the safe direction.

The drain is elided exactly when (a) the inter-run stream is pure-RWC
separators only, all launch-latched, with slot credit granted only to
classes no later pass absorbs (face-class typed advances and audited
raw SETRWC-class words; absorbable INC-class TTINCRWC earns nothing,
per the Dst auto-increment `AIC_RWC_STEP` contract); (b) every access
of the next run is ordered after every pending event by per-event
delays decoded from the descriptor's own sequence words
(`decompose_sequence_word`), cross-checked against the descriptor's
`drain_slots`; same-cycle ordering follows retire-before-issue, so a
front-end access at the last retirement cycle is accepted at equality,
while two staged events in one cycle remain a race; and (c) the
enumerated follower words cover the whole horizon.  The final run's
drain is the region's exit contract and is never elided.

**Loop-backedge elision** (same flag).  A loop-body region's final run
ends at the loop latch, so the drain executes once per trip where the
architecture requires it once per loop exit.
`rvtt_macro_drain_backedge_elidable` proves the backedge follower
stream (the in-body tail, the loop-head prefix, and the region's own
first run in the next iteration) with the same decoded slot
arithmetic, after classifying every interposed instruction:
never-absorbed launch-latched pure-RWC words earn slot credit,
absorbable INC words none, and proven-neutral scalars (no call, no
asm, no memory store) none.  On success the full derived drain is
placed once on the loop's exit path (the exit head when it has a single
predecessor, otherwise a commit-time edge split).

Refusals keep the full derived drain and never block formation:
`drain-follower-opaque`, `drain-lreg-overlap`, `drain-cc-live`,
`drain-config-overlap`, `drain-dst-raw`, `drain-delay-unproven`,
`drain-horizon-spill`, and, at the backedge, `drain-exit-shared`.  The
profitability model still prices the full per-run drain.

## 8. Descriptor-program residency

Under `-mtt-tensix-macro-planner-residency`, the SFPLOADMACRO descriptor
registers are treated as a programmable dictionary whose entries are
the planner's derived descriptor words, and identical descriptor
programs are pushed once per kernel instead of once per region or per
enclosing-loop trip.  Both parts are implemented in `rvtt-macro-desc.cc`.

* **Outward span extension** (`rvtt_macro_residency_extend`).  After a
  successful epoch hoist (Sec. 5), the per-level epoch proof
  (`rvtt_macro_prefix_epoch_hoist`) is iterated through successively
  enclosing loops, and the outermost proven level's preheader receives
  the hoisted block.  Because the resident program may now execute on
  paths that never reach the region, the function-wide owned-state
  invariance walk (`rvtt_macro_epoch_owned_state_invariant_p`) must
  show that, outside the planner's own emissions and the region, no
  instruction writes an owned SFPCONFIG destination or delivers an
  unresolvable or opaque word; executing the programming words early is
  then unobservable.  The walk deliberately accepts foreign LREG/CC
  dataflow and owned-destination reads, through which a placement move
  cannot be observed.

* **Content-equality de-duplication** (`rvtt_macro_residency_lookup`).
  Descriptor programs are canonicalized by content: the bit-exact
  template, sequence and misc words (SETC16 programs and launch tuples
  are per region).  A later formed region whose canonical words equal
  an already-programmed entry, whose launch block that entry's
  placement dominates (regions are processed in forward program
  order), and whose function-wide owned-state invariance holds, omits
  its descriptor-word programming; its ambient enable and owned SETC16
  program stay.  Different content programs per region as before.

The ambient enable and the owned SETC16 program are never moved by
residency.  Refusals keep the existing placement:
`resid-skip-path-unproven`, `resid-span-unproven`,
`resid-dominance-unproven`.

## 9. Iterative modulo scheduling: repair and arbitration

Under `-mtt-tensix-macro-ims`, the scheduler applies the structure of
iterative modulo scheduling (Rau, MICRO-27, 1994):

- **II** = issued words per row (launches plus explicit issues);
  occupancy is checked modulo II by the derivation core.
- **Reservation tables** = the capability tables: one event per
  (macro, sub-unit byte), one event per sub-unit per cycle across row
  instances, the write-port classes, the Simple/Round VD16 split and
  SWAP adjacency rules, delay fields 0..SEQ_MAX_DELAY.
- **Latencies** = `subunit_result_latency` (Simple/Round 1, MAD 2).
- **Feasibility** = the existing derivation fixpoint plus descriptor
  synthesis and the verifier; the repair search only enumerates.
- **Backtracking** (`rvtt_macro_schedule_region`): after the
  established grouping candidates, the search continues with
  deterministically enumerated reduced hosted sets -- single removals
  in reverse program order, then pairs -- capped at IMS_REPAIR_BUDGET
  variants per grouping.  Maximal hosting is tried first, so repair can
  only recover regions the established search refused.

Rows carrying a predicate definition keep the established CC candidate
space.

**Formation-versus-replay arbitration.**  The profitability gates
compare the formed calendar with explicitly issued rows word for
word, which over-prices the alternative when the replay unit would
compress those rows.  With `-mtt-tensix-macro-ims`, replay optimization
enabled, and every row word replay-safe, formation additionally
requires, per run,

    config*PUSH + rows*ii*D + drain*SLOT  <  rows*row_words*SLOT

(loop regions weight config by the preheader count and the rest by the
body count), where D = SLOT when `-mtt-tensix-macro-planner-replay`
wraps the formed launches and PUSH otherwise (rvtt-cost.md).  The
right-hand side is the replay alternative's steady-state lower bound,
so the test is biased toward refusal: a formation that cannot beat the
ideal replay delivery of the same rows refuses
`replay-delivery-preferred`, with both prices printed.

## 10. Extra value carriers

Under `-mtt-tensix-macro-ims-carrier` the planner searches the other
direction, the handwritten kernels' re-load idiom, as a commit-or-revert
mutation of every unrolled row copy:

1. **Seed re-load.**  A region-accepted plain Dst load (no live-value
   merge; RWC-inert) is duplicated into a provably free
   launch-encodable register from L0..L3, highest first.  When every
   low register is taken, a row-internal low web is first relocated to
   a free high register (a whole-web physical rename).  The copy is
   placed at the row head or directly after the seed.
2. **Prefix replication.**  Every in-place writer of the seed register
   between the seed and the chain head is cloned onto the new carrier,
   so the new register carries the exact value the chain consumes.
3. **Version-split chain rename.**  A deterministically enumerated
   chain (the head reads the seed's value, each later member reads the
   previous member's result) is renamed onto the carrier register, with
   version linearity proven per member.
4. **Re-derivation as the oracle.**  The mutated function re-runs the
   full pipeline (discovery, scheduling with repair when enabled,
   synthesis, verification, every formation gate).  A variant commits
   only when the region proves at a strictly smaller II than before, or
   proves where the established search proved nothing; otherwise the
   journal restores the original patterns and deletes the inserted
   copies.

Refusals: `ims-carrier-legality-unproven`,
`ims-carrier-lreg-unavailable`, `ims-carrier-web-unsplittable`,
`ims-carrier-row-divergent`, `ims-carrier-rederive-unproven`,
`ims-carrier-no-improvement`.  The search is bounded to a fixed number
of variants per region.

## 11. Cross-call init hoist

Under `-mtt-tensix-optimize-init-hoist`, a non-inlined per-tile callee
need not re-execute the formation's idempotent init prefix
(descriptor program, owned SETC16 program, all-lanes enable) on every
call.  Formation hands the prefix to `rvtt_crosscall_init_hoist`
(gimple-rvtt-crosscall-hoist.cc), which proves the caller side while
every caller body is still GIMPLE and inserts the prefix as typed
builtin calls in the caller's loop preheader; the callee's emission
omits the hoisted part.  The caller loop is scanned with an audited
delivered-word classifier asking whether a statement could write the
hoisted state, launch a macro, or replay recorded content.

* Stage 1 hoists the descriptor words only: their sole readers are the
  callee's launches.  The callee keeps its per-call enable and owned
  SETC16 program.
* Stage 2 additionally hoists those when every decodable SETC16-class
  delivery to an owned row in the caller closure equals the contract's
  word and each owned row has such a write dominating the loop.

Refusals keep the per-call prefix and never block formation:
`drain-init-callers-unproven`, `drain-init-loop-unproven`,
`drain-init-ownership-unproven`, `drain-init-vector-live`,
`drain-init-mop-slot-unproven`, `drain-init-callee-unproven`,
`drain-init-idempotence-unproven`.

## 12. Template-word vocabulary

`rvtt-macro-vocab-enum.cc` is an offline enumerator over the encodable
(opcode, mod1, operand-binding) class space of the capability tables
and a per-opcode semantic model.  It classifies every class as
accepted, candidate, or architecturally excluded.  Excluded classes
include SFPSWAP's live-operand dual-result form (it writes both VD and
VC), SFPLUT/SFPLUTFP32 (hidden LUT registers), PRNG modes, the
LReg[7]-indirect SFPMAD/SFPMUL modes, and SFPNOT (no effect audit on
its pattern).

Two limits follow from the architecture rather than from the search:
a row may use at most four distinct template words (the four
InstructionTemplate destinations, against which every launch resolves
its selectors), and template-word identity is a function of
(opcode, mod1, src_c, imm12) only, so a hosted event can share a word
only with a bit-identical one.  The derivation core also bounds a
calendar to MAX_EVENTS hosted events.

## 13. Refusal vocabulary

The names are a stable dump interface.

* Discovery: `row-opaque-effect`, `row-not-closed`,
  `cc-template-unsupported`, `row-config-write`, `row-not-isomorphic`,
  `row-stride-mismatch`, `row-live-through`, `cc-enable-unproved`.
* Schedule: `event-delay-unproven`, `sequence-encoding-unproven`,
  `template-capacity-exceeded`, `port-conflict`, `latency-violation`,
  `target-macro-encoding-unproven`, `cc-template-unproved`.
* Descriptor: `descriptor-program-unproven`,
  `descriptor-encoding-failed`, `descriptor-verification-failed`,
  `cc-template-unproved`, `cc-restore-store-race`, and the derivation
  refusals of docs/TIMING_CALENDAR_DERIVATION.md.
* Formation: `config-ownership-unproven`, `planned-lreg-live`,
  `all-lanes-proof-missing`, `loop-preheader-unproven`,
  `zero-trip-preheader-unproven`, `loop-body-not-owned`,
  `unprofitable`, `stride-not-absorbed`, `replay-delivery-preferred`.
* Never blocking formation: the prefix-epoch (Sec. 5), drain (Sec. 7),
  residency (Sec. 8), carrier (Sec. 10) and init-hoist (Sec. 11)
  refusals.

## 14. Flags

All default off:

* `-mtt-tensix-macro-planner` -- formation.
* `-mtt-tensix-macro-planner-analyze` -- report only.
* `-mtt-tensix-macro-planner-verify` -- differential verifier (also
  run when GCC is built with checking).
* `-mtt-tensix-macro-planner-verify-corrupt-template` -- verifier
  self-test.
* `-mtt-tensix-macro-planner-replay` -- accept planner-formed launches
  into automatic replay recording.  A launch is a pure instruction
  word: recording captures the word, never state, and execution at a
  replay site reads the then-current descriptor configuration.
* `-mtt-tensix-optimize-drain-schedule` -- drain placement (Sec. 7).
* `-mtt-tensix-optimize-init-hoist` -- cross-call init hoist (Sec. 11).
* `-mtt-tensix-macro-planner-residency` -- descriptor residency
  (Sec. 8).
* `-mtt-tensix-macro-ims` -- repair and arbitration (Sec. 9).
* `-mtt-tensix-macro-ims-carrier` -- extra value carriers (Sec. 10).

## 15. Known limitations

* The verifier's template, sequence and misc expectations derive from
  the same program table as synthesis; detecting a wrong table entry
  rests on the byte-parity tests (launch words and SETC16 programs are
  re-derived independently).
* Function-global configuration ownership is conservative;
  path-sensitive refinement through `rvtt-macro-ownership` is not
  implemented.
* Loop-body regions are single-block self loops; multi-block loop
  bodies and regions spanning control flow are out of scope.
* The ambient-enable proof accepts any pure CC write as the enable
  shape; that the written state is all lanes is carried by the copied
  instruction itself (emission replays the exact enable pattern), not
  re-proven architecturally.
* The region-scoped ownership proof tolerates foreign code after the
  region, relying on the LLK convention that every SFPLOADMACRO
  consumer programs its own descriptors.
