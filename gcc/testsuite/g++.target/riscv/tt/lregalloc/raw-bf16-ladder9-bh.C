// Refusal edge: 9 simultaneously live values in a kernel whose ONLY Dst
// layout information is bf16 (every load/store uses mod0 FMT_FP16B = 2,
// i.e. 16-bit Dst rows).  A Dst-row spill of a 32-bit LREG value would
// TRUNCATE, so an exact-only spiller must refuse by name
// (lreg-spill-inexact-dst-mode); without a spiller the compiler refuses
// with lreg-pressure-exceeded.  Compiling this, or spilling with
// truncation, is a failure.
// { dg-do compile }
// { dg-options "-mcpu=tt-bh-tensix -O2 -fno-unroll-loops" }
// { dg-error "lreg-pressure-exceeded|lreg-spill-inexact-dst-mode" "" { target *-*-* } 0 }

#define LADDER_NAME ladder_live9_bf16
#define LADDER_N 9
#define LADDER_FMT 2 /* FP16B: 16-bit Dst rows -- spill would truncate */
#define LADDER_NOINC 7
#define LADDER_TRIPS 8
#define LADDER_TWIST 0
#include "ladder-body.h"
