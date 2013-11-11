#include "ncvideo.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <libshmipc.h>

#define NCV_ASSERT_CLEANUP(_v, _e, ...) if(!(_v)){ ret = _e; printf(__VA_ARGS__); puts(""); goto cleanup; }

struct ncv_context
{
	shmipc* read_queue, *write_queue;
	shmhandle* frame_shm;
	const void* shm_area;
	char*** args;
	int num_args;
};

bool parse_args(shmipc* read_queue, int* out_num_args, char**** out_args)
{
	char type[SHMIPC_MESSAGE_TYPE_LENGTH];
	char message[shmipc_get_message_max_length(read_queue)];
	size_t size;
	
	shmipc_error serr = shmipc_recv_message(read_queue, type, message, &size, NCV_INFINITE);

	if(serr != SHMIPC_ERR_SUCCESS || strcmp(type, "arguments") != 0){
		printf("expceted argument, but got: %s\n", type);
		return false;
	}
	
	int num_args = atoi(message);

	char*** args = calloc(1, sizeof(char**) * num_args);
	if(!args)
		return false;

	for(int i = 0; i < num_args; i++){
		shmipc_error serr = shmipc_recv_message(read_queue, type, message, &size, NCV_INFINITE);

		if(serr != SHMIPC_ERR_SUCCESS)
			goto cleanup;

		args[i] = calloc(1, sizeof(char*) * 2);
		if(!args[i])
			goto cleanup;

		args[i][0] = strdup(type);
		args[i][1] = strdup(message);
	}

	*out_num_args = num_args;
	*out_args = args;

	return true;

cleanup:
	// TODO
	return false;
}

ncv_error ncv_ctx_create(const char* shm_queue_name, const char* shm_frame_name, ncv_context** out_context)
{
	ncv_error ret = NCV_ERR_UNKNOWN;

	ncv_context* ctx = calloc(1, sizeof(ncv_context));
	NCV_ASSERT_CLEANUP(ctx, NCV_ERR_ALLOC, "context allocation failed");

	char name_host_writer[512];
	snprintf(name_host_writer, sizeof(name_host_writer), "%s_host_writer", shm_queue_name);

	shmipc_error serr = shmipc_open(shm_queue_name, SHMIPC_AM_WRITE, &ctx->write_queue);
	NCV_ASSERT_CLEANUP(serr == SHMIPC_ERR_SUCCESS, NCV_ERR_SHM, "could not open write_queue: %d", serr);
	
	serr = shmipc_open(name_host_writer, SHMIPC_AM_READ, &ctx->read_queue);
	NCV_ASSERT_CLEANUP(serr == SHMIPC_ERR_SUCCESS, NCV_ERR_SHM, "could not open read_queue: %d", serr);

	size_t shmsize;
	serr = shmipc_open_shm_ro(shm_frame_name, &shmsize, &ctx->shm_area, &ctx->frame_shm);
	NCV_ASSERT_CLEANUP(serr == SHMIPC_ERR_SUCCESS, NCV_ERR_SHM, "could not open shm_area: %d", serr);

	NCV_ASSERT_CLEANUP(parse_args(ctx->read_queue, &ctx->num_args, &ctx->args), NCV_ERR_PARSING_ARGS, "could not parse arguments");

	*out_context = ctx;

	return NCV_ERR_SUCCESS;

cleanup:
	// TODO
	return ret;
}

void ncv_ctx_destroy(ncv_context** ctx)
{
	shmipc_destroy(&(*ctx)->read_queue);
	shmipc_destroy(&(*ctx)->write_queue);

	free(*ctx);
	*ctx = NULL;
}

ncv_error ncv_get_args(ncv_context* ctx, int* out_num_args, const char* const* const* * out_args)
{
	*out_args = (const char* const* const*)ctx->args;
	*out_num_args = ctx->num_args;
	return NCV_ERR_SUCCESS;
}

ncv_error ncv_get_num_frames(ncv_context* ctx, int* out_num_frames)
{
	*out_num_frames = *(((uint32_t*)ctx->shm_area) + 2);
	return NCV_ERR_SUCCESS;
}

ncv_error ncv_wait_for_frame(ncv_context* ctx, int timeout, int* out_width, int* out_height, void** out_frame)
{
	const char msg[] = "ready";
	shmipc_error serr = shmipc_send_message(ctx->write_queue, "status", msg, sizeof(msg), timeout); 

	if(serr != SHMIPC_ERR_TIMEOUT && serr != SHMIPC_ERR_SUCCESS)
		return NCV_ERR_SHM;

	if(serr == SHMIPC_ERR_TIMEOUT)
		return NCV_ERR_TIMEOUT;

	char type[SHMIPC_MESSAGE_TYPE_LENGTH];
	char message[shmipc_get_message_max_length(ctx->read_queue)];
	size_t size;
	
	serr = shmipc_recv_message(ctx->read_queue, type, message, &size, timeout);

	if(serr != SHMIPC_ERR_TIMEOUT && serr != SHMIPC_ERR_SUCCESS)
		return NCV_ERR_SHM;

	if(serr == SHMIPC_ERR_TIMEOUT)
		return NCV_ERR_TIMEOUT;

	if(strcmp(type, "cmd") != 0)
		return NCV_ERR_UNKNOWN_MSG;
	
	if(!strcmp(message, "quit"))
		return NCV_ERR_HOST_QUIT;
	
	if(!strcmp(message, "newframe")){
		*out_width = *((uint32_t*)ctx->shm_area);
		*out_height = *(((uint32_t*)ctx->shm_area) + 1);
		*out_frame = ((char*)ctx->shm_area + 4096);

		return NCV_ERR_SUCCESS;
	}

	return NCV_ERR_UNKNOWN_MSG;
}

ncv_error ncv_report_error(ncv_context* ctx, int err_code, const char* err_str, size_t size)
{
	char buffer[SHMIPC_MESSAGE_TYPE_LENGTH] = {0};
	snprintf(buffer, sizeof(buffer), "error %d", err_code);
	shmipc_error serr = shmipc_send_message(ctx->write_queue, buffer, err_str, size, SHMIPC_INFINITE);

	if(serr != SHMIPC_ERR_SUCCESS)
		return NCV_ERR_SHM;

	return NCV_ERR_SUCCESS;
}

ncv_error ncv_report_result(ncv_context* ctx, int timeout, void* data, size_t size)
{
	if(shmipc_get_message_max_length(ctx->write_queue) < size)
		return NCV_ERR_RESULT_TOO_LONG;

	char* buffer;
	shmipc_error serr = shmipc_acquire_buffer_w(ctx->write_queue, &buffer, timeout);

	if(serr == SHMIPC_ERR_TIMEOUT)
		return NCV_ERR_TIMEOUT;

	if(serr != SHMIPC_ERR_SUCCESS)
		return NCV_ERR_SHM;

	memcpy(buffer, data, size);
	
	serr = shmipc_return_buffer_w(ctx->write_queue, &buffer, size, "results");

	if(serr != SHMIPC_ERR_SUCCESS)
		return NCV_ERR_SHM;

	return NCV_ERR_SUCCESS;
}
