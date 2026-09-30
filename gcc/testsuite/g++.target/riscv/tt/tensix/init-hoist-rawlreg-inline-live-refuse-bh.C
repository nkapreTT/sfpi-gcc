// A committed inline helper can establish raw L0 ownership before the
// caller's loop.  The descriptor staging edge must respect that value.
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-early-inlining -fno-exceptions -fno-rtti -mtt-tensix-macro-planner -mtt-tensix-macro-planner-verify -mtt-tensix-optimize-init-hoist -fdump-rtl-rvtt_macro_planner-details" }
// { dg-final { scan-rtl-dump "init-hoist: refused \\(drain-init-fixed-lreg-live\\)" "rvtt_macro_planner" } }

static inline void raw_l0_seed ()
{
  __builtin_rvtt_sfprawlreg_access (0, 0x01);
}

#define INIT_CALLER_PRELUDE() raw_l0_seed ()
#include "init-hoist-caller-body.h"
