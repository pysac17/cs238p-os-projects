/**
 * Tony Givargis
 * Copyright (C), 2023-2026
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * scm.c
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include "scm.h"

#define SCM_ADDR ((void *)(uintptr_t)0x600000000000UL)
#define ALIGNMENT 16UL

struct header {
	uint64_t size;
	uint64_t padding;
};

struct block {
	uint64_t size;
	uint64_t valid;
};

struct scm {
	int fd;
	char *mem;
	size_t length;
	struct header *header;
};

struct scm *
scm_open(const char *pathname, int truncate)
{
	struct scm *scm;
	struct stat st;
	void *mem;
	int saved;

	assert( pathname );

	if (!(scm = malloc(sizeof(struct scm)))) {
		return NULL;
	}
	memset(scm, 0, sizeof(struct scm));
	scm->fd = open(pathname, O_RDWR);
	if (scm->fd < 0) {
		free(scm);
		return NULL;
	}
	if (fstat(scm->fd, &st)) {
		goto fail;
	}
	if (!S_ISREG(st.st_mode) ||
	    st.st_size < (off_t)(sizeof(struct header) + sizeof(struct block) + ALIGNMENT) ||
	    (uint64_t)st.st_size > (uint64_t)(size_t)-1) {
		errno = EINVAL;
		goto fail;
	}
	scm->length = (size_t)st.st_size;
#ifdef __linux__

	if ((uintptr_t)sbrk(0) >= (uintptr_t)SCM_ADDR) {
		errno = ENOMEM;
		goto fail;
	}
#endif

	mem = mmap(SCM_ADDR, scm->length, PROT_READ | PROT_WRITE,
		   MAP_SHARED, scm->fd, 0);
	if (mem == MAP_FAILED) {
		goto fail;
	}
	if (mem != SCM_ADDR) {
		munmap(mem, scm->length);
		errno = EADDRNOTAVAIL;
		goto fail;
	}
	scm->mem = mem;
	scm->header = mem;
	if (truncate) {
		memset(mem, 0, scm->length);
		scm->header->size = sizeof(struct header);
	}

	if (scm->header->size < sizeof(struct header) ||
	    scm->header->size > scm->length ||
	    scm->header->size % ALIGNMENT) {
		errno = EINVAL;
		goto fail;
	}

#ifdef __linux__
	printf("MAPPED REGION   : %012lx - %012lx\n",
	       0UL, (unsigned long)((uintptr_t)sbrk(0) - 1));
	printf("UNMAPPED REGION : %012lx - %012lx\n",
	       (unsigned long)(uintptr_t)sbrk(0),
	       (unsigned long)((uintptr_t)SCM_ADDR - 1));
#endif
	printf("SCM REGION      : %012lx - %012lx\n\n",
	       (unsigned long)(uintptr_t)scm->mem,
	       (unsigned long)((uintptr_t)scm->mem + scm->length - 1));
	return scm;

fail:

	saved = errno;
	if (scm->mem) {
		munmap(scm->mem, scm->length);
	}
	close(scm->fd);
	free(scm);
	errno = saved;
	return NULL;
}

void
scm_close(struct scm *scm)
{
	if (scm) {
		if (msync(scm->mem, scm->length, MS_SYNC)) {
			TRACE("msync failed");
		}
		munmap(scm->mem, scm->length);
		close(scm->fd);
		free(scm);
	}
}

void *
scm_malloc(struct scm *scm, size_t n)
{
	struct block *block;
	size_t offset;
	size_t remaining;

	assert( scm );

	if (!n || n > (size_t)-1 - (ALIGNMENT - 1)) {
		return NULL;
	}

	n = ((n + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;
	for (offset = sizeof(struct header); offset < scm->header->size;
	     offset += sizeof(struct block) + block->size) {
		block = (struct block *)(scm->mem + offset);
		if (!block->valid && block->size >= n) {
			block->valid = 1;
			return block + 1;
		}
	}
	remaining = scm->length - scm->header->size;
	if (remaining < sizeof(struct block) || n > remaining - sizeof(struct block)) {
		errno = ENOMEM;
		return NULL;
	}
	block = (struct block *)(scm->mem + scm->header->size);
	block->size = n;
	block->valid = 1;
	scm->header->size += sizeof(struct block) + n;
	return block + 1;
}

char *
scm_strdup(struct scm *scm, const char *s)
{
	char *copy;
	size_t n;

	assert( s );

	n = strlen(s) + 1;
	copy = scm_malloc(scm, n);
	if (copy) {
		memcpy(copy, s, n);
	}
	return copy;
}

void
scm_free(struct scm *scm, void *p)
{
	struct block *block;
	size_t offset;

	if (!scm || !p) {
		return;
	}
	for (offset = sizeof(struct header); offset < scm->header->size;
	     offset += sizeof(struct block) + block->size) {
		block = (struct block *)(scm->mem + offset);
		if ((void *)(block + 1) == p) {
			block->valid = 0;
			return;
		}
	}
}

size_t
scm_utilized(const struct scm *scm)
{
	const struct block *block;
	size_t offset;
	size_t used;

	assert( scm );

	used = 0;
	for (offset = sizeof(struct header); offset < scm->header->size;
	     offset += sizeof(struct block) + block->size) {
		block = (const struct block *)(scm->mem + offset);
		if (block->valid) {
			used += sizeof(struct block) + block->size;
		}
	}
	return used;
}

size_t
scm_capacity(const struct scm *scm)
{
	assert( scm );

	return scm->length - sizeof(struct header);
}

void *
scm_mbase(struct scm *scm)
{
	assert( scm );

	return scm->mem + sizeof(struct header) + sizeof(struct block);
}
