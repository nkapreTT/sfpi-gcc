/* Break storage-induced false LREG dependences in capturable rows.
   Copyright (C) 2026 Tenstorrent Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* After allocation, a row can carry a false recurrence: an invariant-input
   member's destination register is also written by other row members purely as
   storage reuse (the allocator packed unrelated short lifetimes into one
   LREG).  The downstream stall-fill mechanisms (interlock fill, capture
   rotation in rtl-rvtt-schedule.cc) then refuse the member as a filler by name
   -- "writes a register another row member also writes" -- although no value
   ever flows between the colliding lifetimes.  This is regrename's classic
   du-chain problem (gcc/regrename.cc) scoped to the architectural LREG file
   with typed-effect proofs instead of constraint queries.

   Options.  -mtt-tensix-optimize-lreg-rename-chains (default off) enables the
   du-chain engine below; -mtt-tensix-optimize-lreg-rename requests the same
   engine (the pass gates on either).  -mtt-tensix-optimize-rename-temporal and
   -mtt-tensix-optimize-rename-cc-region (both default off) widen target
   selection and the CC-span rule respectively.

   The engine performs post-RA register renaming over def-use chains in the
   gcc/regrename.cc formulation, restricted to the architectural LREG file,
   over single basic blocks of any shape.  One chain is one single-LREG
   definition plus every true reader of that definition up to the chain close
   (the next writer of the register, or the register's death at block exit).
   The whole web moves to a proven-free LREG; delivered words are unchanged,
   only register fields move (asserted after the commit).  Writers of any
   audited latency are candidates, and their inputs need not be invariant: a
   pure rename moves no instruction, so input dependence is irrelevant.

   The engine has two modes.

   - Service: rvtt_lreg_rename_chain (bb, def_insn, target) carries the
     complete legality proof and the post-commit re-verification, so the fill,
     rotation and interior-scheduling consumers can request renames without
     duplicating any proof.  The requesting consumer prices its own benefit and
     can undo the web exactly (legality and pricing are decoupled).

   - Standalone: the pass itself renames storage-collision chains greedily,
     accepting a rename only when the whole row is no worse under the shared
     timing engine (rvtt_timing::interlock_sim) and pricing nothing it cannot
     prove, subject to the periodic-window guard below.

   Periodic-window guard (standalone mode only).  A fully unrolled loop leaves
   a block that is k identical repetitions of one delivered-word body, and the
   allocator's first-fit reuse packs each repetition's short lifetimes into the
   same registers -- so every repetition boundary is a storage collision,
   exactly the shape the greedy sweep chases.  But those repetitions are what
   the downstream replay recorder exploits (one recorded window replayed k-1
   times), and renaming cannot preserve them: the whole-block-free tier proves
   a target untouched across the block, so once one repetition's chain takes a
   target no other repetition can take the same one.  A uniform rename of all k
   copies is impossible, and a partial rename makes identical copies diverge,
   splitting the replay recording.  The guard refuses, as
   regrename-periodic-window, any standalone-mode chain whose edited window's
   Tensix word sequence -- writer through the last position the commit would
   rewrite; not the full span, whose kill close is often the next repetition's
   first word and whose dead-at-exit tail is often unique block-end code --
   occurs a second time in the block's Tensix stream (INSN_CODE plus
   delivered-word pattern equality, with same-mode scratch clobbers, which emit
   no word, comparing equal).  The service path is exempt: a consumer prices
   its own benefit against a realized schedule and undoes refused webs exactly.

   Typed-effect veto (every fact through rvtt_insn_effects -- the single
   typed-effect table -- and rvtt-effects.h's post-admission helpers): a chain
   refuses on a CC write, config-dest write, RWC/Dst counter effect, Dst store
   destination, replay owner class, companion-coupled multi-result group,
   pinned zero-length LREG protocol markers, and any implicit (non-operand)
   register access -- an effect-mask reference to the chain register with no
   explicit REG occurrence in the pattern (the LUT implicit-slot class) cannot
   be edited and refuses.

   CC safety: a chain span (def to close) never crosses a basic-block boundary,
   and any CC-state event inside the span (cc_write in the typed effect set:
   SETCC/ENCC/PUSHC/POPC and every other mask writer, all-lanes-proven or not)
   refuses regrename-cc-span.  With the mask constant across the span, every
   reader executes under the writer's mask, lanewise operations never read
   sources on disabled lanes, and a kill-close overwrites exactly the written
   lanes -- the renamed register's undefined disabled lanes are unobservable
   and the old register's disabled lanes carry the identical pre-def values.
   With -mtt-tensix-optimize-rename-cc-region the rule widens to the two proven
   arms of the span-scoped frame view that tt/rvtt-cc-region-rtl.cc derives
   from the post-RA insn stream: CC activity confined to balanced in-span
   pushc/popc frames with narrowing-only vocabulary (every interior lane-enable
   state a subset of the span-entry mask, so every reader still observes only
   definition-written lanes and a kill-close still overwrites exactly them), or
   an all-lanes span entry proven by the kill-modeling backward walk (the
   definition wrote every lane; a required end state is restored word-exactly
   by the all-lanes SFPENCC).  Every unproven arm refuses
   regrename-cc-span-region-unproven with the view's class in the dump.
   Without the flag the blanket rule applies, and under flag_checking the view
   shadows it (every span the blanket rule allows must classify as NO_EVENT; a
   divergence is a hard assert).  The temporal tier's close-to-fresh-definition
   gap rule keeps the blanket no-CC-event discipline: its obligation is mask
   equality between the chain span and the fresh definition, a different fact
   from the span's subset containment.

   Death proof: the close is the next writer of the register inside the block
   (lane-exact under the constant-mask span rule), or -- only in functions
   containing no opaque instruction (calls, asm, raw .ttinsn words are
   invisible to DF hard-register liveness) -- the register's absence from the
   block's DF live-out set.  Anything else refuses regrename-chain-open.

   Target deadness proof (the same DF trust boundary): a whole-block-free
   target must be dead across the block -- neither live-in nor live-out -- so
   the rename clobbers no value that flows around the block.  DF hard-register
   liveness is reliable only in a function with no opaque instruction: in an
   opaque function a block-untouched register can be loop-carried live through
   the block (produced in a successor, consumed around the backedge) without DF
   seeing it, and renaming onto it would overwrite the loop-carried value.  The
   whole-block tier therefore admits no target in an opaque function
   (regrename-liveness-untrusted); the temporal tier's admitted arm
   re-establishes the target with an in-block fresh definition, is
   DF-independent, and stays available.

   Post-commit structural re-verification: after apply_change_group commits,
   the pass re-extracts every edited pattern's typed effects and re-proves the
   chain shape on the actual stream -- writer defines exactly the target,
   readers read the target and no longer the source, no other block instruction
   references the target, the close still kills, and every edited instruction
   re-recognizes with its delivered word count unchanged.  Any divergence
   reverts the rename, refuses regrename-postcommit-divergence (hard assert
   under -fchecking), and changes nothing.  This census scans only the block,
   so it cannot see a loop-carried live-through target whose other references
   are in other blocks; it therefore also fails closed on a committed
   whole-block (non-temporal) target in a function with an opaque instruction,
   so the check does not depend on the admission path alone.

   Pipeline placement: after register allocation and Dst ownership, and ahead
   of the hazard scheduler's fill passes, so a broken storage collision widens
   the fill candidate sets in the same compilation (see rvtt-passes.def).
   Renames neither add upward-exposed uses nor extend any live range across a
   block boundary, so DF liveness computed at pass entry stays valid across
   commits.

   -mtt-tensix-optimize-rename-temporal widens target selection from
   whole-block freeness to span freeness: when no register is untouched across
   the whole block, a target is still admissible when it is untouched across
   the chain span itself and its lifetimes outside the span provably cannot
   observe the chain value.  The proof obligations, all fail-closed:

   - the target is untouched by every position inside the span (writer through
     close inclusive; to block end for a dead-at-exit chain);

   - no zero-length pinned-LREG protocol marker references the target anywhere
     in the block (pinned interfaces never temporal-rename);

   - if the target is touched after the close, the first such position is an
     effect-audited Tensix instruction that freshly defines the target (writes
     it, does not read it), and no CC-state event occurs between the close and
     that definition: the fresh definition then executes under the same lane
     mask as the chain span, so it overwrites exactly the lanes the renamed web
     wrote, and the disabled lanes -- which no masked write in either world
     ever touched -- carry the identical pre-existing contents; after that
     definition the two worlds' register contents are equal everywhere.  A
     first post-span touch that reads the target, is not effect-audited, or
     sits beyond a CC event refuses the candidate;

   - if the target is never touched after the close, it must be DF-dead at
     block exit, and DF hard-register liveness is trusted only in functions
     with no opaque instruction (the dead-at-exit close's own discipline);

   - touches before the writer need no extra proof: the span rule plus the
     post-span rule leave no position that can read the target between the
     chain's first write and the point the two worlds re-converge, and every
     ordering constraint the shared register creates is carried by the
     pattern-derived dependence vocabulary the downstream schedulers consult;

   - the writer must not read its own destination.  A dest-reuses-dying-source
     writer (w.fx.lreg_read & oldbit -- and not the operand-1 live-value merge,
     already refused regrename-self-merge) is renamed asymmetrically: the write
     moves to the target, but the input read of the old register stays (it
     reads that register's previous live range).  The original writer killed
     the old register; the renamed one does not, so the dying-source value
     stays live on the old register from the writer through to the close.  The
     span-scoped target-freedom proof models only each chain's own span, not
     this extended old-register range, so a later temporal chain could pick the
     old register (or an overlapping span-marginal one) and alias the
     still-live value, with every lane enabled.  Such writers refuse
     regrename-temporal-dest-reuse; the whole-block tier is immune (its targets
     are all block-globally dead) and still serves them when a globally-dead
     target exists.

   Pricing of temporal targets.  A temporal target is not free storage the way
   a whole-block target is: it is borrowed from a register with a live
   downstream story, and the loan carries two costs a span-scoped no-worse
   check cannot see:

   - span-external coupling: a fresh-definition-arm target is by construction
     touched again after the close, so the rename couples the chain span to
     that fresh definition (a new scoreboard dependence) and claims the
     register hole between the close and the definition that the downstream
     interlock-fill pass uses to hide latency.  The temporal priced window
     therefore extends through the target's first post-span touch, so the
     coupling is priced in the row it taxes;

   - stream-identity externality: replay and MOP formation downstream require
     textually identical unrolled bodies; a temporal rename that fires in one
     unrolled body while its congruent siblings refuse (a span-local accident
     of register history) diverges the bodies and dissolves the replay window.
     No local model can price a window another pass has not yet formed, so the
     acceptance demands payment up front: a temporal rename must show a strict
     modeled issue-slot gain over the extended window.  Zero-gain temporal
     candidates refuse regrename-temporal-no-modeled-gain.

   Whole-block-free targets keep the plain no-worse acceptance: they collide
   with nothing, couple to nothing, and their renames are the uniform whole-web
   moves replay identity survives.  Because the two externalities are
   properties of the temporal borrow itself, not of who asked for it, the
   temporal tier self-prices in service mode too: a consumer-requested temporal
   rename must clear the same strict-gain bar before the consumer's own
   acceptance sees it.  One requester class is exempt: a consumer that owns the
   row's delivery shape end to end -- the MVE kernel-unroll realization, which
   replaces the row's schedule under its own strict acceptance, preserves the
   counted replay-capture shape explicitly, and undoes its webs exactly --
   declares RVTT_RENAME_SHAPE_OWNED (rvtt-protos.h) and internalizes both
   externalities.  Whole-block service requests stay unpriced.

   Under the current timing model the strict-gain bar admits no temporal
   candidate.  Over a window opened at the chain writer with a fresh
   scoreboard, the two worlds are isomorphic through the close (the rename is a
   register bijection with identical latencies; every in-span touch of either
   register belongs to the web), so their slot counts are equal there, and the
   only asymmetry -- the coupling at the target's fresh definition -- can only
   add slots.  So slots[1] >= slots[0] identically and every temporal candidate
   refuses no-modeled-gain or cost-regressed, in standalone and service mode
   alike; only shape-owning requesters receive temporal renames.  The
   acceptance is nevertheless expressed as pricing rather than a fixed refusal:
   a dependence model in which a rename can remove a modeled stall (a
   backward-extended window pricing an unshadowed WAW against the colliding
   web's writer, an issue-port contention class, a priced fill-freedom account)
   would admit exactly the candidates that pay, with no change to the admission
   code.

   The post-commit check generalizes accordingly: target references outside the
   span are recorded before the commit and re-verified untouched after it (for
   a whole-block-free target that record is empty).

   The same option gates the cyclic-interior consumer in rtl-rvtt-schedule.cc:
   interior regions of a barrier-chopped self-loop row request
   storage-collision chain renames through the service before candidate
   generation (rvtt_lreg_rename_web records the committed web so a scheduling
   refusal undoes it exactly via rvtt_lreg_rename_web_undo).

   LINEAGE.
     technique  none.  Post-allocation register renaming over def-use
                chains has no single paper this implementation takes an
                idea from; the antecedent is GCC's own, below.  What is
                specific here is the direction of the payoff: a rename
                buys nothing by itself -- it moves register fields and
                changes no delivered word -- so every question the pass
                answers is about what a later pass can then do, which
                is why legality and pricing are decoupled and why the
                standalone sweep needs guards a classical renamer does
                not.
     modelled on  gcc/regrename.cc: build_def_use, regrename_analyze,
                regrename_do_replace.  The chain formulation is taken
                literally -- one chain is one definition plus every
                true reader of it up to the chain close, and the whole
                web moves together or not at all.  What is not taken:
                regrename asks the recognizer whether an operand admits
                a register (constraint queries) and takes liveness at
                face value.  Here every fact comes from the typed-effect
                table (rvtt_insn_effects) instead; DF hard-register
                liveness is trusted only in functions with no opaque
                instruction, because raw .ttinsn words, asm and calls
                are invisible to it; and every commit is re-proved on
                the actual stream afterwards.

   HARDWARE.  The eight architectural SFPU vector registers L0-L7 (riscv.h
   SFPU_REG_NUM), after allocation, with no memory spill path -- so a rename is
   only ever a permutation of the eight names and can never manufacture
   storage.  The pass spends no delivered words: only register fields move, and
   the unchanged delivered-word count of every edited instruction is asserted
   after the commit.  What it spends instead is downstream scheduling freedom:
   LREG live ranges (a temporal target is borrowed and the loan is charged to
   the borrower), issue slots (priced through rvtt_timing::interlock_sim, with
   the temporal window extended through the target's first post-span touch),
   and the replay window, which exists only while the unrolled bodies are
   textually identical -- hence the standalone periodic-window guard and the
   temporal tier's strict-gain bar.

     - eight allocatable LREGs      riscv.h SFPU_REG_NUM
     - delivered words unchanged    asserted post-commit; every edited
                                    pattern re-recognized with its word
                                    count re-checked
     - issue-slot pricing           rvtt-timing.h
                                    rvtt_timing::interlock_sim
     - typed-effect veto            rvtt-effects.h / rvtt_insn_effects */

