// { dg-do compile }
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-unroll-loops -mtt-tensix-optimize-dst-autoincr -fdump-rtl-rvtt_dst_autoincr-details" }
// The face advance written as two TYPED TTSETRWC builtins (lltt::setrwc,
// clear=0, CR=4, D=8, B=0, A=0, mask=4).  The typed pattern takes the same
// field decode as the raw TTI_SETRWC word, so it is a pure Dst/RWC counter
// step and the program lands once in the face-loop preheader -- the same
// decisions as dst-autoincr-face-rawword-bh.C.
// { dg-final { scan-rtl-dump-times "Dst-autoincr group: bb \[0-9\]+ rows 2 stride 2 config 3 words .preheader." 1 "rvtt_dst_autoincr" } }
// { dg-final { scan-assembler-not "TTINCRWC" } }
// { dg-final { scan-assembler-times "TTSETC16\t18, 0" 1 } }
// { dg-final { scan-assembler-times "TTSETC16\t34, 2" 1 } }
// { dg-final { scan-assembler-times "TTSETC16\t53, 0" 1 } }
// { dg-final { scan-assembler-times {TTSETRWC\t0, 4, 8, 0, 0, 4} 2 } }
// { dg-final { scan-assembler-times "SFPSTORE\tL., 0, 0, 6" 2 } }

#define FACE_MODE 7
#define FACE_ADVANCE                                                          \
  do                                                                          \
    {                                                                         \
      __builtin_rvtt_ttsetrwc (0, 4, 8, 0, 0, 4);                             \
      __builtin_rvtt_ttsetrwc (0, 4, 8, 0, 0, 4);                             \
    }                                                                         \
  while (0)
#define FACE_FN face_setrwc
#include "dst-autoincr-face-domloop-body.h"
