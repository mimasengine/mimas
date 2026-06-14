//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Fixed point arithemtics, implementation.
//


#ifndef __M_FIXED__
#define __M_FIXED__




//
// Fixed point, 32bit as 16.16.
//
#define FRACBITS		16
#define FRACUNIT		(1<<FRACBITS)

typedef int fixed_t;

/* SATURN: DMULS.L gives signed 32×32→64 in MACH:MACL.
   XTRCT Rm,Rn: Rn = (Rn>>16)|(Rm<<16) extracts bits[47:16] = the 16.16 result.
   Four instructions inline, zero call overhead across all 167 call sites. */
static inline fixed_t FixedMul(fixed_t a, fixed_t b)
{
    int mach, macl;
    __asm__ volatile (
        "dmuls.l %2, %3\n\t"
        "sts     mach, %0\n\t"
        "sts     macl, %1\n\t"
        "xtrct   %0,   %1"
        : "=r"(mach), "=r"(macl)
        : "r"(a), "r"(b)
        : "mach", "macl"
    );
    return (fixed_t)macl;
}

/* SATURN: SH-2 hardware 64/32→32 division using DIV0U+DIV1 (unsigned, no sign correction
   needed). ~37 cycles vs ~200+ for the __sdivdi3 library call. Signs handled in C.
   DIV0S was tried first but the non-restoring algorithm requires an explicit MOVT+ADDC
   correction for mixed-sign inputs; DIV0U avoids the ambiguity entirely. */
static inline fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    if (__builtin_expect(
            ((unsigned)__builtin_abs(a) >> 14) >= (unsigned)__builtin_abs(b), 0))
        return (a ^ b) < 0 ? (fixed_t)0x80000000 : (fixed_t)0x7fffffff;
    {
        int sign = (a ^ b) >> 31;                   /* 0 same-sign, -1 diff-sign */
        unsigned int ua = (unsigned)(a < 0 ? -a : a);
        unsigned int ub = (unsigned)(b < 0 ? -b : b);
        unsigned int hi = ua >> 16;                  /* high 32 bits of (ua << 16) */
        unsigned int lo = ua << 16;                  /* low  32 bits of (ua << 16) */
        __asm__ volatile(
            "div0u\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0\n\t" "div1 %2, %1\n\t"
            "rotcl  %0"
            : "+r"(lo), "+r"(hi)
            : "r"(ub)
            : "cc"
        );
        return sign ? -(fixed_t)lo : (fixed_t)lo;
    }
}



#endif
