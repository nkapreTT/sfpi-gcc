// { dg-do compile }
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-unroll-loops -mtt-tensix-optimize-dst-autoincr -fdump-rtl-rvtt_dst_autoincr-details" }
// A typed TTSETRWC that also resets the SrcA/SrcB counters (mask 15) is
// not a pure Dst-leg step: it keeps the refusing default, the dominating
// placement refuses, and the two-row groups cannot pay for a program of
// their own -- the function is emitted unchanged.
// { dg-final { scan-rtl-dump "Dst-autoincr: dominating placement refused: foreign effect on a path .loop \[0-9\]+." "rvtt_dst_autoincr" } }
// { dg-final { scan-rtl-dump-not "preheader" "rvtt_dst_autoincr" } }
// { dg-final { scan-assembler-times "TTINCRWC\t0, 2, 0, 0" 2 } }
// { dg-final { scan-assembler-not "TTSETC16" } }

#define FACE_MODE 7
#define FACE_ADVANCE __builtin_rvtt_ttsetrwc (0, 0, 0, 0, 0, 15)
#define FACE_FN face_setrwc_refuse
#include "dst-autoincr-face-domloop-body.h"
