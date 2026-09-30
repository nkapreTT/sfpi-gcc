// A raw L0 read on either incoming path is live at the join and therefore
// through the descriptor staging edge.
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-exceptions -fno-rtti -mtt-tensix-macro-planner -mtt-tensix-macro-planner-verify -mtt-tensix-optimize-init-hoist -fdump-rtl-rvtt_macro_planner-details" }
// { dg-final { scan-rtl-dump "init-hoist: refused \\(drain-init-fixed-lreg-live\\)" "rvtt_macro_planner" } }

#define INIT_LOOP_TAIL() do { \
  if (t & 1) \
    __builtin_rvtt_sfprawlreg_effect (0x01, 0); \
  else \
    __builtin_rvtt_sfprawlreg_effect (0, 0); \
} while (0)
#include "init-hoist-caller-body.h"
