/**
 * Tony Givargis
 * Copyright (C), 2023-2026
 * University of California, Irvine
 *
 * CS 238P - Operating Systems
 * logfs.c
 */

#include <pthread.h>
#include "device.h"
#include "logfs.h"

#define WCACHE_BLOCKS 32
#define RCACHE_BLOCKS 256

struct logfs {
	struct device *device;
	uint64_t block;
	uint64_t capacity;
	uint64_t head;
	uint64_t tail;
	uint64_t tags[RCACHE_BLOCKS];
	char *write_memory, *write_buffer;
	char *read_memory, *read_cache;
	pthread_t worker;
	pthread_mutex_t lock;
	pthread_cond_t data_ready;
	pthread_cond_t space_ready;
	int closing;
	int error;
};

/* The writer consumes full blocks from the circular write buffer. */
static void *
write_worker(void *arg)
{
	struct logfs *logfs;
	uint64_t pending, position, slot;

	logfs = arg;
	pthread_mutex_lock(&logfs->lock);
	for (;;) {
		pending = logfs->head - logfs->tail;
		while (!logfs->closing && !logfs->error && pending < logfs->block) {
			pthread_cond_wait(&logfs->data_ready, &logfs->lock);
			pending = logfs->head - logfs->tail;
		}
		if (logfs->error || (logfs->closing && !pending)) {
			break;
		}
		position = logfs->tail % (WCACHE_BLOCKS * logfs->block);
		if (pending < logfs->block) {
			memset(logfs->write_buffer + position + pending,
			       0, logfs->block - pending);
		}
		if (device_write(logfs->device,
				 logfs->write_buffer + position,
				 logfs->block + logfs->tail, logfs->block)) {
			logfs->error = 1;
			break;
		}
		slot = (logfs->tail / logfs->block) % RCACHE_BLOCKS;
		logfs->tags[slot] = (uint64_t)-1;
		logfs->tail += MIN(pending, logfs->block);
		pthread_cond_signal(&logfs->space_ready);
	}
	pthread_cond_signal(&logfs->space_ready);
	pthread_mutex_unlock(&logfs->lock);
	return NULL;
}

struct logfs *
logfs_open(const char *pathname)
{
	struct logfs *logfs;
	uint64_t i;

	if (!(logfs = malloc(sizeof(struct logfs)))) {
		return NULL;
	}
	memset(logfs, 0, sizeof(struct logfs));
	if (!(logfs->device = device_open(pathname))) {
		goto fail;
	}
	logfs->block = device_block(logfs->device);
	if (device_size(logfs->device) < 2 * logfs->block ||
	    logfs->block > (size_t)-1 / (RCACHE_BLOCKS + 1)) {
		goto fail;
	}
	/* One device block stores the logical length; data starts after it. */
	logfs->capacity = device_size(logfs->device) - logfs->block;
	logfs->write_memory = malloc((WCACHE_BLOCKS + 1) * logfs->block);
	logfs->read_memory = malloc((RCACHE_BLOCKS + 1) * logfs->block);
	if (!logfs->write_memory || !logfs->read_memory) {
		goto fail;
	}
	logfs->write_buffer = memory_align(logfs->write_memory, logfs->block);
	logfs->read_cache = memory_align(logfs->read_memory, logfs->block);
	memset(logfs->write_buffer, 0, WCACHE_BLOCKS * logfs->block);
	for (i = 0; i < RCACHE_BLOCKS; ++i) {
		logfs->tags[i] = (uint64_t)-1;
	}
	/* Before starting the worker, use one cache block to read the saved length. */
	if (device_read(logfs->device, logfs->read_cache, 0, logfs->block)) {
		goto fail;
	}
	memcpy(&logfs->head, logfs->read_cache, sizeof(logfs->head));
	if (logfs->head > logfs->capacity) {
		TRACE("invalid saved log length");
		goto fail;
	}
	logfs->tail = logfs->head / logfs->block * logfs->block;
	/* Keep the last partial block so appending after reopen preserves it. */
	if (logfs->head != logfs->tail &&
	    device_read(logfs->device,
			logfs->write_buffer + logfs->tail % (WCACHE_BLOCKS * logfs->block),
			logfs->block + logfs->tail, logfs->block)) {
		goto fail;
	}
	if (pthread_mutex_init(&logfs->lock, NULL)) {
		goto fail;
	}
	if (pthread_cond_init(&logfs->data_ready, NULL)) {
		goto fail_lock;
	}
	if (pthread_cond_init(&logfs->space_ready, NULL)) {
		goto fail_data;
	}
	if (pthread_create(&logfs->worker, NULL, write_worker, logfs)) {
		goto fail_space;
	}
	return logfs;

fail_space:
	pthread_cond_destroy(&logfs->space_ready);
fail_data:
	pthread_cond_destroy(&logfs->data_ready);
fail_lock:
	pthread_mutex_destroy(&logfs->lock);
fail:
	device_close(logfs->device);
	FREE(logfs->write_memory);
	FREE(logfs->read_memory);
	FREE(logfs);
	return NULL;
}

