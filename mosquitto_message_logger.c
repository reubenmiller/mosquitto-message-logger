/*
Copyright (c) 2021 Roger Light <roger@atchoo.org>
Copyright (c) 2026 thin-edge.io contributors

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
   thin-edge.io contributors - message logging functionality.
*/

/*
 * Log MQTT messages with metadata.
 *
 * This plugin logs all MQTT messages passing through the broker with comprehensive
 * metadata including timestamps, topics, QoS, retain flags, client IDs, and payloads.
 *
 * Configuration via environment variables:
 *   MQTT_LOG_DIR - Directory for log files (default: /var/log/mosquitto)
 *   MQTT_LOG_STDERR - Set to "1" to also log to stderr in mosquitto_sub format
 *
 * Note that this only works on Mosquitto 2.0 or later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <ctype.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>

#include <mosquitto.h>
#include <mosquitto_broker.h>
#include <mosquitto_plugin.h>

#define UNUSED(A) (void)(A)

/* Plugin version function for mosquitto 2.0+ */
int mosquitto_plugin_version(int supported_version_count, const int *supported_versions)
{
    UNUSED(supported_version_count);
    UNUSED(supported_versions);
    return 5;
}

static mosquitto_plugin_id_t *mosq_pid = NULL;
static char log_file_path[1024] = {0};
static int log_to_stderr = 0;

/* Base64 encoding table */
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Create directory recursively */
static int mkdir_recursive(const char *path, mode_t mode)
{
	char tmp[1024];
	char *p = NULL;
	size_t len;
	struct stat st;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if(tmp[len - 1] == '/'){
		tmp[len - 1] = 0;
	}

	for(p = tmp + 1; *p; p++){
		if(*p == '/'){
			*p = 0;
			if(stat(tmp, &st) != 0){
				if(mkdir(tmp, mode) != 0 && errno != EEXIST){
					return -1;
				}
			}
			*p = '/';
		}
	}
	
	if(stat(tmp, &st) != 0){
		if(mkdir(tmp, mode) != 0 && errno != EEXIST){
			return -1;
		}
	}
	
	return 0;
}

/* Base64 encode */
static char *base64_encode(const unsigned char *input, size_t length)
{
	size_t output_length = 4 * ((length + 2) / 3);
	char *encoded = malloc(output_length + 1);
	
	if(!encoded) return NULL;
	
	size_t i, j;
	for(i = 0, j = 0; i < length;){
		uint32_t octet_a = i < length ? input[i++] : 0;
		uint32_t octet_b = i < length ? input[i++] : 0;
		uint32_t octet_c = i < length ? input[i++] : 0;
		uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

		encoded[j++] = base64_chars[(triple >> 18) & 0x3F];
		encoded[j++] = base64_chars[(triple >> 12) & 0x3F];
		encoded[j++] = base64_chars[(triple >> 6) & 0x3F];
		encoded[j++] = base64_chars[triple & 0x3F];
	}

	/* Add padding */
	size_t mod = length % 3;
	if(mod == 1){
		encoded[output_length - 2] = '=';
		encoded[output_length - 1] = '=';
	}else if(mod == 2){
		encoded[output_length - 1] = '=';
	}

	encoded[output_length] = '\0';
	return encoded;
}

/* Convert to hex string */
static char *to_hex(const unsigned char *input, size_t length)
{
	char *hex = malloc(length * 2 + 1);
	if(!hex) return NULL;
	
	for(size_t i = 0; i < length; i++){
		sprintf(hex + (i * 2), "%02x", input[i]);
	}
	hex[length * 2] = '\0';
	return hex;
}

/* Check if payload appears to be binary */
static int is_binary(const unsigned char *payload, size_t len)
{
	if(len == 0) return 0;
	
	size_t check_len = len < 1024 ? len : 1024;
	size_t null_count = 0;
	size_t binary_count = 0;
	
	for(size_t i = 0; i < check_len; i++){
		if(payload[i] == 0){
			null_count++;
		}else if(payload[i] < 32 && payload[i] != '\t' && payload[i] != '\n' && payload[i] != '\r'){
			binary_count++;
		}
	}
	
	/* Consider binary if more than 10% null bytes or control characters */
	return (null_count > check_len / 10) || (binary_count > check_len / 10);
}

/* Escape JSON string */
static char *json_escape(const char *str, size_t len)
{
	size_t i, j;
	size_t escaped_len = 0;
	char *escaped;
	
	/* Calculate required size */
	for(i = 0; i < len; i++){
		unsigned char c = (unsigned char)str[i];
		if(c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t'){
			escaped_len += 2;
		}else if(c < 32){
			escaped_len += 6; /* \uXXXX */
		}else{
			escaped_len++;
		}
	}
	
	escaped = malloc(escaped_len + 1);
	if(!escaped) return NULL;
	
	for(i = 0, j = 0; i < len; i++){
		unsigned char c = (unsigned char)str[i];
		if(c == '"'){
			escaped[j++] = '\\';
			escaped[j++] = '"';
		}else if(c == '\\'){
			escaped[j++] = '\\';
			escaped[j++] = '\\';
		}else if(c == '\b'){
			escaped[j++] = '\\';
			escaped[j++] = 'b';
		}else if(c == '\f'){
			escaped[j++] = '\\';
			escaped[j++] = 'f';
		}else if(c == '\n'){
			escaped[j++] = '\\';
			escaped[j++] = 'n';
		}else if(c == '\r'){
			escaped[j++] = '\\';
			escaped[j++] = 'r';
		}else if(c == '\t'){
			escaped[j++] = '\\';
			escaped[j++] = 't';
		}else if(c < 32){
			sprintf(&escaped[j], "\\u%04x", c);
			j += 6;
		}else{
			escaped[j++] = (char)c;
		}
	}
	escaped[j] = '\0';
	return escaped;
}

