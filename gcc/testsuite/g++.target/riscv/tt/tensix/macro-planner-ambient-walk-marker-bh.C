// { dg-do compile }
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-exceptions -fno-rtti -mtt-tensix-macro-planner -mtt-tensix-macro-planner-verify -fdump-rtl-rvtt_macro_planner-details" }
// The entry-ambient walk steps over the zero-length raw-LREG markers that
// follow raw init words (the typecast init anatomy: an SFPLOADI whose L0
// result is read back, and an SFPCONFIG with its LREG effects named).
// The markers deliver no instruction word; the raw words themselves still
// decode CC-inert, so both functions form.  A marker after a real CC
// writer (raw SFPSETCC) does not hide it: that walk stays dirty.
// { dg-final { scan-rtl-dump-times "Macro-planner formation: entry-ambient all-lanes derived" 2 "rvtt_macro_planner" } }
// { dg-final { scan-rtl-dump-times "Macro-planner formation-refusal: all-lanes-proof-missing \\(ambient-entry-unproven\\)" 1 "rvtt_macro_planner" } }
// { dg-final { scan-rtl-dump-times "Macro-planner ambient-walk dirty: insn \\d+ bb \\d+ \\(tu-audit-not-run\\) word=0x7b000001" 1 "rvtt_macro_planner" } }
// { dg-final { scan-rtl-dump-not "opaque-typed-effects" "rvtt_macro_planner" } }

#define TTI(w) __asm__ __volatile__ (".ttinsn %0" :: "n" (w))

static inline void formable_loop (int base)
{
#pragma GCC unroll 8
  for (int row = 0; row < 32; ++row)
    {
      auto v = __builtin_rvtt_sfpload (nullptr, base, 0, 0, 0, 7);
      auto cst = __builtin_rvtt_sfpreadlreg (9);
      auto pair = __builtin_rvtt_sfpswap (cst, v, 1);
      auto r = __builtin_rvtt_sfpselect2 (pair, 1);
      __builtin_rvtt_sfpstore (nullptr, r, base, 0, 0, 0, 7);
      __builtin_rvtt_ttincrwc (0, 2, 0, 0);
    }
}

__attribute__((noinline)) void effect_marker_walk_fire ()
{
  TTI (0x710a0004);		/* SFPLOADI L0 */
  __builtin_rvtt_sfprawlreg_effect (0, 1);
  TTI (0x91000040);		/* SFPCONFIG dest 4 (LoadMacroConfig) */
  __builtin_rvtt_sfprawlreg_effect (1, 0);
  formable_loop (0);
}

__attribute__((noinline)) void read_marker_walk_fire ()
{
  TTI (0x710a0004);		/* SFPLOADI L0 */
  auto l0 = __builtin_rvtt_sfpreadlreg (0);
  __builtin_rvtt_sfpwritelreg (l0, 0);
  formable_loop (32);
}

__attribute__((noinline)) void effect_marker_ccwriter_refuse ()
{
  TTI (0x7b000001);		/* SFPSETCC: a real CC writer */
  __builtin_rvtt_sfprawlreg_effect (1, 0);
  formable_loop (64);
}
