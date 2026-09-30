// In-place store (store Dst address == first load's): the shared-carrier
// candidate proves no descriptor program and the planner's deterministic
// fallback demotes the store to its own delayed-store carrier, deriving
// the three-slot alternating-VD calendar.  The store launch word
// 2473639936 carries the in-place address 0 (the distinct-address form
// carries 2473640064).
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-exceptions -fno-rtti -mtt-tensix-macro-planner" }
// { dg-final { scan-assembler-times "SFPENCC" 1 } }
// { dg-final { scan-assembler-times "SFPCONFIG" 5 } }
// { dg-final { scan-assembler-times "\\.ttinsn" 19 } }
// { dg-final { scan-assembler-times "\\.ttinsn\\t2987524096" 1 } }
// { dg-final { scan-assembler-times "\\.ttinsn\\t2988572674" 1 } }
// { dg-final { scan-assembler-times "\\.ttinsn\\t2989817856" 1 } }
// { dg-final { scan-assembler-times "\\.ttinsn\\t2466308096" 4 } }
// { dg-final { scan-assembler-times "\\.ttinsn\\t2467356672" 4 } }
// { dg-final { scan-assembler-times "\\.ttinsn\\t2473639936" 8 } }
// { dg-final { scan-assembler-times {SFPLOADI\tL0, 705, 2} 1 } }
// { dg-final { scan-assembler-times "SFPLOAD\\t" 8 } }
// { dg-final { scan-assembler-times "SFPNOP" 3 } }
// { dg-final { scan-assembler-not "SFPSWAP" } }
// { dg-final { scan-assembler-not "SFPSTORE" } }
// { dg-final { scan-assembler-not "TTINCRWC" } }

#include "loadmacro-periodic-minmax-inplace-body.h"
