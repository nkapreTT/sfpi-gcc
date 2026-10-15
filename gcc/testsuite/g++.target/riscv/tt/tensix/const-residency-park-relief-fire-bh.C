// PRESSURE-PARK relief: the loop's in-loop constant finds every PRGM
// register claimed by owner_init and the 8-LREG file full, so the LREG
// tier would refuse it.  The hoisted `a' already lives in L12 TU-wide
// (owner_init stores the same value), so it is read from there instead,
// freeing an LREG, and the in-loop constant parks in the preheader.
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-unroll-loops -mtt-tensix-optimize-const-residency -mtt-tensix-optimize-pressure-park -fdump-tree-rvtt_prgm_const-details" }
// { dg-final { scan-tree-dump-times "refused .prgm-exhausted" 1 "rvtt_prgm_const" } }
// { dg-final { scan-tree-dump-times "reusing TU-programmed PRGM L12 .every TU write stores 0x3f317218" 1 "rvtt_prgm_const" } }
// { dg-final { scan-tree-dump-times "pressure-park: hoisted constant 0x3f317218 now read from its resident PRGM register" 1 "rvtt_prgm_const" } }
// { dg-final { scan-tree-dump-times "pressure-park: hoisted invariant materialization to a free LREG" 1 "rvtt_prgm_const" } }
// { dg-final { scan-tree-dump-not "lreg-file-exhausted" "rvtt_prgm_const" } }
// The init's three SFPCONFIGs plus the kernel's reprogramming of L12.
// { dg-final { scan-assembler-times "SFPCONFIG" 4 } }
// { dg-final { scan-assembler {SFPMUL\tL\d+, L\d+, L12} } }

void owner_init (void)
{
  auto k0 = __builtin_rvtt_sfpxloadi (nullptr, 0x3f317218, 0, 0, 31);
  __builtin_rvtt_sfpwriteconfig_v (k0, 0, 12);
  auto k1 = __builtin_rvtt_sfpxloadi (nullptr, 0x3dd8adac, 0, 0, 31);
  __builtin_rvtt_sfpwriteconfig_v (k1, 0, 13);
  auto k2 = __builtin_rvtt_sfpxloadi (nullptr, 0xbf317218, 0, 0, 31);
  __builtin_rvtt_sfpwriteconfig_v (k2, 0, 14);
}

void kernel_relief (void)
{
  auto x0 = __builtin_rvtt_sfpreadlreg (0);
  auto x1 = __builtin_rvtt_sfpreadlreg (1);
  auto x2 = __builtin_rvtt_sfpreadlreg (2);
  auto x3 = __builtin_rvtt_sfpreadlreg (3);
  auto a = __builtin_rvtt_sfpxloadi (nullptr, 0x3f317218, 0, 0, 31);
  auto b = __builtin_rvtt_sfpxloadi (nullptr, 0x3dd8adac, 0, 0, 31);
  auto c = __builtin_rvtt_sfpxloadi (nullptr, 0xbf317218, 0, 0, 31);
  for (unsigned ix = 0; ix != 32; ++ix)
    {
      auto d = __builtin_rvtt_sfpxloadi (nullptr, 0x40490fdb, 0, 0, 31);
      x0 = __builtin_rvtt_sfpmul (x0, a, 0);
      x1 = __builtin_rvtt_sfpmul (x1, b, 0);
      x2 = __builtin_rvtt_sfpmul (x2, c, 0);
      x3 = __builtin_rvtt_sfpmul (x3, d, 0);
    }
  __builtin_rvtt_sfpwritelreg (x0, 0);
  __builtin_rvtt_sfpwritelreg (x1, 1);
  __builtin_rvtt_sfpwritelreg (x2, 2);
  __builtin_rvtt_sfpwritelreg (x3, 3);
}
