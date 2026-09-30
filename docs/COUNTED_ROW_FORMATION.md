# Counted-row parameterized replay formation

This document describes the counted-row formation phase of the Tensix
replay pass (rtl-rvtt-replay.cc), enabled by
-mtt-tensix-optimize-counted-row-formation (default off).  The phase
canonicalizes families of near-identical instruction "rows" into
word-exact form ahead of the replay pass's word-exact sequence
discovery, so that the existing discovery, budgeting, capture and
launch machinery can record one parameterized row program and replay
it once per row.

## The problem

Template-expanded SFPU kernels frequently produce a fully unrolled,
straight-line body made of rows that perform the same computation on
different Dst rows.  The replay pass's in-block sequence discovery
only forms a record over runs that are identical word for word, and
such rows usually are not:

1. Rows carry per-row immediate materializations (for example an
   SFPLOADI pair loading a per-row constant in the middle of the row).
2. The register allocator rotates equivalent register assignments from
   row to row and block to block, so otherwise identical rows name
   different LREGs.
3. The body is straight-line code, so the counted-loop machinery never
   sees a loop.

The replay buffer is also a shared resource.  Slots that are recorded
by code outside the function (for example by a kernel's init routine)
may be launched from code the compiler cannot see, so the phase never
treats them as free: the function-scope ownership rule does not extend
to overwriting content recorded by other code, and the available
budget is only what the function itself may use.

## The mechanism

The phase is generic: no instruction identity or operation name
participates in any decision.

1. **Exclusion is transparency.**  A member position whose clones
   disagree only on compile-time-constant operands is excluded from
   the residual row program.  The excludable class is derived from the
   cross-clone invariance proof together with the audited effect set:
   a single-slot immediate materialization whose only SFPU dataflow is
   the write (and read-modify-write) of its single destination
   register, and which carries no CC, Dst, RWC or configuration effect.
   Excluded members are transparent to sequence discovery and are
   relocated to the head of the clone that consumes their value, where
   they issue between launches; one with no consumer in the family
   moves past the containing clone's tail.  Movement legality is
   value-based: every use follows the new position, and no CC write or
   opaque instruction is crossed.  The residual must then be word-exact.

2. **Clone canonicalization for register rotation.**  Clones are
   matched against a reference clone under an evolving register value
   map (the launch-conversion matcher's test, run as a transform rather
   than a recognizer).  Every hard-register value whose mapping differs
   is rewritten to the recorded register: the whole value, its
   definition and every use.  The rewrite plan handles seed chain
   closure (a value with both a definition role and a live-in role
   retargets the reference definition), read-modify-write tie
   propagation (tied operand halves move together), reference-clone
   rotation, per-clone drops, live-in bridging by one all-lanes register
   move where the pinning definition is a fixed multi-definition
   instruction, and a bystander cascade (an untouched value displaced
   into an evacuated register).  The plan is verified by a whole-block
   final-assignment occupancy simulation plus a lane-state constancy
   window: no CC write may occur inside any rewritten value's span.

3. **Slot budget.**  Candidates are ranked by modeled slot saving, with
   shorter residuals covering more clones winning ties.  A family whose
   residual exceeds the largest available replay-buffer span refuses by
   name; the budget never grows by taking slots recorded outside the
   function.

The phase emits the capture and its launches itself, mirroring the
replay pass's own sequence replacement, and marks its slots
persistent rather than relying on rediscovery.  The resulting launches
are priced by the replay pass's benefit model (delivered versus
executed slots, rvtt-cost.md): each launch grows by the excluded
words issued around it, and each bridge move costs one slot.

## Refusals

Every refusal is reported by name in the dump and leaves the code
byte-identical:

counted-row-excluded-member-unmovable, counted-row-residual-not-uniform,
counted-row-multidef-member, counted-row-map-live-out,
counted-row-slot-budget, counted-row-rename-interference,
counted-row-rename-constraint, counted-row-lane-state,
counted-row-bridge-clobber, counted-row-vacated-delay-shadow (a move
may not re-open a delay shadow that the nop inserter discharged against
the original order), and counted-row-final-lockstep-divergence (the
final order fails re-verification), plus the epoch and dirty-state
refusals inherited from sequence discovery.

## Limitations

A family whose clones use differently aligned register partitions, or
whose reference clone keeps a bridged value live across a block, is
refused rather than split: bridge targets must be dead across each
block.  Coverage of a whole unrolled body therefore depends on the
register assignment being uniform enough that one residual record
fits the available replay span.
