// PRESSURE-PARK relief skips a one-word in-loop constant that its
// consumer overwrites in place: SFPIADD writes its result over the 3.0,
// so a parked copy would cost a per-row SFPMOV, saving nothing while
// adding an SFPCONFIG.  The hoisted constants stay in LREGs.
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-unroll-loops -mtt-tensix-optimize-const-residency -mtt-tensix-optimize-pressure-park -fdump-tree-rvtt_prgm_const-details" }
// { dg-final { scan-tree-dump-times "pressure-park: refused .lreg-file-exhausted" 1 "rvtt_prgm_const" } }
// { dg-final { scan-tree-dump-not "now read from its resident PRGM register" "rvtt_prgm_const" } }
// { dg-final { scan-assembler-times "SFPCONFIG" 3 } }

void owner_init (void)
{
  auto k0 = __builtin_rvtt_sfpxloadi (nullptr, 0x3f317218, 0, 0, 31);
  __builtin_rvtt_sfpwriteconfig_v (k0, 0, 12);
  auto k1 = __builtin_rvtt_sfpxloadi (nullptr, 0x3dd8adac, 0, 0, 31);
  __builtin_rvtt_sfpwriteconfig_v (k1, 0, 13);
  auto k2 = __builtin_rvtt_sfpxloadi (nullptr, 0xbf317218, 0, 0, 31);
  __builtin_rvtt_sfpwriteconfig_v (k2, 0, 14);
}

void kernel_relief_overwritten (void)
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
      auto d = __builtin_rvtt_sfpxloadi (nullptr, 0x40400000, 0, 0, 31);
      x0 = __builtin_rvtt_sfpmul (x0, a, 0);
      x1 = __builtin_rvtt_sfpmul (x1, b, 0);
      x2 = __builtin_rvtt_sfpmul (x2, c, 0);
      x3 = __builtin_rvtt_sfpiadd_v (d, x3, 4);
    }
  __builtin_rvtt_sfpwritelreg (x0, 0);
  __builtin_rvtt_sfpwritelreg (x1, 1);
  __builtin_rvtt_sfpwritelreg (x2, 2);
  __builtin_rvtt_sfpwritelreg (x3, 3);
}
