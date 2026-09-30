// List-scheduler test: the d5 dual-Horner P/Q pair on Wormhole.  Same
// kernel and dependence argument as the BH d5 test; the architectural
// difference is that WH has no scoreboard, so every unhidden mad-family
// shadow is an EXPLICIT required SFPNOP word in the assembly.  The
// interleave therefore shows up as a reduction in delivered words, not
// just a modeled number.  Expect: fire on the region, and no SFPNOP in
// the scheduled function body.
// { dg-options "-mcpu=tt-wh-tensix -O2 -I [SFPI]/include -fno-exceptions -fno-rtti -fno-shrink-wrap -mtt-tensix-optimize-list-schedule -mno-tt-tensix-optimize-replay -fdump-rtl-rvtt_schedule-details" }
// Expected: 40 -> 22, and the required-SFPNOP count drops 18 -> 0 (the
// WH baseline's 18 NOP words are real words).
// { dg-final { scan-assembler-not "SFPNOP" } }
// { dg-final { scan-rtl-dump-times "List-schedule: bb \\d+ nodes=22 makespan 40 -> 22 target=wh" 1 "rvtt_schedule" } }

namespace ckernel { volatile unsigned long *instrn_buffer; }
#include <sfpi.h>
using namespace sfpi;

void pq_rational_d5_wh ()
{
  vFloat x = dst_reg[0];
  vFloat p = x * 2.75f + -1.125f;
  p = p * x + 4.5f;
  p = p * x + 0.375f;
  p = p * x + -0.625f;
  p = p * x + 1.5f;
  vFloat q = x * -0.4375f + 1.25f;
  q = q * x + -6.5f;
  q = q * x + 0.9375f;
  q = q * x + 5.5f;
  q = q * x + -3.25f;
  dst_reg[0] = p;
  dst_reg[1] = q;
}
