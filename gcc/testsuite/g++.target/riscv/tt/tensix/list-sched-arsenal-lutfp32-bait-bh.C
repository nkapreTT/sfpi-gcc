// List-scheduler test: unaudited-latency motion bait (SFPLUTFP32).
//
// The latency table (rvtt-cost.md) deliberately leaves SFPLUTFP32
// unaudited: its Mod1/Mod1Mirror scheduling split (SFPLUTFP32.md keys
// the stalling logic on Mod1Mirror, not Mod1) and its per-mode register
// envelopes (3-entry/6-entry/FP32 tables read different LReg sets) need
// their own per-mod audit; the SFPLUT audit does NOT transfer.  A
// scheduler must therefore never GUESS a latency for it: the instruction
// is a named barrier, scheduled conservatively in place.
//
// Constructed bait: the mad chain before the LUT word carries modeled
// one-slot stalls, and the SFPLUTFP32 -- whose operands are all
// chain-independent -- is exactly the filler a naive scheduler would
// sink into them.  (Its expansion also materializes the L7 indirection
// word, an audited loadi the scheduler may only treat as the LUT
// word's own dependence, never as a movable filler across it.)  Expect
// the named barrier and unchanged order.
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-exceptions -fno-rtti -fno-shrink-wrap -mtt-tensix-optimize-list-schedule -mno-tt-tensix-optimize-replay -fdump-rtl-rvtt_schedule-details" }
// SFPLUTFP32 prints "effect-opaque", the stronger refusal: its whole
// effect record is unaudited (per-mode register envelopes included), so
// admission refuses at the effect check before reaching the latency
// check.  "unaudited-latency" is for effect-audited classes whose
// latency alone is missing (e.g. SFPSHFT2).  Either name is a correct
// conservative refusal, so the scan accepts both.
// { dg-final { scan-rtl-dump-times "List-schedule barrier: (?:effect-opaque|unaudited-latency) uid=\\d+" 1 "rvtt_schedule" } }
// { dg-final { scan-rtl-dump-times "List-schedule refused: no modeled makespan decrease" 1 "rvtt_schedule" } }
// { dg-final { scan-rtl-dump-not "List-schedule: bb" "rvtt_schedule" } }

void lutfp32_bait ()
{
  auto t0 = __builtin_rvtt_sfpreadlreg (0);
  auto t1 = __builtin_rvtt_sfpreadlreg (1);
  auto t2 = __builtin_rvtt_sfpreadlreg (2);
  auto v  = __builtin_rvtt_sfpreadlreg (3);
  auto p  = __builtin_rvtt_sfpreadlreg (4);
  auto x  = __builtin_rvtt_sfpreadlreg (5);
  /* Serial mad chain: bait shadows.  */
  p = __builtin_rvtt_sfpmad (p, x, p, 0);
  p = __builtin_rvtt_sfpmad (p, x, p, 0);
  p = __builtin_rvtt_sfpmad (p, x, p, 0);
  /* Chain-independent LUT word: the tempting filler.  Latency
     unaudited by name -- must not move, must not be guessed.  */
  auto r = __builtin_rvtt_sfplutfp32_3r (t0, t1, t2, v, 10);
  __builtin_rvtt_sfpwritelreg (p, 4);
  __builtin_rvtt_sfpwritelreg (r, 3);
}