#define INCLUDE_ALGORITHM
#define INCLUDE_VECTOR
#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "rtl.h"
#include "tree.h"
#include "memmodel.h"
#include "emit-rtl.h"
#include "df.h"
#include "tree-pass.h"
#include "cfgrtl.h"
#include "insn-config.h"
#include "insn-attr.h"
#include "insn-codes.h"
#include "recog.h"
#include "tm_p.h"
#include "print-rtl.h"
#include "rvtt.h"
#include "rvtt-effects.h"
#include "rvtt-refuse.h"
#include "rvtt-timing.h"
#include "rvtt-cc-region.h"

/* The one place the LREG-file capacity is read (the pressure engine;
   rvtt-pressure.h itself is a GIMPLE-side header, so the constant is
   reached through its extern).  */
extern unsigned rvtt_pressure_capacity ();

namespace {

struct row_member
{
  rtx_insn *insn;
  xtt_effect_set fx;
};

/* BB is a self-loop whose payload is SFPU words plus the counter step and jump
   (the capturable-row shape).  Collect the SFPU members with their typed
   effects; fail on any opaque or scalar-extra content.  Mirrors the admission
   the stall-fill passes use, so a rename here is visible exactly where the
   fills look.  This serves only the standalone mode's phase-1 candidate
   prioritization (rows with a proven stall claim free registers first); the
   *REASON strings are informational and never emitted.  */

static bool
rename_row_p (basic_block bb, std::vector<row_member> *members,
	      const char **reason)
{
  *reason = nullptr;
  bool self = false;
  edge e;
  edge_iterator ei;
  FOR_EACH_EDGE (e, ei, bb->succs)
    if (e->dest == bb)
      self = true;
  if (!self)
    return false;

  bool saw_scalar = false;
  rtx_insn *insn;
  FOR_BB_INSNS (bb, insn)
    {
      if (!NONDEBUG_INSN_P (insn))
	continue;
      if (JUMP_P (insn))
	{
	  if (insn != BB_END (bb))
	    {
	      *reason = "control flow inside the row";
	      return false;
	    }
	  continue;
	}
      if (CALL_P (insn) || asm_noperands (PATTERN (insn)) >= 0)
	{
	  *reason = "rename-span-opaque";
	  return false;
	}
      if (GET_CODE (PATTERN (insn)) == USE
	  || GET_CODE (PATTERN (insn)) == CLOBBER)
	continue;
      if (recog_memoized (insn) >= 0 && get_attr_type (insn) == TYPE_TENSIX)
	{
	  if (!get_attr_length (insn))
	    continue; /* bookkeeping ghost */
	  row_member m;
	  m.insn = insn;
	  m.fx = rvtt_insn_effects (insn);
	  if (m.fx.opaque)
	    {
	      *reason = "rename-member-effects-unaudited";
	      return false;
	    }
	  members->push_back (m);
	  continue;
	}
      rtx set = single_set (insn);
      if (saw_scalar || !set || !REG_P (SET_DEST (set))
	  || SFPU_REG_P (REGNO (SET_DEST (set)))
	  || contains_mem_rtx_p (PATTERN (insn)))
	{
	  *reason = "scalar payload beyond the counter";
	  return false;
	}
      saw_scalar = true;
    }
  return members->size () >= 2;
}

/* An audited stall exists: some member with result_latency > 0 is
   immediately followed (in issue order) by a member reading one of its
   destinations.  */

static bool
row_has_audited_stall_p (const std::vector<row_member> &members)
{
  for (size_t i = 0; i + 1 < members.size (); ++i)
    if (members[i].fx.result_latency > 0
	&& (members[i + 1].fx.lreg_read & members[i].fx.lreg_write))
      return true;
  return false;
}

/* Replace every use of hard reg OLDR with NEWR inside *LOC.  Collect
   change requests into the validate_change group.  */

static void
queue_reg_replacements (rtx_insn *insn, rtx *loc, unsigned oldr, rtx newreg)
{
  rtx x = *loc;
  if (!x)
    return;
  if (REG_P (x))
    {
      if (REGNO (x) == oldr)
	validate_change (insn, loc, gen_rtx_REG (GET_MODE (x),
						 REGNO (newreg)), true);
      return;
    }
  const char *fmt = GET_RTX_FORMAT (GET_CODE (x));
  for (int i = GET_RTX_LENGTH (GET_CODE (x)) - 1; i >= 0; --i)
    {
      if (fmt[i] == 'e')
	queue_reg_replacements (insn, &XEXP (x, i), oldr, newreg);
      else if (fmt[i] == 'E')
	for (int j = XVECLEN (x, i) - 1; j >= 0; --j)
	  queue_reg_replacements (insn, &XVECEXP (x, i, j), oldr, newreg);
    }
}

/* ==================================================================
   The general du-chain engine.  See the file header.  */

/* One scanned block position.  */

struct span_insn
{
  rtx_insn *insn;
  enum kind_t { SI_TENSIX, SI_MARKER, SI_SCALAR, SI_OPAQUE, SI_GHOST } kind;
  xtt_effect_set fx;		/* SI_TENSIX only.  */
  uint32_t touch;		/* L0..L7 bits this position references.  */
};

/* Architectural LREG bits (L0..L7) of every explicit REG occurrence
   in X.  Paranoid vocabulary for non-effect-audited positions.  */

static uint32_t
sfpu_reg_mask (rtx x)
{
  if (!x)
    return 0;
  if (REG_P (x))
    return SFPU_REG_P (REGNO (x)) ? 1u << (REGNO (x) - SFPU_REG_FIRST) : 0;
  uint32_t mask = 0;
  const char *fmt = GET_RTX_FORMAT (GET_CODE (x));
  for (int i = GET_RTX_LENGTH (GET_CODE (x)) - 1; i >= 0; --i)
    {
      if (fmt[i] == 'e')
	mask |= sfpu_reg_mask (XEXP (x, i));
      else if (fmt[i] == 'E')
	for (int j = XVECLEN (x, i) - 1; j >= 0; --j)
	  mask |= sfpu_reg_mask (XVECEXP (x, i, j));
    }
  return mask;
}

/* Explicit REG occurrences of hard register REGNO in X.  */

static unsigned
count_reg_occurrences (rtx x, unsigned regno)
{
  if (!x)
    return 0;
  if (REG_P (x))
    return REGNO (x) == regno ? 1 : 0;
  unsigned n = 0;
  const char *fmt = GET_RTX_FORMAT (GET_CODE (x));
  for (int i = GET_RTX_LENGTH (GET_CODE (x)) - 1; i >= 0; --i)
    {
      if (fmt[i] == 'e')
	n += count_reg_occurrences (XEXP (x, i), regno);
      else if (fmt[i] == 'E')
	for (int j = XVECLEN (x, i) - 1; j >= 0; --j)
	  n += count_reg_occurrences (XVECEXP (x, i, j), regno);
    }
  return n;
}

/* Classify every nondebug insn of BB in order.  */

static void
scan_block (basic_block bb, std::vector<span_insn> *out)
{
  rtx_insn *insn;
  FOR_BB_INSNS (bb, insn)
    {
      if (!NONDEBUG_INSN_P (insn))
	continue;
      span_insn si;
      si.insn = insn;
      si.fx = xtt_effect_set ();
      si.touch = 0;
      uint32_t marker_mask = 0;
      if (CALL_P (insn) || asm_noperands (PATTERN (insn)) >= 0)
	si.kind = span_insn::SI_OPAQUE;
      else if (GET_CODE (PATTERN (insn)) == USE
	       || GET_CODE (PATTERN (insn)) == CLOBBER)
	{
	  /* Bookkeeping; any LREG mention still poisons the bits.  */
	  si.kind = span_insn::SI_SCALAR;
	  si.touch = sfpu_reg_mask (PATTERN (insn));
	}
      else if (rvtt_lreg_marker (insn, &marker_mask))
	{
	  /* Zero-length pinned-LREG protocol marker: the register is
	     part of a fixed-LREG interface and can never move.  */
	  si.kind = span_insn::SI_MARKER;
	  si.touch = marker_mask & 0xFF;
	}
      else if (recog_memoized (insn) >= 0
	       && get_attr_type (insn) == TYPE_TENSIX)
	{
	  if (!get_attr_length (insn))
	    si.kind = span_insn::SI_GHOST;	/* bookkeeping ghost */
	  else
	    {
	      si.kind = span_insn::SI_TENSIX;
	      si.fx = rvtt_insn_effects (insn);
	      si.touch = (si.fx.lreg_read | si.fx.lreg_write) & 0xFF;
	    }
	  /* Fail closed against pattern references the effect audit
	     does not carry (and ghosts' pinned bits).  */
	  si.touch |= sfpu_reg_mask (PATTERN (insn)) & 0xFF;
	}
      else
	{
	  si.kind = span_insn::SI_SCALAR;
	  si.touch = sfpu_reg_mask (PATTERN (insn)) & 0xFF;
	}
      out->push_back (si);
    }
}

/* The function contains an instruction DF hard-register liveness
   cannot see through (call, asm, raw .ttinsn word).  Gates the
   dead-at-exit chain close.  */

static bool
function_has_opaque_insn_p (function *fn)
{
  basic_block bb;
  rtx_insn *insn;
  FOR_EACH_BB_FN (bb, fn)
    FOR_BB_INSNS (bb, insn)
      if (NONDEBUG_INSN_P (insn)
	  && (CALL_P (insn) || asm_noperands (PATTERN (insn)) >= 0))
	return true;
  return false;
}

/* One analyzed, rename-admissible chain.  */

struct chain_desc
{
  basic_block bb;
  std::vector<span_insn> scan;	/* the block scan the proof used */
  size_t wi;			/* writer index into scan */
  std::vector<size_t> readers;	/* reader indices */
  size_t close;			/* kill-close index, or scan.size () */
  bool close_reads;		/* the close also reads the chain value
				   through clean OP_IN operands */
  int old_l, new_l;		/* L indices */
  bool temporal;		/* target admitted by the span-scoped
				   (temporal) tier, not whole-block
				   freeness */
};

static unsigned n_chain_renamed;

/* Register the named refusal REASON for a chain in BB, attributed to
   the blocking insn AT, and print the standard dump line.  */

static void
refuse_chain (const char *reason, rtx_insn *at, basic_block bb)
{
  rvtt_refuse_by_name_at (reason, at, dump_file,
			  "Lreg chain rename refused: %s in bb %d\n",
			  reason, bb->index);
}

/* W is an explicit-operand-editable member of a chain on hard reg
   OLDR: every effect-mask reference has a matching explicit REG
   occurrence in the pattern (the LUT implicit-slot / hidden-access
   veto).  */

static bool
explicit_operand_p (const span_insn &si, unsigned oldr)
{
  return count_reg_occurrences (PATTERN (si.insn), oldr) > 0;
}

/* Analyze the chain of the single-LREG definition at SCAN[WI] in BB.
   On success fill *CH (which takes a copy of SCAN) and return true;
   otherwise refuse by name and return false.  FN_HAS_OPAQUE gates the
   dead-at-exit close.  */

static bool
analyze_chain (basic_block bb, const std::vector<span_insn> &scan,
	       size_t wi, bool fn_has_opaque, int target_l,
	       chain_desc *ch)
{
  const span_insn &w = scan[wi];
  gcc_assert (w.kind == span_insn::SI_TENSIX);

  /* Writer vetoes, all through the one typed-effect table.  */
  if (w.fx.opaque)
    {
      refuse_chain ("regrename-span-opaque", w.insn, bb);
      return false;
    }
  if (popcount_hwi (w.fx.lreg_write) != 1
      || (w.fx.lreg_write & ~0xFFu))
    {
      refuse_chain ("regrename-effect-veto", w.insn, bb);
      return false;
    }
  uint32_t oldbit = w.fx.lreg_write;
  int old_l = exact_log2 (oldbit);
  if (old_l < 0 || old_l >= (int) SFPU_REG_NUM)
    {
      refuse_chain ("regrename-effect-veto", w.insn, bb);
      return false;
    }
  unsigned oldr = SFPU_REG_FIRST + old_l;
  if (w.fx.cc_write || w.fx.config_dests_written || w.fx.addr_mod_slot_write
      || w.fx.rwc.kind != xtt_rwc_effect_t::NONE || w.fx.dst_mem_write)
    {
      refuse_chain ("regrename-effect-veto", w.insn, bb);
      return false;
    }
  xtt_multiresult_group grp;
  if (rvtt_multiresult_group (w.insn, w.fx, &grp))
    {
      refuse_chain ("regrename-companion-coupled", w.insn, bb);
      return false;
    }
  if (get_attr_xtt_replay (w.insn) == XTT_REPLAY_OWNER)
    {
      refuse_chain ("regrename-replay-boundary", w.insn, bb);
      return false;
    }
  /* A def whose LV-merge source (the canonical operand-1 position;
     the plain forms carry a noval marker there) is its own
     destination register genuinely merges with the PRIOR destination
     value on disabled lanes: renaming the destination breaks the
     merge (the prior value lives in the source register) and refuses.
     Other input occurrences of the destination register are ordinary
     source reads of the register's PREVIOUS live range (the
     allocator's reuse, e.g. p = mul (x, x) packed into x's register):
     the commit edits ONLY the writer's output operands, so those
     reads stay on the source register and remain correct.  The
     destructive "0"-tied families (the BH XOR shape) cannot split
     destination from tied source and fail the constraint
     re-recognition (regrename-constraint), never silently.  */
  if (w.fx.lreg_read & oldbit)
    {
      extract_insn (w.insn);
      for (int oi = 1; oi < recog_data.n_operands; ++oi)
	{
	  rtx op = recog_data.operand[oi];
	  if (REG_P (op) && REGNO (op) == oldr
	      && recog_data.operand_type[oi] == OP_IN
	      && oi == 1)
	    {
	      refuse_chain ("regrename-self-merge", w.insn, bb);
	      return false;
	    }
	}
    }
  if (!explicit_operand_p (w, oldr))
    {
      refuse_chain ("regrename-implicit-operand", w.insn, bb);
      return false;
    }

  /* Walk the span: collect readers up to the close.  */
  std::vector<size_t> readers;
  size_t close = scan.size ();
  bool close_reads = false;
  /* First CC-state event inside the span, when the widening flag lets
     the walk continue past it (see the cc_write arm below).  */
  rtx_insn *first_cc = nullptr;
  for (size_t i = wi + 1; i < scan.size (); ++i)
    {
      const span_insn &si = scan[i];
      switch (si.kind)
	{
	case span_insn::SI_OPAQUE:
	  refuse_chain ("regrename-span-opaque", si.insn, bb);
	  return false;
	case span_insn::SI_MARKER:
	  if (si.touch & oldbit)
	    {
	      refuse_chain ("regrename-pinned-protocol", si.insn, bb);
	      return false;
	    }
	  continue;
	case span_insn::SI_SCALAR:
	case span_insn::SI_GHOST:
	  if (si.touch & oldbit)
	    {
	      refuse_chain ("regrename-pinned-protocol", si.insn, bb);
	      return false;
	    }
	  continue;
	case span_insn::SI_TENSIX:
	  break;
	}
      if (si.fx.opaque)
	{
	  refuse_chain ("regrename-span-opaque", si.insn, bb);
	  return false;
	}
      if (si.fx.lreg_write & oldbit)
	{
	  /* The close.  The allocator's favourite reuse packs a dying
	     source's register into the consumer's destination, so the
	     close often BOTH genuinely reads the chain value AND
	     kills the register.  A clean OP_IN read is splittable:
	     only the input operand locations move to the target and
	     the kill stays.  A read through the destination itself
	     (an OP_INOUT operand) is a tied merge the rename cannot
	     split and refuses.  */
	  extract_insn (si.insn);
	  for (int oi = 0; oi < recog_data.n_operands; ++oi)
	    {
	      rtx op = recog_data.operand[oi];
	      if (REG_P (op) && REGNO (op) == oldr
		  && recog_data.operand_type[oi] == OP_INOUT)
		{
		  refuse_chain ("regrename-tied-close", si.insn, bb);
		  return false;
		}
	      if (REG_P (op) && REGNO (op) == oldr
		  && recog_data.operand_type[oi] == OP_IN)
		close_reads = true;
	    }
	  close = i;
	  break;
	}
      if (si.fx.cc_write)
	{
	  /* A mask event inside the open span: a reader past it may
	     observe lanes the writer never wrote, and a lane-masked
	     close past it kills only the narrowed lanes -- either way
	     the disabled-lane contents of the two worlds diverge.
	     Without the widening flag, fail closed on ANY CC event
	     inside the span (the blanket rule), after asking the
	     CC-region engine's RTL view for the dump-only census
	     class.  With -mtt-tensix-optimize-rename-cc-region the
	     walk continues (the event may be a collectable reader)
	     and the view adjudicates the whole span after the close
	     is known.  */
	  if (!riscv_tt_opt_rename_cc_region)
	    {
	      if (dump_file)
		{
		  /* Dump-only census: the close the blanket rule would have
		     found, so the dump names the RTL view's class without
		     changing code generation.  */
		  size_t c = scan.size ();
		  for (size_t j = i; j < scan.size (); ++j)
		    if (scan[j].kind == span_insn::SI_TENSIX
			&& (scan[j].fx.lreg_write & oldbit))
		      {
			c = j;
			break;
		      }
		  rvtt_cc_rtl_span_verdict v
		    = rvtt_cc_rtl_classify_span
			(bb, w.insn,
			 c == scan.size () ? nullptr : scan[c].insn,
			 c != scan.size ());
		  fprintf (dump_file,
			   "Lreg chain rename cc-span rtl-view: %s"
			   " in bb %d\n",
			   rvtt_cc_rtl_span_verdict_name (v), bb->index);
		}
	      refuse_chain ("regrename-cc-span", si.insn, bb);
	      return false;
	    }
	  if (!first_cc)
	    first_cc = si.insn;
	}
      if (si.fx.lreg_read & oldbit)
	{
	  if (get_attr_xtt_replay (si.insn) == XTT_REPLAY_OWNER)
	    {
	      refuse_chain ("regrename-replay-boundary", si.insn, bb);
	      return false;
	    }
	  if (!explicit_operand_p (si, oldr))
	    {
	      refuse_chain ("regrename-implicit-operand", si.insn, bb);
	      return false;
	    }
	  readers.push_back (i);
	}
    }
  if (first_cc)
    {
      /* The widened arm: the span crossed CC activity and the flag is live.
	 The RTL view adjudicates the whole interior (writer to close, or to
	 the block end for an open chain); a kill-close demands the end state
	 equal the span-entry mask (it must overwrite exactly the
	 definition-written lanes).  Every unproven arm refuses
	 regrename-cc-span-region-unproven; the dump names the view's
	 class.  */
      gcc_assert (riscv_tt_opt_rename_cc_region);
      rvtt_cc_rtl_span_verdict v
	= rvtt_cc_rtl_classify_span (bb, w.insn,
				     close == scan.size ()
				     ? nullptr : scan[close].insn,
				     close != scan.size ());
      if (dump_file)
	fprintf (dump_file,
		 "Lreg chain rename cc-span rtl-view: %s in bb %d\n",
		 rvtt_cc_rtl_span_verdict_name (v), bb->index);
      /* The walk saw a CC event, so the view cannot answer NO_EVENT (the two
	 scans classify from the same typed-effect table).  Gated on
	 flag_checking at run time, like the shadow assert below, so the check
	 survives release-checking builds, where gcc_checking_assert would
	 compile away.  */
      if (flag_checking)
	gcc_assert (v != RVTT_CC_RTL_SPAN_NO_EVENT);
      if (!rvtt_cc_rtl_span_admissible_p (v))
	{
	  refuse_chain ("regrename-cc-span-region-unproven", first_cc, bb);
	  return false;
	}
    }
  else if (flag_checking)
    {
      /* Shadow check: every span the blanket rule allows must be NO_EVENT
	 under the RTL view; a disagreement is dumped by name, fails closed to
	 the blanket refusal, and asserts.  */
      rvtt_cc_rtl_span_verdict v
	= rvtt_cc_rtl_classify_span (bb, w.insn,
				     close == scan.size ()
				     ? nullptr : scan[close].insn,
				     close != scan.size ());
      if (v != RVTT_CC_RTL_SPAN_NO_EVENT)
	{
	  if (dump_file)
	    fprintf (dump_file,
		     "Lreg chain rename cc-span rtl-view SHADOW"
		     " DIVERGENCE: %s in bb %d\n",
		     rvtt_cc_rtl_span_verdict_name (v), bb->index);
	  refuse_chain ("regrename-cc-span", w.insn, bb);
	  gcc_assert (!"cc-span rtl-view shadow divergence");
	  return false;
	}
    }

  if (close == scan.size ())
    {
      /* No kill inside the block: admissible only as a dead-at-exit
	 close, and only when DF's hard-register liveness is beyond
	 suspicion (no opaque instruction anywhere in the function --
	 calls, asm, and raw .ttinsn words are invisible to DF
	 hard-register liveness).  The span (and so the no-CC rule
	 above) then extends to the block end.  */
      if (fn_has_opaque
	  || REGNO_REG_SET_P (df_get_live_out (bb), oldr))
	{
	  refuse_chain ("regrename-chain-open", w.insn, bb);
	  return false;
	}
    }

  /* Target selection: a free architectural LREG, deterministic lowest
     index.  Free = untouched by every position in the block (typed
     effects plus the paranoid pattern scan), not live into and not
     live out of the block, never a constant register (the L0..L7
     range is the allocatable file; the capacity is read through the
     pressure engine).  */
  uint32_t block_touch = 0;
  for (const span_insn &si : scan)
    block_touch |= si.touch;
  unsigned nlreg = SFPU_REG_NUM;
  if (rvtt_pressure_capacity () < nlreg)
    nlreg = rvtt_pressure_capacity ();
  int new_l = -1;
  bool temporal = false;
  /* Whole-block-free tier.  A target's deadness across the block boundary (not
     live-in, not live-out) is proven from DF hard-register liveness, which is
     reliable only in a function with no opaque instruction (call, asm, raw
     .ttinsn word) -- the same trust boundary the dead-at-exit close
     (fn_has_opaque above) and the temporal never-touched-after arm
     (fn_has_opaque below) enforce.  In an opaque function a register untouched
     in the block can be loop-carried live through it (produced in a successor,
     consumed around the backedge) without DF seeing it, and renaming onto it
     would clobber the loop-carried value.  Fail closed: in an opaque function
     this tier admits no target; the candidate wall is recorded so the refusal
     names the class, and the temporal tier's DF-independent
     first-post-definition arm may still admit a target.  */
  bool liveness_untrusted_wall = false;
  for (unsigned l = 0; l < nlreg; ++l)
    {
      if (target_l >= 0 && (int) l != target_l)
	continue;
      uint32_t bit = 1u << l;
      if (block_touch & bit)
	continue;
      if (fn_has_opaque)
	{
	  /* A by-touch-free candidate exists, but its cross-block
	     deadness cannot be proven (DF hard-reg liveness untrusted).  */
	  liveness_untrusted_wall = true;
	  continue;
	}
      if (REGNO_REG_SET_P (df_get_live_in (bb), SFPU_REG_FIRST + l)
	  || REGNO_REG_SET_P (df_get_live_out (bb), SFPU_REG_FIRST + l))
	continue;
      new_l = l;
      break;
    }
  /* Temporal-tier dest-reuse-writer gate.  A chain whose writer reads its own
     destination register (w.fx.lreg_read & oldbit -- the allocator's
     dest-reuses-dying-source shape p = op (x, ...) packed into x's register;
     not the operand-1 live-value merge, already refused regrename-self-merge
     above) has an asymmetric register-field edit: the commit moves the write
     off the old register but must leave the input read of the old register in
     place (it is a read of that register's previous live range).  So the
     original writer killed the old register (overwrote it with the chain
     value); the renamed writer does not -- the dying-source value it read now
     stays live on the old register from the writer through to the close (the
     next instruction to overwrite it).

     The whole-block-free tier is immune: its target is dead across the whole
     block and every other admissible target it could hand out is equally
     block-dead, so this extended old-register live range collides with
     nothing.  The temporal tier is not: it proves target freedom only over
     each chain's own span (span_clear below), a span-local fact.  It does not
     model the extended live range this dest-reuse rename leaves on the old
     register, so a later temporal chain could pick that old register (or an
     overlapping span-marginal one) as its target and alias the still-live
     dying-source value -- register-aliasing wrong code even with every lane
     enabled.  No narrower predicate (reader count, reading-close shape) is
     sufficient: any dest-reuse writer can be the extended range a later chain
     aliases.

     Fail closed by name: a non-dest-reuse writer does not extend any
     old-register range (its original def had no live predecessor) and still
     admits temporally.  The whole-block tier already ran above; refusing here
     forgoes only a temporal target for this chain, never a whole-block
     one.  */
  if (new_l < 0 && riscv_tt_opt_rename_temporal
      && (w.fx.lreg_read & oldbit))
    {
      refuse_chain ("regrename-temporal-dest-reuse", w.insn, bb);
      return false;
    }
  if (new_l < 0 && riscv_tt_opt_rename_temporal)
    {
      /* Temporal tier (see the file header): a
	 target free across the SPAN whose out-of-span lifetimes
	 provably cannot observe the chain value.  Deterministic
	 lowest admissible index.  */
      size_t span_end = close == scan.size () ? scan.size () - 1 : close;
      for (unsigned l = 0; l < nlreg; ++l)
	{
	  if (target_l >= 0 && (int) l != target_l)
	    continue;
	  uint32_t bit = 1u << l;
	  if (bit & oldbit)
	    continue;
	  /* Untouched across the span (writer through close, or to the
	     block end for a dead-at-exit chain).  */
	  bool span_clear = true;
	  for (size_t i = wi; i <= span_end && span_clear; ++i)
	    span_clear = !(scan[i].touch & bit);
	  if (!span_clear)
	    continue;
	  /* Pinned protocol markers bar the register block-wide.  */
	  bool pinned = false;
	  for (const span_insn &si : scan)
	    if (si.kind == span_insn::SI_MARKER && (si.touch & bit))
	      pinned = true;
	  if (pinned)
	    continue;
	  /* Post-span rule: the first later touch must be a fresh
	     effect-audited Tensix definition with no CC event between
	     the close and it; no later touch needs beyond-suspicion
	     DF death at block exit instead.  */
	  size_t first_post = scan.size ();
	  for (size_t i = span_end + 1; i < scan.size (); ++i)
	    if (scan[i].touch & bit)
	      {
		first_post = i;
		break;
	      }
	  if (first_post != scan.size ())
	    {
	      const span_insn &p = scan[first_post];
	      if (p.kind != span_insn::SI_TENSIX || p.fx.opaque
		  || !(p.fx.lreg_write & bit) || (p.fx.lreg_read & bit))
		continue;
	      bool gap_clear = true;
	      for (size_t i = span_end; i < first_post && gap_clear; ++i)
		{
		  if (scan[i].kind == span_insn::SI_OPAQUE)
		    gap_clear = false;
		  else if (scan[i].kind == span_insn::SI_TENSIX
			   && (scan[i].fx.opaque || scan[i].fx.cc_write))
		    gap_clear = false;
		}
	      if (!gap_clear)
		continue;
	    }
	  else
	    {
	      /* Live-in with no in-block touch would mean the write
		 clobbers a value DF still carries -- incoherent with
		 dead-out, but fail closed on it anyway.  */
	      if (fn_has_opaque
		  || REGNO_REG_SET_P (df_get_live_in (bb),
				      SFPU_REG_FIRST + l)
		  || REGNO_REG_SET_P (df_get_live_out (bb),
				      SFPU_REG_FIRST + l))
		continue;
	    }
	  new_l = l;
	  temporal = true;
	  break;
	}
    }
  if (new_l < 0)
    {
      /* Name the class precisely: a by-touch-free target existed but its
	 cross-block deadness could not be trusted in this opaque function
	 (and no temporal DF-independent target was admissible either);
	 otherwise the ordinary no-free-lreg wall.  */
      refuse_chain (liveness_untrusted_wall
		    ? "regrename-liveness-untrusted"
		    : "regrename-no-free-lreg", w.insn, bb);
      return false;
    }

  ch->bb = bb;
  ch->scan = scan;
  ch->wi = wi;
  ch->readers = readers;
  ch->close = close;
  ch->close_reads = close_reads;
  ch->old_l = old_l;
  ch->new_l = new_l;
  ch->temporal = temporal;
  return true;
}

/* Whole-row acceptance for the standalone pass mode: the modeled interlocked
   issue-slot count of the priced window under the timing engine's scoreboard,
   before and after the register-field edit.  For a whole-block-free target the
   window is the span (writer through close inclusive) and the acceptance is
   no-worse.  For a temporal target the window extends through the target's
   first post-span touch -- the fresh definition the rename newly couples to
   the span -- and the acceptance is a strict modeled gain: a temporal target
   is borrowed from a register with a live downstream story, and a rename that
   buys no modeled slots cannot pay for the interlock-fill hole it claims or
   the replay-window identity it risks (see the file header).  Returns false
   (refusing by name) when the window is unpriceable -- an unaudited producer
   feeding a priced consumer; nothing unpriceable is accepted.  */

static bool
span_no_worse_p (const chain_desc &ch)
{
  uint32_t oldbit = 1u << ch.old_l;
  uint32_t newbit = 1u << ch.new_l;
  size_t end = ch.close == ch.scan.size () ? ch.scan.size () : ch.close + 1;
  /* The temporal coupling window: through the target's first
     post-span touch (the admission's fresh-definition arm; the
     never-touched arm leaves END at the close and the span alone).  */
  if (ch.temporal && ch.close != ch.scan.size ())
    for (size_t i = ch.close + 1; i < ch.scan.size (); ++i)
      if (ch.scan[i].touch & newbit)
	{
	  end = i + 1;
	  break;
	}
  int64_t slots[2] = { 0, 0 };
  for (int world = 0; world < 2; ++world)
    {
      rvtt_timing::interlock_sim sim;
      for (size_t i = ch.wi; i < end; ++i)
	{
	  const span_insn &si = ch.scan[i];
	  if (si.kind != span_insn::SI_TENSIX)
	    continue;
	  uint32_t read = si.fx.lreg_read;
	  uint32_t write = si.fx.lreg_write;
	  if (world == 1)
	    {
	      if (i == ch.wi)
		write = (write & ~oldbit) | newbit;
	      else if (ch.close_reads && i == ch.close)
		read = (read & ~oldbit) | newbit;
	      else
		for (size_t r : ch.readers)
		  if (r == i)
		    read = (read & ~oldbit) | newbit;
	    }
	  rvtt_timing::issue_op op;
	  op.deps = (read | write) & 0xFFFF;
	  op.writes = write & 0xFFFF;
	  op.words = get_attr_length (si.insn) / 4;
	  op.lat = si.fx.result_latency;
	  op.next_slot_stall = si.fx.next_slot_stall;
	  if (!sim.step (op))
	    {
	      refuse_chain ("regrename-row-unpriceable",
			    si.insn, ch.bb);
	      return false;
	    }
	}
      slots[world] = sim.slots ();
    }
  if (slots[1] > slots[0])
    {
      refuse_chain ("regrename-cost-regressed",
		    ch.scan[ch.wi].insn, ch.bb);
      return false;
    }
  /* Temporal targets must pay: a zero-gain rename consumes downstream schedule
     freedom (the fill hole, the replay identity) it cannot repay, so require a
     strict gain (see the file header).  */
  if (ch.temporal && slots[1] == slots[0])
    {
      refuse_chain ("regrename-temporal-no-modeled-gain",
		    ch.scan[ch.wi].insn, ch.bb);
      return false;
    }
  return true;
}

/* Periodic-window guard for the standalone pass mode (see the file
   header).  True iff the chain's EDITED window sits inside a repeated
   delivered-word window: the Tensix word sequence from the writer
   through the last position the commit would edit (the last reader,
   or the close when the close's clean OP_IN reads move too) occurs a
   second time in the block's Tensix stream.  The edited window -- not
   the full span to the close -- is what matters: a chain whose
   kill close is the NEXT repetition's first word, or whose
   dead-at-exit span trails into unique block-end code, still rewrites
   only the repeated body words, and any full-span repetition implies
   an edited-window repetition (the edited window is a prefix), so
   this choice strictly widens the refusal.  Word equality is
   INSN_CODE plus delivered_pattern_equal_p below: post-RA, equal
   patterns deliver equal words.  The comparison walks only SI_TENSIX members
   (the replay recorder's currency is the launched Tensix word stream;
   interleaved scalar instructions do not enter a recording), and a
   second occurrence may overlap the window (an overlapping self-match
   is a period shorter than the window -- still a repetition).
   Single-word windows are never classified periodic (a lone recurring
   word is not a window).  */

/* Delivered-word pattern equality for the periodic-window guard:
   rtx_equal_p with one deliberate widening -- two SCRATCH rtxes of the
   same mode compare EQUAL.  rtx_equal_p hard-wires SCRATCH to false
   (each scratch is a unique object), but a post-RA scratch clobber
   emits no delivered word, and the Tensix load/store patterns all
   carry one -- under rtx_equal_p no two loads ever compare equal and
   the guard would be structurally blind to exactly the load/store
   iteration bodies it exists for.
   Everything else matches rtx_equal_p's discipline: registers by
   number, ints/wide-ints/strings by value, vectors elementwise.  */

static bool
delivered_pattern_equal_p (const_rtx x, const_rtx y)
{
  if (x == y)
    return true;
  if (!x || !y)
    return false;
  enum rtx_code code = GET_CODE (x);
  if (code != GET_CODE (y) || GET_MODE (x) != GET_MODE (y))
    return false;
  switch (code)
    {
    case SCRATCH:
      return true;
    case REG:
      return REGNO (x) == REGNO (y);
    case LABEL_REF:
      return label_ref_label (x) == label_ref_label (y);
    case SYMBOL_REF:
      return XSTR (x, 0) == XSTR (y, 0);
    default:
      break;
    }
  const char *fmt = GET_RTX_FORMAT (code);
  for (int i = GET_RTX_LENGTH (code) - 1; i >= 0; --i)
    switch (fmt[i])
      {
      case 'e':
	if (!delivered_pattern_equal_p (XEXP (x, i), XEXP (y, i)))
	  return false;
	break;
      case 'E':
	if (XVECLEN (x, i) != XVECLEN (y, i))
	  return false;
	for (int j = XVECLEN (x, i) - 1; j >= 0; --j)
	  if (!delivered_pattern_equal_p (XVECEXP (x, i, j),
					  XVECEXP (y, i, j)))
	    return false;
	break;
      case 'i':
      case 'n':
	if (XINT (x, i) != XINT (y, i))
	  return false;
	break;
      case 'w':
	if (XWINT (x, i) != XWINT (y, i))
	  return false;
	break;
      case 's':
	if (strcmp (XSTR (x, i), XSTR (y, i)) != 0)
	  return false;
	break;
      case '0':
	break;
      default:
	/* An operand kind this comparator does not model: fail closed
	   to inequality (the guard then sees no repetition, which only
	   forgoes a refusal of the rename, never soundness).  */
	return false;
      }
  return true;
}

static bool
chain_in_periodic_window_p (const chain_desc &ch)
{
  /* Last position the commit would edit.  */
  size_t edit_end = ch.wi;
  if (!ch.readers.empty ())
    edit_end = ch.readers.back ();
  if (ch.close_reads && ch.close != ch.scan.size () && ch.close > edit_end)
    edit_end = ch.close;
  /* The block's Tensix members in delivery order, with the edited
     window's first and last member located in that stream.  */
  std::vector<size_t> tensix;
  size_t s0 = SIZE_MAX, send = SIZE_MAX;
  for (size_t i = 0; i < ch.scan.size (); ++i)
    if (ch.scan[i].kind == span_insn::SI_TENSIX)
      {
	if (i == ch.wi)
	  s0 = tensix.size ();
	if (i >= ch.wi && i <= edit_end)
	  send = tensix.size ();
	tensix.push_back (i);
      }
  gcc_assert (s0 != SIZE_MAX && send != SIZE_MAX && send >= s0);
  size_t len = send - s0 + 1;
  if (len < 2)
    return false;
  auto same_word = [&] (size_t a, size_t b)
    {
      rtx_insn *x = ch.scan[tensix[a]].insn;
      rtx_insn *y = ch.scan[tensix[b]].insn;
      return INSN_CODE (x) == INSN_CODE (y)
	     && delivered_pattern_equal_p (PATTERN (x), PATTERN (y));
    };
  for (size_t t = 0; t + len <= tensix.size (); ++t)
    {
      if (t == s0)
	continue;
      bool match = true;
      for (size_t k = 0; k < len && match; ++k)
	match = same_word (t + k, s0 + k);
      if (match)
	return true;
    }
  return false;
}

/* Commit CH: move the def-use web from OLD_L to NEW_L, then re-prove
   the chain shape on the ACTUAL committed stream (the final-lockstep
   discipline; see the file header).  Any divergence
   reverts, refuses by name, and hard-asserts under -fchecking.
   Returns true iff the rename stands.  WEB, when non-null, receives
   the committed web (the consumer undo record; a web too large for
   the record refuses before any edit).  */

static bool
commit_chain (const chain_desc &ch, rvtt_lreg_rename_web *web = nullptr)
{
  basic_block bb = ch.bb;
  unsigned oldr = SFPU_REG_FIRST + ch.old_l;
  unsigned newr = SFPU_REG_FIRST + ch.new_l;
  uint32_t oldbit = 1u << ch.old_l;
  uint32_t newbit = 1u << ch.new_l;
  rtx newreg = gen_rtx_REG (XTT32SImode, newr);
  rtx_insn *w_insn = ch.scan[ch.wi].insn;

  /* Recorded pre-commit facts for the lockstep re-verification.  */
  std::vector<size_t> edited;
  edited.push_back (ch.wi);
  for (size_t r : ch.readers)
    edited.push_back (r);
  std::vector<int> pre_len;
  std::vector<uint32_t> pre_write;
  for (size_t e : edited)
    {
      pre_len.push_back (get_attr_length (ch.scan[e].insn));
      pre_write.push_back (ch.scan[e].fx.lreg_write);
    }

  /* Fail closed on a web the consumer record cannot carry, BEFORE any
     edit (the record is the undo contract).  */
  if (web
      && edited.size () + (ch.close_reads ? 1 : 0)
	 > RVTT_LREG_RENAME_WEB_MAX)
    {
      refuse_chain ("regrename-web-record-overflow", w_insn, bb);
      return false;
    }

  /* Pre-commit census of target references OUTSIDE the span: empty
     for a whole-block-free target, the temporal tier's out-of-span
     lifetimes otherwise.  The post-commit belt re-verifies these
     positions are exactly preserved.  */
  size_t span_end
    = ch.close == ch.scan.size () ? ch.scan.size () - 1 : ch.close;
  std::vector<rtx_insn *> outside;
  for (size_t i = 0; i < ch.scan.size (); ++i)
    if ((i < ch.wi || i > span_end) && (ch.scan[i].touch & newbit))
      outside.push_back (ch.scan[i].insn);
  gcc_assert (ch.temporal || outside.empty ());

  /* The WRITER's edit is output-operands-only: an input occurrence of
     the source register is a read of its previous live range and must
     stay (see the self-merge comment in analyze_chain).  Readers are
     whole-pattern (their only occurrences are reads of the chain
     value).  */
  {
    rtx_insn *insn = ch.scan[ch.wi].insn;
    extract_insn (insn);
    bool renamed_op[MAX_RECOG_OPERANDS] = {};
    for (int oi = 0; oi < recog_data.n_operands; ++oi)
      {
	rtx op = recog_data.operand[oi];
	if (REG_P (op) && REGNO (op) == oldr
	    && recog_data.operand_type[oi] != OP_IN)
	  {
	    validate_change (insn, recog_data.operand_loc[oi],
			     gen_rtx_REG (GET_MODE (op), newr), true);
	    renamed_op[oi] = true;
	  }
      }
    for (int di = 0; di < recog_data.n_dups; ++di)
      if (renamed_op[recog_data.dup_num[di]])
	{
	  rtx dup = *recog_data.dup_loc[di];
	  validate_change (insn, recog_data.dup_loc[di],
			   gen_rtx_REG (GET_MODE (dup), newr), true);
	}
  }
  for (size_t e : edited)
    if (e != ch.wi)
      queue_reg_replacements (ch.scan[e].insn, &PATTERN (ch.scan[e].insn),
			      oldr, newreg);
  if (ch.close_reads)
    {
      /* The close both kills the register and genuinely reads the
	 chain value: only its clean OP_IN operand locations (and
	 their match_dups) move; the kill stays put.  */
      rtx_insn *close_insn = ch.scan[ch.close].insn;
      extract_insn (close_insn);
      bool renamed_op[MAX_RECOG_OPERANDS] = {};
      for (int oi = 0; oi < recog_data.n_operands; ++oi)
	{
	  rtx op = recog_data.operand[oi];
	  if (REG_P (op) && REGNO (op) == oldr
	      && recog_data.operand_type[oi] == OP_IN)
	    {
	      validate_change (close_insn, recog_data.operand_loc[oi],
			       gen_rtx_REG (GET_MODE (op), newr), true);
	      renamed_op[oi] = true;
	    }
	}
      for (int di = 0; di < recog_data.n_dups; ++di)
	if (renamed_op[recog_data.dup_num[di]])
	  {
	    rtx dup = *recog_data.dup_loc[di];
	    validate_change (close_insn, recog_data.dup_loc[di],
			     gen_rtx_REG (GET_MODE (dup), newr), true);
	  }
    }
  if (!apply_change_group ())
    {
      refuse_chain ("regrename-constraint", w_insn, bb);
      return false;
    }

  /* ---- Post-commit structural lockstep re-verification.  */
  const char *diverged = nullptr;
  for (size_t ei = 0; ei < edited.size () && !diverged; ++ei)
    {
      rtx_insn *insn = ch.scan[edited[ei]].insn;
      if (recog_memoized (insn) < 0
	  || get_attr_type (insn) != TYPE_TENSIX
	  || get_attr_length (insn) != pre_len[ei])
	{
	  diverged = "edited insn shape";
	  break;
	}
      xtt_effect_set fx = rvtt_insn_effects (insn);
      if (fx.opaque)
	diverged = "edited insn effects opaque";
      else if (edited[ei] == ch.wi)
	{
	  /* Every output operand must be the target and none the
	     source; an EXPLICIT input occurrence of the target means
	     the edit rewrote a genuine source read (input reads of
	     the source register's previous live range legitimately
	     remain -- the read mask may keep the source bit).  */
	  if (fx.lreg_write != newbit)
	    diverged = "writer web";
	  else
	    {
	      extract_insn (insn);
	      for (int oi = 0; oi < recog_data.n_operands; ++oi)
		{
		  rtx op = recog_data.operand[oi];
		  if (!REG_P (op))
		    continue;
		  if (recog_data.operand_type[oi] == OP_IN
		      && REGNO (op) == newr)
		    diverged = "writer source rewritten";
		  if (recog_data.operand_type[oi] != OP_IN
		      && REGNO (op) == oldr)
		    diverged = "writer output kept source";
		}
	    }
	}
      else if (!(fx.lreg_read & newbit) || (fx.lreg_read & oldbit)
	       || fx.lreg_write != pre_write[ei])
	diverged = "reader web";
      if (!diverged && edited[ei] != ch.wi
	  && count_reg_occurrences (PATTERN (insn), oldr))
	diverged = "stale source reference";
    }
  if (!diverged && ch.close != ch.scan.size ())
    {
      rtx_insn *close_insn = ch.scan[ch.close].insn;
      xtt_effect_set fx = rvtt_insn_effects (close_insn);
      if (!(fx.lreg_write & oldbit))
	diverged = "close no longer kills";
      else if (ch.close_reads)
	{
	  /* The moved reads must be on the target now and no clean
	     OP_IN read of the source may remain.  */
	  if (!(fx.lreg_read & newbit))
	    diverged = "reading close web";
	  else if (recog_memoized (close_insn) < 0)
	    diverged = "reading close shape";
	  else
	    {
	      extract_insn (close_insn);
	      for (int oi = 0; oi < recog_data.n_operands; ++oi)
		{
		  rtx op = recog_data.operand[oi];
		  if (REG_P (op) && REGNO (op) == oldr
		      && recog_data.operand_type[oi] == OP_IN)
		    diverged = "stale reading-close reference";
		}
	    }
	}
    }
  if (!diverged)
    {
      /* Every position referencing the target must be an edited web
	 member, the reading close, or a recorded out-of-span lifetime
	 position (temporal targets only; exactly preserved).  */
      std::vector<span_insn> rescan;
      scan_block (bb, &rescan);
      unsigned hits = 0, strays = 0;
      for (const span_insn &si : rescan)
	{
	  if (!(si.touch & newbit))
	    continue;
	  ++hits;
	  bool known = ch.close_reads && si.insn == ch.scan[ch.close].insn;
	  for (size_t e : edited)
	    if (si.insn == ch.scan[e].insn)
	      known = true;
	  for (rtx_insn *o : outside)
	    if (si.insn == o)
	      known = true;
	  if (!known)
	    ++strays;
	}
      if (strays
	  || hits != edited.size () + (ch.close_reads ? 1 : 0)
		     + outside.size ())
	diverged = "target reference census";
    }
  /* Untrusted-liveness check: the in-block census above scans only BB
     (scan_block (bb)), so it cannot see a loop-carried live-through target --
     that register's other references live in other basic blocks -- and passes
     even when the rename clobbered a value live across the block.  The
     cross-block deadness of a whole-block (non-temporal) target rests entirely
     on DF hard-register live-in/out, trustworthy only in a function with no
     opaque instruction, so fail closed here too: a committed non-temporal
     target in an opaque function is a soundness violation.  (The temporal
     tier's admitted arm re-establishes the target with an in-block fresh
     definition -- DF-independent -- so temporal targets are exempt.)  */
  if (!diverged && !ch.temporal && function_has_opaque_insn_p (cfun))
    diverged = "whole-block target committed in opaque function"
	       " (DF hard-register liveness untrusted)";
  if (diverged)
    {
      /* Fail closed: revert the web, refuse by name.  (For a reading
	 close, its only target references are the moved inputs, so
	 the full-pattern replacement reverts exactly those.)  */
      rtx oldreg_rtx = gen_rtx_REG (XTT32SImode, oldr);
      for (size_t e : edited)
	queue_reg_replacements (ch.scan[e].insn,
				&PATTERN (ch.scan[e].insn), newr,
				oldreg_rtx);
      if (ch.close_reads)
	queue_reg_replacements (ch.scan[ch.close].insn,
				&PATTERN (ch.scan[ch.close].insn), newr,
				oldreg_rtx);
      bool reverted = apply_change_group ();
      if (dump_file)
	fprintf (dump_file,
		 "Lreg chain rename POST-COMMIT DIVERGENCE (%s),"
		 " reverted=%d in bb %d\n", diverged, reverted, bb->index);
      refuse_chain ("regrename-postcommit-divergence", w_insn, bb);
      if (flag_checking)
	gcc_assert (reverted && !"lreg chain rename post-commit divergence");
      return false;
    }

  /* Keep DF insn-level references current: consumers (the scheduler's
     node collections) read register webs through DF refs, not raw
     patterns.  Block-level liveness needs no update (no upward
     exposure or cross-block extension is ever added).  */
  for (size_t e : edited)
    df_insn_rescan (ch.scan[e].insn);
  if (ch.close_reads)
    df_insn_rescan (ch.scan[ch.close].insn);

  if (dump_file)
    {
      int words = 0;
      for (size_t e : edited)
	words += get_attr_length (ch.scan[e].insn) / 4;
      fprintf (dump_file,
	       "Lreg chain rename%s: L%d -> L%d in bb %d (def uid=%d,"
	       " %zu readers, close=%s)\n",
	       ch.temporal ? " (temporal)" : "",
	       ch.old_l, ch.new_l, bb->index, INSN_UID (w_insn),
	       ch.readers.size (),
	       ch.close == ch.scan.size () ? "dead-at-exit"
	       : ch.close_reads ? "kill+read" : "kill");
      fprintf (dump_file,
	       "Lreg chain census: %d words before == %d words after,"
	       " register fields only\n", words, words);
    }
  if (web)
    {
      web->old_l = ch.old_l;
      web->new_l = ch.new_l;
      web->n_insns = 0;
      for (size_t e : edited)
	web->insns[web->n_insns++] = ch.scan[e].insn;
      if (ch.close_reads)
	web->insns[web->n_insns++] = ch.scan[ch.close].insn;
    }
  n_chain_renamed++;
  return true;
}

const pass_data pass_data_rvtt_lreg_rename_chains =
{
  RTL_PASS,
  "rvtt_lreg_rename_chains",
  OPTGROUP_OTHER,
  TV_NONE,
  0,
  0,
  0,
  0,
  0,
};

class pass_rvtt_lreg_rename_chains : public rtl_opt_pass
{
public:
  pass_rvtt_lreg_rename_chains (gcc::context *ctxt)
    : rtl_opt_pass (pass_data_rvtt_lreg_rename_chains, ctxt)
  {}