void
logfs_close(struct logfs *logfs)
{
	if (!logfs) {
		return;
	}
	pthread_mutex_lock(&logfs->lock);
	logfs->closing = 1;
	pthread_cond_signal(&logfs->data_ready);
	pthread_mutex_unlock(&logfs->lock);
	pthread_join(logfs->worker, NULL);
	/* Worker has finished: reuse one cache block to save the exact length. */
	if (!logfs->error) {
		memset(logfs->read_cache, 0, logfs->block);
		memcpy(logfs->read_cache, &logfs->head, sizeof(logfs->head));
		if (device_write(logfs->device, logfs->read_cache, 0, logfs->block)) {
			TRACE("unable to save log length");
		}
	}
	else {
		TRACE("background device write failed");
	}
	pthread_cond_destroy(&logfs->data_ready);
	pthread_cond_destroy(&logfs->space_ready);
	pthread_mutex_destroy(&logfs->lock);
	device_close(logfs->device);
	FREE(logfs->write_memory);
	FREE(logfs->read_memory);
	FREE(logfs);
}

int
logfs_append(struct logfs *logfs, const void *buf, uint64_t len)
{
	const char *input;
	uint64_t space, position, chunk, buffer_size;

	assert( logfs );
	assert( !len || buf );

	pthread_mutex_lock(&logfs->lock);
	if (logfs->error || len > logfs->capacity - logfs->head) {
		pthread_mutex_unlock(&logfs->lock);
		return -1;
	}
	input = buf;
	buffer_size = WCACHE_BLOCKS * logfs->block;
	while (len) {
		space = buffer_size - (logfs->head - logfs->tail);
		while (!space && !logfs->error) {
			pthread_cond_wait(&logfs->space_ready, &logfs->lock);
			space = buffer_size - (logfs->head - logfs->tail);
		}
		if (logfs->error) {
			pthread_mutex_unlock(&logfs->lock);
			return -1;
		}
		position = logfs->head % buffer_size;
		chunk = MIN(len, MIN(space, buffer_size - position));
		memcpy(logfs->write_buffer + position, input, chunk);
		logfs->head += chunk;
		input += chunk;
		len -= chunk;
		pthread_cond_signal(&logfs->data_ready);
	}
	pthread_mutex_unlock(&logfs->lock);
	return 0;
}

int
logfs_read(struct logfs *logfs, void *buf, uint64_t off, size_t len)
{
	char *output, *source;
	uint64_t number, slot, inside, chunk;

	assert( logfs );
	assert( !len || buf );

	pthread_mutex_lock(&logfs->lock);
	if (logfs->error || off > logfs->head || len > logfs->head - off) {
		pthread_mutex_unlock(&logfs->lock);
		return -1;
	}
	output = buf;
	while (len) {
		number = off / logfs->block;
		inside = off % logfs->block;
		chunk = MIN(len, logfs->block - inside);
		if (off >= logfs->tail) {
			/* New data is readable even before the worker writes it. */
			source = logfs->write_buffer + off % (WCACHE_BLOCKS * logfs->block);
		}
		else {
			slot = number % RCACHE_BLOCKS;
			if (logfs->tags[slot] != number) {
				if (device_read(logfs->device,
						logfs->read_cache + slot * logfs->block,
						logfs->block + number * logfs->block,
						logfs->block)) {
					pthread_mutex_unlock(&logfs->lock);
					return -1;
				}
				logfs->tags[slot] = number;
			}
			source = logfs->read_cache + slot * logfs->block + inside;
		}
		memcpy(output, source, chunk);
		output += chunk;
		off += chunk;
		len -= chunk;
	}
	pthread_mutex_unlock(&logfs->lock);
	return 0;
}

uint64_t
logfs_size(struct logfs *logfs)
{
	uint64_t size;

	pthread_mutex_lock(&logfs->lock);
	size = logfs->head;
	pthread_mutex_unlock(&logfs->lock);
	return size;
}

uint64_t
logfs_capacity(const struct logfs *logfs)
{
	return logfs->capacity;
}
