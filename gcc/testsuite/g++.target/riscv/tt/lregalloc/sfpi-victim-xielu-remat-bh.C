// Contrast twin of sfpi-victim-xielu-bh.C: the SAME loop-held-alphas
// body COMPILES once -mtt-tensix-optimize-const-remat is on (the alphas
// and polynomial constants are proven-constant and rematerializable).
// This marks the boundary between constant pressure (relieved by
// rematerialization) and computed-value pressure (which needs a
// spiller).
// { dg-do compile }
// { dg-options "-mcpu=tt-bh-tensix -O2 -I [SFPI]/include -fno-exceptions -fno-rtti -mtt-tensix-optimize-const-remat" }

#define victim_xielu_loopheld victim_xielu_loopheld_remat
#include "sfpi-victim-xielu-bh.C"
