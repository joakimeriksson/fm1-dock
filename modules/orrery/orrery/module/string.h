/* Freestanding target declarations; implementations supplied by hb_lib.c.
 * Native tests use their system header (module/ is not a native include directory). */
#ifndef MD_MODULE_STRING_H
#define MD_MODULE_STRING_H
#include <stddef.h>
void *memset(void *,int,size_t);
void *memcpy(void *,const void *,size_t);
int memcmp(const void *,const void *,size_t);
#endif