  bool gate (function *) final override
  {
    /* -mtt-tensix-optimize-lreg-rename also requests this engine.  */
    return TARGET_XTT_TENSIX
	   && (riscv_tt_opt_lreg_rename_chains > 0
	       || riscv_tt_opt_lreg_rename > 0);
  }

  /* The candidate at SCAN[I] has the fill-blocking profile: a latency-0,
     invariant-input, pure-value colliding definition in a capturable self-loop
     row with an audited stall.  Phase 1 of the standalone mode renames exactly
     these first, in row order, so the scarce free registers are never spent on
     general chains before the chains the stall fills are blocked on have
     claimed their targets.  */
  static bool
  v1_profile_candidate_p (const std::vector<span_insn> &scan, size_t i)
  {
    const span_insn &w = scan[i];
    uint32_t row_writes = 0;
    bool row_writes_cc = false;
    for (const span_insn &si : scan)
      if (si.kind == span_insn::SI_TENSIX && si.insn != w.insn)
	{
	  row_writes |= si.fx.lreg_write;
	  row_writes_cc |= si.fx.cc_write;
	}
    if (w.fx.result_latency != 0
	|| w.fx.cc_write
	|| (w.fx.cc_read && row_writes_cc)
	|| w.fx.config_dests_written || w.fx.addr_mod_slot_write
	|| w.fx.rwc.kind != xtt_rwc_effect_t::NONE
	|| w.fx.dst_mem_read || w.fx.dst_mem_write
	|| popcount_hwi (w.fx.lreg_write) != 1)
      return false;
    /* Invariant inputs: nothing another member writes.  */
    if ((w.fx.lreg_read & ~w.fx.lreg_write) & row_writes)
      return false;
    /* The wall.  */
    return (w.fx.lreg_write & row_writes) != 0;
  }

