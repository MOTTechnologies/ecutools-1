#include "wcbridge.h"

#include <errno.h>
#include <linux/can/raw.h>
#include <linux/can.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

void wcbridge_websocket_onopen(void *websocket) {
	cwebsocket_client *wsclient = (cwebsocket_client *)websocket;
	syslog(LOG_DEBUG, "wcbridge_onopen: websocket file descriptor: %i\n", wsclient->fd);
}

void wcbridge_websocket_onmessage(void *websocket, cwebsocket_message *message) {

	while((bridge->canbus->state & CANBUS_STATE_CONNECTED) == 0) {
		syslog(LOG_DEBUG, "wcbridge_websocket_onmessage: waiting for CAN connection\n");
		sleep(1);
	}

	if(message == NULL || message->payload == NULL || message->payload_len <= 0) return;

	if(strcmp(message->payload, "cmd:log") == 0) {
		if(pthread_create(&bridge->canbus_thread, NULL, wcbridge_canbus_logger_thread, (void *)bridge) == -1) {
			syslog(LOG_ERR, "wcbridge_websocket_onmessage: %s", strerror(errno));
			return;
		}
		syslog(LOG_DEBUG, "thread created\n");
		return;
	}
	else if(strcmp(message->payload, "cmd:nolog") == 0) {
		if(pthread_cancel(bridge->canbus_thread) == -1) {
			syslog(LOG_ERR, "wcbridge_websocket_onmessage: %s", strerror(errno));
			return;
		}
		return;
	}
	else if(strstr(message->payload, "cmd:filter:") != NULL) {
		strtok(message->payload, ":");
		strtok(NULL, ":");
		char *pch2 = strtok(NULL, ":");
		unsigned hex = (int)strtol(pch2, NULL, 16);
		struct can_filter filter[1];
		filter[0].can_id = hex;
		filter[0].can_mask = 0x000007FF;
		if(setsockopt(bridge->canbus->socket, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) == -1) {
			syslog(LOG_ERR, "wcbridge_websocket_onmessage: %s", strerror(errno));
			return;
		}
		syslog(LOG_DEBUG, "wcbridge_websocket_onmessage: filter applied");
		return;
	}
	else if(strcmp(message->payload, "cmd:nofilter") == 0) {
		canbus_close(bridge->canbus);
		if(canbus_connect(bridge->canbus) != 0) {
			syslog(LOG_CRIT, "wcbridge_websocket_onmessage: unable to connect to CAN\n");
			return;
		}
		syslog(LOG_DEBUG, "wcbridge_websocket_onmessage: filter cleared");
		return;
	}
	else if(strcmp(message->payload, "cmd:diagnosticMode") == 0) {
		struct can_frame *frame = malloc(sizeof(struct can_frame));
		memset(frame, 0, sizeof(struct can_frame));
		frame->can_id = strtol("7E0", NULL, 16);
		frame->can_dlc = 8;
	}
	else if(strcmp(message->payload, "cmd:requestSeed") == 0) {

	}
	else {

		syslog(LOG_DEBUG, "wcbridge_websocket_onmessage: processing raw CAN message: %s\n", message->payload);

		if(strstr(message->payload, "#") == NULL) {
			syslog(LOG_DEBUG, "wcbridge_websocket_onmessage: invalid raw CAN payload");
			return;
		}

		char *can_id = strsep(&message->payload, "#");
		char *can_message = strsep(&message->payload, "#");

		struct can_frame *frame = malloc(sizeof(struct can_frame));
		memset(frame, 0, sizeof(struct can_frame));
		frame->can_id = strtol(can_id, NULL, 16);
		frame->can_dlc = 8;

		size_t count = 0;
		for(count = 0; count < sizeof(frame->data)/sizeof(frame->data[0]); count++) {
			sscanf(can_message, "%2hhx", &frame->data[count]);
			can_message += 2 * sizeof(char);
		}

		/*
		int i;
		for(i=0; i<sizeof(bridge->filters); i++) {
			if(bridge->filters[i] != NULL) {
				wcbridge_process_filter(bridge, frame);
			}
		}*/

		if(canbus_write(bridge->canbus, frame) == -1) {
			syslog(LOG_ERR, "wcbridge_websocket_onmessage: unable to forward frame: %s", strerror(errno));
		}

		if(bridge->onmessage != NULL) {
			bridge->onmessage(bridge, frame);
		}

		free(frame);
	}
}

void wcbridge_websocket_onclose(void *websocket, int code, const char *message) {
	cwebsocket_client *wsclient = (cwebsocket_client *)websocket;
	if(message != NULL) {
		syslog(LOG_DEBUG, "wcbridge_websocket_onclose: fd=%i, code=%i, message=%s", wsclient->fd, code, message);
	}
}

void wcbridge_websocket_onerror(void *websocket, const char *error) {
	cwebsocket_client *wsclient = (cwebsocket_client *)websocket;
	syslog(LOG_DEBUG, "wcbridge_websocket_onerror: fd=%i, error=%s\n", wsclient->fd, error);
}

