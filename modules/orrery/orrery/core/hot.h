/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_HOT_H
#define MD_HOT_H
#include <stdint.h>

/* MD_HOT: a function the render runs every block, a loop over the frames. On the FM-1 code runs from the flash
 * through a 32 KiB cache that the main loop's code shares, and a band's inner loops alone are about that much:
 * a host with RAM for code names its section in MD_HOT_SECTION (fm1-dock: ".ram_text.md", 16 KiB for a module)
 * and these functions run from there, out of the cache's way. Elsewhere MD_HOT is nothing.
 *
 * RAM and flash are farther apart than a direct call reaches, and the linker refuses one that does not reach:
 *   - code in the flash calls a MD_HOT function through MD_FAR(fn), or through a descriptor's pointer;
 *   - a MD_HOT function calls only MD_HOT functions directly; anything else through MD_FAR or a pointer, and its
 *     helpers are MD_INLINE (a plain static inline may stay out of line, in the flash). No memset, no memcpy
 *     (md_clear32 below). */
#ifdef MD_HOT_SECTION
#define MD_HOT __attribute__((section(MD_HOT_SECTION), noinline))
static inline void *md_far_pointer(void *p) { void *volatile q = p; return q; }   /* (the compiler cannot fold it) */
#define MD_FAR(fn) ((__typeof__(&fn))md_far_pointer((void *)&fn))
#else
#define MD_HOT
#define MD_FAR(fn) (&(fn))
#endif
#define MD_INLINE static inline __attribute__((always_inline))

/* n words of zero (the library's memset is a byte at a time, and in the flash) */
MD_INLINE void md_clear32(int32_t *p, uint32_t n)
{
    for (; n >= 4u; n -= 4u, p += 4) { p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; }
    while (n--) *p++ = 0;
}
#endif
