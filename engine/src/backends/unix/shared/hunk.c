/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * This file implements the low level part of the Hunk_* memory system
 *
 * =======================================================================
 */

/* For mremap() - must be before sys/mman.h include! */
#if defined(__linux__) && !defined(_GNU_SOURCE)
 #define _GNU_SOURCE
#endif

#include <sys/mman.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../../common/header/common.h"

#if defined(__FreeBSD__) || defined(__OpenBSD__)
 #include <machine/param.h>
 #define MAP_ANONYMOUS MAP_ANON
#endif

#if defined(__APPLE__)
 #include <sys/types.h>
 #define MAP_ANONYMOUS MAP_ANON
#endif

byte *membase;
size_t maxhunksize;
size_t curhunksize;

#if defined(__EMSCRIPTEN__)
/* Emscripten's mmap()/munmap() are a shim over a plain heap allocation
 * (malloc-backed), not real page-level virtual memory -- there's no
 * kernel to hand pages back to, it's all one contiguous typed array
 * either way. The generic mmap/mremap/munmap implementation below
 * relies on being able to shrink a *portion* of a previously-mapped
 * block down to just what Hunk_End's caller actually used, then later
 * munmap() exactly that shrunk size from Hunk_Free -- confirmed on a
 * real device that neither half of that works here: an in-place
 * mremap()-style shrink at Hunk_End reliably failed with errno 28
 * (ENOSPC) the first time a map ever finished loading, so this used to
 * just skip the shrink and keep membase pointed at the full
 * maxhunksize reservation -- except Hunk_End's very last line below
 * (shared by every platform) always overwrites the stored header with
 * the *shrunk* curhunksize, not the true maxhunksize that got
 * reserved. Hunk_Free then trusted that now-wrong, too-small header
 * and munmap()'d only that many bytes -- leaving the remainder of
 * every world model's oversized hunk reservation (hunkSize is a
 * padded upper-bound estimate, see Mod_CalcLumpHunkSize in
 * sw_model.c, so this was routinely a large fraction of a few MB)
 * permanently unreachable and unfreed on every single map transition.
 * Confirmed against a real device's console log: heap baseline
 * ratcheted upward by roughly one hunkSize's worth on every fresh
 * level load that evicted and replaced the previous world model, with
 * no matching drop back down, eventually hitting the fixed 64MB
 * ceiling after just three or four transitions and aborting with OOM.
 *
 * Fix: don't use mmap/munmap for this platform at all -- malloc the
 * full reservation up front, realloc() it down to the real size at
 * Hunk_End (a real heap allocator, unlike the mmap shim, actually
 * gives the trimmed-off tail back to its free list here), and free()
 * exactly what realloc last returned at Hunk_Free. No separate stored
 * size needed for the free path -- malloc's own bookkeeping already
 * knows how big its returned pointer is. */

void *
Hunk_Begin(int maxsize)
{
	/* plus 32 bytes for cacheline, matching the generic path below */
	maxhunksize = maxsize + sizeof(size_t) + 32;
	curhunksize = 0;

	membase = (byte *)malloc(maxhunksize);

	if (membase == NULL)
	{
		Sys_Error("unable to allocate %d bytes", maxsize);
	}

	*((size_t *)membase) = curhunksize;

	return membase + sizeof(size_t);
}

void *
Hunk_Alloc(int size)
{
	byte *buf;

	/* round to cacheline */
	size = (size + 31) & ~31;

	if (curhunksize + size > maxhunksize)
	{
		Sys_Error("%s: overflow %d > %d",
			__func__, curhunksize + size, maxhunksize);
	}

	buf = membase + sizeof(size_t) + curhunksize;
	curhunksize += size;
	return buf;
}

int
Hunk_End(void)
{
	byte *n = (byte *)realloc(membase, curhunksize + sizeof(size_t));

	if (n == NULL)
	{
		Sys_Error("Hunk_End: realloc failed shrinking to %d bytes",
			curhunksize + sizeof(size_t));
	}

	membase = n;
	*((size_t *)membase) = curhunksize + sizeof(size_t);

	return curhunksize;
}

void
Hunk_Free(void *base)
{
	if (base)
	{
		free(((byte *)base) - sizeof(size_t));
	}
}

#else /* !__EMSCRIPTEN__ */

void *
Hunk_Begin(int maxsize)
{

	/* reserve a huge chunk of memory, but don't commit any yet */
	/* plus 32 bytes for cacheline */
	maxhunksize = maxsize + sizeof(size_t) + 32;
	curhunksize = 0;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	int prot = PROT_READ | PROT_WRITE;

#if defined(MAP_ALIGNED_SUPER)
	const size_t hgpagesize = 1UL<<21;
	size_t page_size = sysconf(_SC_PAGESIZE);

	/* Archs supported has 2MB for super pages size */
	if (maxhunksize >= hgpagesize)
	{
		maxhunksize = (maxhunksize & ~(page_size-1)) + page_size;
		flags |= MAP_ALIGNED_SUPER;
	}
#endif

#if defined(PROT_MAX)
	/* For now it is FreeBSD exclusif but could possibly be extended
	   to other like DFBSD for example */
	prot |= PROT_MAX(prot);
#endif

	membase = (byte *)mmap(0, maxhunksize, prot,
			flags, -1, 0);

	if ((membase == NULL) || (membase == (byte *)-1))
	{
		Sys_Error("unable to virtual allocate %d bytes", maxsize);
	}

	*((size_t *)membase) = curhunksize;

	return membase + sizeof(size_t);
}

void *
Hunk_Alloc(int size)
{
	byte *buf;

	/* round to cacheline */
	size = (size + 31) & ~31;

	if (curhunksize + size > maxhunksize)
	{
		Sys_Error("%s: overflow %d > %d",
			__func__, curhunksize + size, maxhunksize);
	}

	buf = membase + sizeof(size_t) + curhunksize;
	curhunksize += size;
	return buf;
}

int
Hunk_End(void)
{
	byte *n = NULL;

#if defined(__linux__)
	n = (byte *)mremap(membase, maxhunksize, curhunksize + sizeof(size_t), 0);
#elif defined(__NetBSD__)
	n = (byte *)mremap(membase, maxhunksize, NULL, curhunksize + sizeof(size_t), 0);
#else
 #ifndef round_page
 size_t page_size = sysconf(_SC_PAGESIZE);
 #define round_page(x) ((((size_t)(x)) + page_size-1) & ~(page_size-1))
 #endif

	size_t old_size = round_page(maxhunksize);
	size_t new_size = round_page(curhunksize + sizeof(size_t));

	if (new_size > old_size)
	{
		/* Can never happen. If it happens something's very wrong. */
		n = 0;
	}
	else if (new_size < old_size)
	{
		/* Hunk is to big, we need to shrink it. */
		n = munmap(membase + new_size, old_size - new_size) + membase;
	}
	else
	{
		/* No change necessary. */
		n = membase;
	}
#endif

	if (n != membase)
	{
		Sys_Error("Hunk_End: Could not remap virtual block (%d)", errno);
	}

	*((size_t *)membase) = curhunksize + sizeof(size_t);

	return curhunksize;
}

void
Hunk_Free(void *base)
{
	if (base)
	{
		byte *m;

		m = ((byte *)base) - sizeof(size_t);

		if (munmap(m, *((size_t *)m)))
		{
			Sys_Error("Hunk_Free: munmap failed (%d)", errno);
		}
	}
}

#endif /* !__EMSCRIPTEN__ */

