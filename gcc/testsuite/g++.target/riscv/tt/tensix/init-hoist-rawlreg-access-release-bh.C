// Releasing raw L0 before the loop makes the descriptor's fixed L0 staging
// register available at the loop entry.
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-exceptions -fno-rtti -mtt-tensix-macro-planner -mtt-tensix-macro-planner-verify -mtt-tensix-optimize-init-hoist -fdump-rtl-rvtt_macro_planner-details" }
// { dg-final { scan-rtl-dump "Macro-planner init-hoist: stage=1 init contract hoisted to caller loop preheader" "rvtt_macro_planner" } }
// { dg-final { scan-rtl-dump-not "init-hoist: refused \\(drain-init-fixed-lreg-live\\)" "rvtt_macro_planner" } }

#define INIT_CALLER_PRELUDE() do { \
  __builtin_rvtt_sfprawlreg_access (0, 0x01); \
  __builtin_rvtt_sfprawlreg_access (0x01, 0); \
} while (0)
#include "init-hoist-caller-body.h"
