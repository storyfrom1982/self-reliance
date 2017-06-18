/*
 * sr_message.c
 *
 *  Created on: 2017年6月18日
 *      Author: kly
 */


#include "sr_message.h"

#include "sr_common.h"
#include "sr_pipe.h"
#include "sr_mutex.h"
#include "sr_malloc.h"

struct Sr_message_listener
{
	bool running;
	bool stopped;
	pthread_t tid;
	Sr_pipe *pipe;
	Sr_mutex *mutex;
	unsigned int msg_size;
	Sr_message_callback *cb;
};

static void *sr_message_listener_loop(void *p)
{
	logd("enter\n");

	int result = 0;
	Sr_message msg = {0};
	Sr_message_listener *listener = (Sr_message_listener *) p;

	while (ISTRUE(listener->running)) {
		msg.size = 0;
		if ((result = sr_message_listener_pop(listener, &msg)) < 0) {
			loge(result);
			break;
		}
		listener->cb->notify(listener->cb, msg);
	}

	SETTRUE(listener->stopped);

	logd("exit\n");

	return NULL;
}

int sr_message_listener_create(Sr_message_callback *cb, Sr_message_listener **pp_listener)
{
	logd("enter\n");

	int result = 0;
	Sr_message_listener *listener = NULL;

	if (pp_listener == NULL) {
		loge(ERRPARAM);
		return ERRPARAM;
	}

	if ((listener = (Sr_message_listener *) calloc(1, sizeof(Sr_message_listener))) == NULL) {
		loge(ERRMALLOC);
		return ERRMALLOC;
	}

	listener->cb = cb;
	listener->msg_size = sizeof(Sr_message);

	result = sr_mutex_create(&(listener->mutex));
	if (result != 0) {
		free(listener);
		loge(result);
		return result;
	}

	result = sr_pipe_create(65536, &(listener->pipe));
	if (result != 0) {
		sr_mutex_release(&listener->mutex);
		free(listener);
		loge(result);
		return result;
	}

	SETTRUE(listener->running);

	if (listener->cb != NULL && listener->cb->notify != NULL){
		result = pthread_create(&(listener->tid), NULL, sr_message_listener_loop, listener);
		if (result != 0) {
			listener->tid = 0;
			sr_pipe_release(&listener->pipe);
			sr_mutex_release(&listener->mutex);
			free(listener);
			loge(ERRSYSCALL);
			return ERRSYSCALL;
		}
	}

	*pp_listener = listener;

	logd("exit\n");

	return 0;
}

void sr_message_listener_release(Sr_message_listener **pp_listener)
{
	logd("enter\n");

	if (pp_listener && *pp_listener) {
		Sr_message_listener *listener = *pp_listener;
		*pp_listener = NULL;
		sr_mutex_lock(listener->mutex);
		SETFALSE(listener->running);
		sr_mutex_broadcast(listener->mutex);
		sr_mutex_unlock(listener->mutex);
		if (listener->tid != 0){
			while(ISFALSE(listener->stopped)){
				sr_mutex_lock(listener->mutex);
				sr_mutex_broadcast(listener->mutex);
				sr_mutex_unlock(listener->mutex);
				nanosleep((const struct timespec[]){{0, 1000L}}, NULL);
			}
			pthread_join(listener->tid, NULL);
		}
		sr_pipe_release(&(listener->pipe));
		sr_mutex_release(&(listener->mutex));
		free(listener);
	}

	logd("exit\n");
}

int sr_message_listener_push(Sr_message_listener *listener, Sr_message msg)
{
	if (listener == NULL) {
		loge(ERRPARAM);
		return ERRPARAM;
	}

	sr_mutex_lock(listener->mutex);

	if (sr_pipe_writable(listener->pipe) < (listener->msg_size + msg.size)) {
		sr_mutex_wait(listener->mutex);
		if (ISFALSE(listener->running)){
			sr_mutex_unlock(listener->mutex);
			return ERREOF;
		}
	}

	sr_pipe_write(listener->pipe, (uint8_t *) &msg, listener->msg_size);
	if (msg.size > 0 && msg.data != NULL){
		sr_pipe_write(listener->pipe, (uint8_t *) &(msg.data), msg.size);
	}

	sr_mutex_signal(listener->mutex);
	sr_mutex_unlock(listener->mutex);

	return 0;
}

int sr_message_listener_push_event(Sr_message_listener *listener, int event)
{
	Sr_message msg = { .event = event, .i64 = 0, .size = 0, .data = NULL };

	if (listener == NULL) {
		loge(ERRPARAM);
		return ERRPARAM;
	}

	sr_mutex_lock(listener->mutex);

	if (sr_pipe_writable(listener->pipe) < (listener->msg_size)) {
		sr_mutex_wait(listener->mutex);
		if (ISFALSE(listener->running)){
			sr_mutex_unlock(listener->mutex);
			return ERREOF;
		}
	}

	sr_pipe_write(listener->pipe, (uint8_t *) &msg, listener->msg_size);

	sr_mutex_signal(listener->mutex);
	sr_mutex_unlock(listener->mutex);

	return 0;
}

int sr_message_listener_pop(Sr_message_listener *listener, Sr_message *msg)
{
	if (listener == NULL || msg == NULL) {
		loge(ERRPARAM);
		return ERRPARAM;
	}

	sr_mutex_lock(listener->mutex);

	if (sr_pipe_readable(listener->pipe) < listener->msg_size) {
		sr_mutex_wait(listener->mutex);
		if (ISFALSE(listener->running)){
			sr_mutex_unlock(listener->mutex);
			return ERREOF;
		}
	}

	sr_pipe_read(listener->pipe, (uint8_t *)msg, listener->msg_size);
	if (msg->size > 0){
		if ((msg->data = malloc(msg->size)) == NULL){
			sr_mutex_unlock(listener->mutex);
			loge(ERRMALLOC);
			return ERRMALLOC;
		}
		sr_pipe_read(listener->pipe, (uint8_t *)(msg->data), msg->size);
	}

	sr_mutex_signal(listener->mutex);
	sr_mutex_unlock(listener->mutex);

	return 0;
}