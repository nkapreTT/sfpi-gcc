// { dg-options "-mcpu=tt-bh-tensix -O3 -fno-exceptions -fno-rtti" }
// { dg-final { scan-assembler-not "SFPCONFIG" } }
// { dg-final { scan-assembler-not "\\.ttinsn" } }
// { dg-final { scan-assembler-times "TTINCRWC" 8 } }
// PRNG-consuming rounds remain explicit until replay preserves entropy order.
// { dg-final { scan-assembler-not "TTREPLAY" } }
// { dg-final { scan-assembler-times "SFPLOAD" 8 } }
// { dg-final { scan-assembler-times "SFPCAST" 8 } }
// { dg-final { scan-assembler-times "SFPSTOCH" 8 } }
// { dg-final { scan-assembler-times "SFPSTORE" 8 } }

#include "macro-planner-cast-round-emit.inc"