  unsigned execute (function *fn) final override
  {
    df_analyze ();
    n_chain_renamed = 0;
    bool fn_has_opaque = function_has_opaque_insn_p (fn);
    basic_block bb;
    FOR_EACH_BB_FN (bb, fn)
      {
	/* Standalone mode, two phases per block, re-scanning after every
	   commit (effects moved):
	   1. fill-blocking-profile candidates in row order, on capturable rows
	      with an audited stall, so the scarce free registers go to the
	      chains the stall fills need first;
	   2. the general greedy sweep over every remaining storage-collision
	      chain.

	   Both phases run the identical analyze/price/commit/verify path; only
	   candidate selection differs.  */
	std::vector<row_member> row;
	const char *row_reason = nullptr;
	bool v1_row = rename_row_p (bb, &row, &row_reason)
		      && row_has_audited_stall_p (row);
	for (int phase = v1_row ? 0 : 1; phase < 2; ++phase)
	  {
	    bool progress = true;
	    unsigned attempts = 0;
	    std::vector<bool> tried;
	    while (progress && ++attempts < 64)
	      {
		progress = false;
		std::vector<span_insn> scan;
		scan_block (bb, &scan);
		if (tried.size () < scan.size ())
		  tried.resize (scan.size (), false);
		for (size_t i = 0; i < scan.size (); ++i)
		  {
		    if (tried[i] || scan[i].kind != span_insn::SI_TENSIX
			|| scan[i].fx.opaque
			|| popcount_hwi (scan[i].fx.lreg_write) != 1)
		      continue;
		    uint32_t bit = scan[i].fx.lreg_write;
		    if (bit & ~0xFFu)
		      continue;
		    if (phase == 0)
		      {
			if (!v1_profile_candidate_p (scan, i))
			  continue;
		      }
		    else
		      {
			/* The wall: another position also writes this
			   register (the storage collision the fills
			   refuse by).  Non-colliding chains are not
			   candidates in standalone mode; consumers may
			   still request them through the service.  */
			bool collision = false;
			for (size_t j = 0; j < scan.size () && !collision;
			     ++j)
			  if (j != i && scan[j].kind == span_insn::SI_TENSIX
			      && (scan[j].fx.lreg_write & bit))
			    collision = true;
			if (!collision)
			  continue;
		      }
		    tried[i] = true;
		    chain_desc ch;
		    if (!analyze_chain (bb, scan, i, fn_has_opaque, -1, &ch))
		      continue;
		    /* Phase 1 admits only chains with a pure kill close inside
		       the row (no reading close, no open chain at the block
		       boundary); general chains wait for phase 2.  */
		    if (phase == 0
			&& (ch.close == ch.scan.size () || ch.close_reads))
		      continue;
		    /* Standalone-mode periodic-window guard (see the
		       file header): never spend the block's
		       repetitions -- the replay recorder's currency --
		       on a greedy rename.  The service path is exempt
		       (consumers price and undo).  */
		    if (chain_in_periodic_window_p (ch))
		      {
			refuse_chain ("regrename-periodic-window",
				      scan[i].insn, bb);
			continue;
		      }
		    if (!span_no_worse_p (ch))
		      continue;
		    if (commit_chain (ch))
		      {
			progress = true;
			break;	/* re-scan */
		      }
		  }
	      }
	    /* The tried set is per phase, so phase 2 considers the candidates
	       phase 1's profile skipped; a chain phase 1 analyzed and refused
	       is refused again by the same analysis.  */
	  }
      }
    if (dump_file)
      fprintf (dump_file, "Lreg chain rename: renames=%u\n",
	       n_chain_renamed);
    if (n_chain_renamed)
      df_analyze ();
    return 0;
  }
};

} /* anonymous namespace */