/* Get ISO 8601 timestamp */
static void get_iso8601_timestamp(char *buf, size_t buflen, struct timeval *tv)
{
	struct tm tm_info;
	gmtime_r(&tv->tv_sec, &tm_info);
	size_t len = strftime(buf, buflen, "%Y-%m-%dT%H:%M:%S", &tm_info);
	snprintf(buf + len, buflen - len, ".%06ld+0000", (long)tv->tv_usec);
}

static int callback_message_in(int event, void *event_data, void *userdata)
{
	struct mosquitto_evt_message *ed = event_data;
	struct timeval tv;
	char timestamp_iso[64];
	const char *client_id = NULL;
	FILE *log_file = NULL;
	
	UNUSED(event);
	UNUSED(userdata);

	gettimeofday(&tv, NULL);
	get_iso8601_timestamp(timestamp_iso, sizeof(timestamp_iso), &tv);
	
	/* Get client ID */
	if(ed->client){
		client_id = mosquitto_client_id(ed->client);
	}
	
	/* Log to file */
	if(log_file_path[0] != '\0'){
		log_file = fopen(log_file_path, "a");
		if(log_file){
			char *payload_str = NULL;
			int is_payload_binary = is_binary(ed->payload, ed->payloadlen);
			
			if(is_payload_binary){
				/* Encode binary as base64 */
				char *encoded = base64_encode(ed->payload, ed->payloadlen);
				if(encoded){
					payload_str = malloc(strlen(encoded) + 20);
					if(payload_str){
						sprintf(payload_str, "\"payload_base64\":\"%s\"", encoded);
					}
					free(encoded);
				}
			}else{
				/* Escape text payload */
				char *escaped = json_escape((const char *)ed->payload, ed->payloadlen);
				if(escaped){
					payload_str = malloc(strlen(escaped) + 20);
					if(payload_str){
						sprintf(payload_str, "\"payload\":\"%s\"", escaped);
					}
					free(escaped);
				}
			}
			
			fprintf(log_file, "{\"timestamp\":\"%s\",\"topic\":\"%s\",\"qos\":%d,\"retain\":%d,\"payloadlen\":%d",
				timestamp_iso, ed->topic, ed->qos, ed->retain ? 1 : 0, ed->payloadlen);
			
			if(client_id){
				fprintf(log_file, ",\"client_id\":\"%s\"", client_id);
			}
			
			if(payload_str){
				fprintf(log_file, ",%s", payload_str);
				free(payload_str);
			}
			
			fprintf(log_file, "}\n");
			fclose(log_file);
		}
	}
	
	/* Log to stderr in mosquitto_sub format */
	if(log_to_stderr){
		char *escaped_payload = NULL;
		char *hex_payload = NULL;
		
		if(ed->payloadlen > 0){
			escaped_payload = json_escape((const char *)ed->payload, ed->payloadlen);
			hex_payload = to_hex(ed->payload, ed->payloadlen);
		}
		
		fprintf(stderr, "MQTT_LOG: {\"timestamp\":%.9f,\"message\":{\"tst\":\"%s\",\"topic\":\"%s\",\"qos\":%d,\"retain\":%d,\"payloadlen\":%d",
			tv.tv_sec + tv.tv_usec / 1000000.0,
			timestamp_iso,
			ed->topic,
			ed->qos,
			ed->retain ? 1 : 0,
			ed->payloadlen);
		
		if(escaped_payload){
			fprintf(stderr, ",\"payload\":\"%s\"", escaped_payload);
			free(escaped_payload);
		}
		
		fprintf(stderr, "}");
		
		if(hex_payload){
			fprintf(stderr, ",\"payload_hex\":\"%s\"", hex_payload);
			free(hex_payload);
		}
		
		fprintf(stderr, "}\n");
		fflush(stderr);
	}

	return MOSQ_ERR_SUCCESS;
}


int mosquitto_plugin_init(mosquitto_plugin_id_t *identifier, void **user_data, struct mosquitto_opt *opts, int opt_count)
{
	const char *log_dir;
	char date_str[32];
	time_t now;
	struct tm *tm_info;
	
	UNUSED(user_data);
	UNUSED(opts);
	UNUSED(opt_count);

	mosq_pid = identifier;
	
	/* Get log directory from environment */
	log_dir = getenv("MQTT_LOG_DIR");
	if(!log_dir){
		log_dir = "/var/log/mosquitto";
	}
	
	/* Create log directory if it doesn't exist */
	if(mkdir_recursive(log_dir, 0755) != 0){
		fprintf(stderr, "Warning: Failed to create log directory %s: %s\n", log_dir, strerror(errno));
		/* Continue anyway, we'll try to write */
	}
	
	/* Create log file path with date */
	now = time(NULL);
	tm_info = localtime(&now);
	strftime(date_str, sizeof(date_str), "%Y%m%d", tm_info);
	snprintf(log_file_path, sizeof(log_file_path), "%s/mqtt-messages-%s.log", log_dir, date_str);
	
	/* Check if stderr logging is enabled */
	const char *stderr_env = getenv("MQTT_LOG_STDERR");
	if(stderr_env && strcmp(stderr_env, "1") == 0){
		log_to_stderr = 1;
	}
	
	mosquitto_callback_register(mosq_pid, MOSQ_EVT_MESSAGE, callback_message_in, NULL, NULL);

	return MOSQ_ERR_SUCCESS;
}


int mosquitto_plugin_cleanup(void *user_data, struct mosquitto_opt *opts, int opt_count)
{
	UNUSED(user_data);
	UNUSED(opts);
	UNUSED(opt_count);

	return MOSQ_ERR_SUCCESS;
}
