#include "ncvideo.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <libshmipc.h>

#define NCV_ASSERT_CLEANUP(_v, _e, ...) if(!(_v)){ ret = _e; printf(__VA_ARGS__); puts(""); goto cleanup; }

struct ncv_context
{
	shmipc* read_queue, *write_queue;
};

ncv_error ncv_ctx_create(const char* shm_name, ncv_context** out_context)
{
	ncv_error ret = NCV_ERR_UNKNOWN;

	ncv_context* ctx = calloc(1, sizeof(ncv_context));
	NCV_ASSERT_CLEANUP(ctx, NCV_ERR_ALLOC, "context allocation failed");

	char name_host_writer[512];
	snprintf(name_host_writer, sizeof(name_host_writer), "%s_host_writer", shm_name);

	shmipc_error serr = shmipc_open(shm_name, SHMIPC_AM_WRITE, &ctx->write_queue);
	NCV_ASSERT_CLEANUP(serr == SHMIPC_ERR_SUCCESS, NCV_ERR_SHM, "could not open write_queue: %d", serr);
	
	serr = shmipc_open(name_host_writer, SHMIPC_AM_READ, &ctx->read_queue);
	NCV_ASSERT_CLEANUP(serr == SHMIPC_ERR_SUCCESS, NCV_ERR_SHM, "could not open read_queue: %d", serr);

	*out_context = ctx;

	return NCV_ERR_SUCCESS;

cleanup:
	return ret;
}

void ncv_ctx_destroy(ncv_context** ctx)
{
	shmipc_destroy(&(*ctx)->read_queue);
	shmipc_destroy(&(*ctx)->write_queue);

	free(*ctx);
	*ctx = NULL;
}

ncv_error ncv_wait_for_frame(ncv_context* ctx, int timeout, int* out_width, int* out_hegiht, void** out_frame)
{
	char type[SHMIPC_MESSAGE_TYPE_LENGTH];
	char message[shmipc_get_message_max_length(ctx->read_queue)];
	size_t size;
	
	shmipc_error serr = shmipc_recv_message(ctx->read_queue, type, message, &size, timeout);

	if(serr != SHMIPC_ERR_TIMEOUT && serr != SHMIPC_ERR_SUCCESS)
		return NCV_ERR_SHM;

	if(serr == SHMIPC_ERR_TIMEOUT)
		return NCV_ERR_TIMEOUT;

	if(strcmp(type, "cmd") != 0)
		return NCV_ERR_UNKNOWN_MSG;
	
	if(strcmp(message, "quit") != 0)
		return NCV_ERR_HOST_QUIT;
	
	if(strcmp(message, "newframe") != 0)
		return NCV_ERR_HOST_QUIT;

	return NCV_ERR_UNKNOWN_MSG;
}