/* Service export: attempt to rename the du-chain of DEF_INSN's single-LREG
   definition inside BB onto TARGET_LREG (an L index; -1 = lowest proven-free).
   Carries the complete legality proof and the post-commit structural
   re-verification; refuses by name and changes nothing on any unproven clause.
   No pricing for whole-block-free targets: the requesting consumer prices
   (legality and pricing are decoupled).  A temporal target, however,
   self-prices under the strict-gain acceptance even here -- its downstream
   costs belong to the tier and are invisible to any consumer's acceptance (see
   the file header).  DF liveness must be current on entry; a committed rename
   leaves it valid (no upward exposure or cross-block extension is ever added).
   Returns true iff a rename committed.  WEB, when non-null, receives the
   committed web for the consumer's exact undo (rvtt_lreg_rename_web_undo); a
   web too large for the record refuses by name before any edit.  */

bool
rvtt_lreg_rename_chain (basic_block bb, rtx_insn *def_insn, int target_lreg,
			rvtt_lreg_rename_web *web,
			enum rvtt_lreg_rename_pricing pricing)
{
  std::vector<span_insn> scan;
  scan_block (bb, &scan);
  size_t wi = scan.size ();
  for (size_t i = 0; i < scan.size (); ++i)
    if (scan[i].insn == def_insn)
      {
	wi = i;
	break;
      }
  if (wi == scan.size () || scan[wi].kind != span_insn::SI_TENSIX)
    {
      rvtt_refuse_by_name_at ("regrename-effect-veto", def_insn, dump_file,
			      "Lreg chain rename refused:"
			      " regrename-effect-veto in bb %d\n", bb->index);
      return false;
    }
  chain_desc ch;
  if (!analyze_chain (bb, scan, wi, function_has_opaque_insn_p (cfun),
		      target_lreg, &ch))
    return false;
  /* The temporal tier self-prices in service mode too (see the file header):
     the borrow's downstream costs -- the claimed interlock-fill hole, the
     risked replay-window identity -- are properties of the temporal target
     itself, not of the requesting consumer, and a consumer's own acceptance
     (e.g. a modeled row-II decrease) cannot see them.  The one exemption is a
     requester that owns the row's delivery shape end to end
     (RVTT_RENAME_SHAPE_OWNED, the rvtt-protos.h contract): it internalizes
     both costs and prices the whole composition itself.  Whole-block-free
     targets stay unpriced service -- the consumer prices.  */
  if (ch.temporal && pricing == RVTT_RENAME_PRICE_TEMPORAL
      && !span_no_worse_p (ch))
    return false;
  return commit_chain (ch, web);
}