void wcbridge_process_filter(wcbridge *bridge, struct can_frame *frame) {
	syslog(LOG_DEBUG, "wcbridge_process_filter: ");
	canbus_print_frame(frame);
}

void *wcbridge_websocket_thread(void *ptr) {

	syslog(LOG_DEBUG, "wcbridge_websocket_thread: running\n");

	wcbridge *args = (wcbridge *)ptr;

	args->wsclient->subprotocol = cwebsocket_subprotocol_echo_client_new();
	args->wsclient->subprotocol->onopen = &wcbridge_websocket_onopen;
	args->wsclient->subprotocol->onmessage = &wcbridge_websocket_onmessage;
	args->wsclient->subprotocol->onclose = &wcbridge_websocket_onclose;
	args->wsclient->subprotocol->onerror = &wcbridge_websocket_onerror;

	cwebsocket_client_init(args->wsclient, NULL, 0);
	//args->wsclient->flags |= WEBSOCKET_FLAG_AUTORECONNECT;
	//args->wsclient->retry = 5;
	args->wsclient->uri = (char *)WCBRIDGE_WEBSOCKET_ENDPOINT;
	if(cwebsocket_client_connect(args->wsclient) == -1) {
		syslog(LOG_ERR, "wcbridge_websocket_thread: unable to connect to websocket server\n");
	}

	cwebsocket_client_listen(args->wsclient);

	syslog(LOG_DEBUG, "wcbridge_websocket_thread: stopping\n");

	return NULL;
}

void *wcbridge_canbus_connect_thread(void *ptr) {
	if(canbus_connect(bridge->canbus) != 0) {
		syslog(LOG_CRIT, "wcbridge_canbus_connect_thread: unable to connect to CAN\n");
		return NULL;
	}
	return NULL;
}

void *wcbridge_canbus_logger_thread(void *ptr) {

	syslog(LOG_DEBUG, "wcbridge_canbus_thread: running\n");

	wcbridge *bridge = (wcbridge *)ptr;

	int can_frame_len = sizeof(struct can_frame);
	struct can_frame frame;
	memset(&frame, 0, can_frame_len);

	int data_len = can_frame_len + 25;
	char data[data_len];
	memset(data, 0, data_len);

	/*
	while((bridge->websocket->state & WEBSOCKET_STATE_OPEN) == 0) {
		syslog(LOG_DEBUG, "wcbridge_canbus_listen_thread: waiting for websocket to connect\n");
		sleep(1);
	}*/

	while((bridge->canbus->state & CANBUS_STATE_CONNECTED) &&
			(bridge->wsclient->state & WEBSOCKET_STATE_OPEN) &&
			canbus_read(bridge->canbus, &frame) > 0) {

		memset(data, 0, data_len);
		canbus_framecpy(&frame, data);

		if(frame.can_id & CAN_ERR_FLAG) {
			syslog(LOG_ERR, "wcbridge_canbus_logger_thread: CAN ERROR: %s", data);
			continue;
		}

		if(cwebsocket_client_write_data(bridge->wsclient, data, strlen(data), TEXT_FRAME) == -1) {
			syslog(LOG_ERR, "wcbridge_canbus_thread: unable to forward CAN frame to websocket");
		}
	}

	syslog(LOG_DEBUG, "wcbridge_canbus_thread: stopping\n");

	return NULL;
}

wcbridge *wcbridge_new() {
	wcbridge *bridge = malloc(sizeof(wcbridge));
	memset(bridge, 0, sizeof(wcbridge));
	bridge->wsclient = malloc(sizeof(cwebsocket_client));
	memset(bridge->wsclient, 0, sizeof(cwebsocket_client));
	bridge->canbus = malloc(sizeof(canbus_client));
	memset(bridge->canbus, 0, sizeof(canbus_client));
	return bridge;
}

int wcbridge_run(wcbridge *bridge) {
	//pthread_attr_init(&bridge->websocket_thread_attr);
	//pthread_attr_setstacksize(&bridge->websocket_thread_attr, STACK_SIZE_MIN);
	pthread_create(&bridge->websocket_thread, NULL, wcbridge_websocket_thread, (void *)bridge);
	pthread_create(&bridge->canbus_thread, NULL, wcbridge_canbus_connect_thread, (void *)bridge);
	pthread_join(bridge->websocket_thread, NULL);
	syslog(LOG_DEBUG, "wcbridge_run: bridge closed\n");
	return 0;
}

void wcbridge_close(wcbridge *bridge, const char *message) {
	syslog(LOG_DEBUG, "wcbridge_close: closing bridge\n");
	canbus_close(bridge->canbus);
	cwebsocket_client_close(bridge->wsclient, 1000, message);
	wcbridge_destroy(bridge);
	syslog(LOG_DEBUG, "wcbridge_close: bridge closed\n");
}

void wcbridge_destroy(wcbridge *bridge) {
	free(bridge->wsclient);
	free(bridge->canbus);
	free(bridge);
}
