// List-scheduler test: Dst store->load pair motion bait.  The SFPSTORE
// to Dst offset 64 and the SFPLOAD from the SAME offset form a memory RAW
// dependence that is INVISIBLE to a pure-LREG dependence DAG (disjoint
// registers).  The kernel is constructed so a naive latency scheduler
// WOULD hoist the reload: the P-chain before the store carries modeled
// one-slot mad stalls, and the reload (plus its dependent tail) is the
// only independent word available to fill them -- hoisting it above
// the store reads stale Dst data.  The list scheduler must name both
// words as dst-access barriers and keep the order unchanged.
//
// If Dst-touching words are ever admitted as schedulable nodes,
// admission without a same-address pair proof must keep refusing
// exactly this shape.
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-exceptions -fno-rtti -fno-shrink-wrap -mtt-tensix-optimize-list-schedule -mno-tt-tensix-optimize-replay -fdump-rtl-rvtt_schedule-details" }
// Effect classification precedes the defless check, so the loads and
// the stores all name dst-access (the second scan also accepts
// "scalar-or-defless" for the defless stores).
// Identical chain shapes defer by name to replay capture formation (the
// repeated-row rule) instead of being scheduled; either way nothing
// moves.
// { dg-final { scan-rtl-dump-times "List-schedule barrier: dst-access uid=\\d+" 4 "rvtt_schedule" } }
// { dg-final { scan-rtl-dump-times "List-schedule barrier: (?:dst-access|scalar-or-defless) uid=\\d+" 4 "rvtt_schedule" } }
// { dg-final { scan-rtl-dump-times "List-schedule deferred: repeated-row shape at uid=\\d+" 2 "rvtt_schedule" } }
// { dg-final { scan-rtl-dump-not "List-schedule: bb" "rvtt_schedule" } }

using vec_t = decltype (__builtin_rvtt_sfpreadlreg (0));

void dst_pair_bait ()
{
  vec_t x = __builtin_rvtt_sfpload (nullptr, 0, 0, 0, 0, 7);
  vec_t p = __builtin_rvtt_sfpmad (x, x, x, 0);
  p = __builtin_rvtt_sfpmad (p, x, p, 0);
  p = __builtin_rvtt_sfpmad (p, x, p, 0);
  __builtin_rvtt_sfpstore (nullptr, p, 64, 0, 0, 0, 7);
  /* Same-offset reload: memory RAW through Dst[64], no LREG edge.  */
  vec_t r = __builtin_rvtt_sfpload (nullptr, 64, 0, 0, 0, 7);
  vec_t s = __builtin_rvtt_sfpmad (r, x, r, 0);
  s = __builtin_rvtt_sfpmad (s, x, s, 0);
  s = __builtin_rvtt_sfpmad (s, x, s, 0);
  __builtin_rvtt_sfpstore (nullptr, s, 128, 0, 0, 0, 7);
}
