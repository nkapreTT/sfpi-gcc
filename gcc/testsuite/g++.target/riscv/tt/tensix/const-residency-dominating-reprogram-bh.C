// Two residency candidates of the same value: the loop class programs
// it at the end of the preheader, then the hoisted constant is
// programmed in place earlier on the same path.  The in-place write
// covers every reader of the preheader one, so only one SFPCONFIG
// remains.
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-unroll-loops -mtt-tensix-optimize-const-residency -mtt-tensix-optimize-hoisted-prgm-reuse -fdump-tree-rvtt_prgm_const-details" }
// { dg-final { scan-tree-dump-times "allocated PRGM L12 for constant 0x40000000 .loop class" 1 "rvtt_prgm_const" } }
// { dg-final { scan-tree-dump-times "PRGM L12 programming for 0x40000000 moved up" 1 "rvtt_prgm_const" } }
// { dg-final { scan-assembler-times "SFPCONFIG" 1 } }

void twice (void)
{
  auto x = __builtin_rvtt_sfpreadlreg (0);
  auto h = __builtin_rvtt_sfpxloadi (nullptr, 0x40000000, 0, 0, 31);
  for (unsigned ix = 0; ix != 32; ++ix)
    {
      auto t = __builtin_rvtt_sfpxloadi (nullptr, 0x40000000, 0, 0, 31);
      x = __builtin_rvtt_sfpmul (x, t, 0);
      x = __builtin_rvtt_sfpmul (x, h, 0);
    }
  __builtin_rvtt_sfpwritelreg (x, 0);
}