/* Exact inverse of a committed service rename: move every occurrence
   of the target register inside the recorded web members back to the
   source register.  Sound because the belt proved the web members'
   only target references are the moved fields (a temporal target's
   out-of-span lifetimes live in OTHER instructions by construction).
   The inverse re-recognizes by construction (the forward commit
   re-recognized both worlds); divergence is a hard assert.  */

void
rvtt_lreg_rename_web_undo (const rvtt_lreg_rename_web &web)
{
  unsigned oldr = SFPU_REG_FIRST + web.old_l;
  unsigned newr = SFPU_REG_FIRST + web.new_l;
  rtx oldreg = gen_rtx_REG (XTT32SImode, oldr);
  for (unsigned i = 0; i < web.n_insns; ++i)
    queue_reg_replacements (web.insns[i], &PATTERN (web.insns[i]),
			    newr, oldreg);
  bool ok = apply_change_group ();
  gcc_assert (ok);
  for (unsigned i = 0; i < web.n_insns; ++i)
    df_insn_rescan (web.insns[i]);
  if (dump_file)
    fprintf (dump_file, "Lreg chain rename undo: L%d -> L%d (%u insns)\n",
	     web.new_l, web.old_l, web.n_insns);
}


/* Instantiate the chain-rename pass for CTXT; rvtt-passes.def places it
   before postreload, and it gates on the Tensix extension plus
   -mtt-tensix-optimize-lreg-rename-chains (or the frozen alias
   -mtt-tensix-optimize-lreg-rename).  */

rtl_opt_pass *
make_pass_rvtt_lreg_rename_chains (gcc::context *ctxt)
{
  return new pass_rvtt_lreg_rename_chains (ctxt);
}
