// Copyright (c) 2019 CloudMakers, s. r. o.
// All rights reserved.
//
// EFA focuser command set is extracted from INDI driver written
// by Phil Shepherd with help of Peter Chance.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO PlaneWave EFA focuser driver
 \file indigo_focuser_efa.c
 */


#define DRIVER_VERSION 0x0001
#define DRIVER_NAME "indigo_focuser_efa"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>

#include <sys/time.h>

#include <indigo/indigo_driver_xml.h>
#include <indigo/indigo_io.h>

#include "indigo_focuser_efa.h"

#define PRIVATE_DATA													((efa_private_data *)device->private_data)

#define X_FOCUSER_FANS_PROPERTY								(PRIVATE_DATA->fans_property)
#define X_FOCUSER_FANS_OFF_ITEM								(X_FOCUSER_FANS_PROPERTY->items+0)
#define X_FOCUSER_FANS_ON_ITEM								(X_FOCUSER_FANS_PROPERTY->items+1)

typedef struct {
	int handle;
	indigo_timer *timer;
	pthread_mutex_t mutex;
	indigo_property *fans_property;
} efa_private_data;

// -------------------------------------------------------------------------------- Low level communication routines

static bool efa_command(indigo_device *device, uint8_t *packet_out, uint8_t *packet_in) {
	int count = packet_out[1], sum = 0;
	for (int i = 0; i <= count; i++)
		sum += packet_out[i + 1];
	packet_out[count + 2] = (-sum) & 0xFF;
	if (count == 3)
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d ← %02X %02X %02X→%02X [%02X] %02X", PRIVATE_DATA->handle, packet_out[0], packet_out[1], packet_out[2], packet_out[3], packet_out[4], packet_out[5]);
	else if (count == 4)
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d ← %02X %02X %02X→%02X [%02X %02X] %02X", PRIVATE_DATA->handle, packet_out[0], packet_out[1], packet_out[2], packet_out[3], packet_out[4], packet_out[5], packet_out[6]);
	else if (count == 5)
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d ← %02X %02X %02X→%02X [%02X %02X %02X] %02X", PRIVATE_DATA->handle, packet_out[0], packet_out[1], packet_out[2], packet_out[3], packet_out[4], packet_out[5], packet_out[6], packet_out[7]);
	else if (count == 6)
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d ← %02X %02X %02X→%02X [%02X %02X %02X %02X] %02X", PRIVATE_DATA->handle, packet_out[0], packet_out[1], packet_out[2], packet_out[3], packet_out[4], packet_out[5], packet_out[6], packet_out[7], packet_out[8]);
	if (indigo_write(PRIVATE_DATA->handle, (const char *)packet_out, count + 3)) {
		if (indigo_read(PRIVATE_DATA->handle, (char *)packet_in, 5) == 5 && packet_in[0] == 0x3B) {
			count = packet_in[1];
			if (indigo_read(PRIVATE_DATA->handle, (char *)packet_in + 5, count - 2) == count - 2) {
				if (count == 3)
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d → %02X %02X %02X→%02X [%02X] %02X", PRIVATE_DATA->handle, packet_in[0], packet_in[1], packet_in[2], packet_in[3], packet_in[4], packet_in[5]);
				else if (count == 4)
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d → %02X %02X %02X→%02X [%02X %02X] %02X", PRIVATE_DATA->handle, packet_in[0], packet_in[1], packet_in[2], packet_in[3], packet_in[4], packet_in[5], packet_in[6]);
				else if (count == 5)
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d → %02X %02X %02X→%02X [%02X %02X %02X] %02X", PRIVATE_DATA->handle, packet_in[0], packet_in[1], packet_in[2], packet_in[3], packet_in[4], packet_in[5], packet_in[6], packet_in[7]);
				else if (count == 6)
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d → %02X %02X %02X→%02X [%02X %02X %02X %02X] %02X", PRIVATE_DATA->handle, packet_in[0], packet_in[1], packet_in[2], packet_in[3], packet_in[4], packet_in[5], packet_in[6], packet_in[7], packet_in[8]);
				return packet_in[2] == packet_out[3] && packet_in[4] == packet_out[4];
			}
		} else {
			INDIGO_DRIVER_DEBUG(DRIVER_NAME, "%d → %02X %02X %02X→%02X [%02X] !!!", PRIVATE_DATA->handle, packet_in[0], packet_in[1], packet_in[2], packet_in[3], packet_in[4]);
		}
	}
	return false;
}

// -------------------------------------------------------------------------------- INDIGO focuser device implementation

static indigo_result focuser_attach(indigo_device *device) {
	assert(device != NULL);
	assert(PRIVATE_DATA != NULL);
	if (indigo_focuser_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		X_FOCUSER_FANS_PROPERTY = indigo_init_switch_property(NULL, device->name, "X_FOCUSER_FANS", FOCUSER_MAIN_GROUP, "Fans", INDIGO_OK_STATE, INDIGO_RW_PERM, INDIGO_ONE_OF_MANY_RULE, 2);
		if (X_FOCUSER_FANS_PROPERTY == NULL)
			return INDIGO_FAILED;
		indigo_init_switch_item(X_FOCUSER_FANS_OFF_ITEM, "OFF", "Off", true);
		indigo_init_switch_item(X_FOCUSER_FANS_ON_ITEM, "ON", "On", false);
		// -------------------------------------------------------------------------------- DEVICE_PORT, DEVICE_PORTS
		DEVICE_PORT_PROPERTY->hidden = false;
		DEVICE_PORTS_PROPERTY->hidden = false;
#ifdef INDIGO_LINUX
		strcpy(DEVICE_PORT_ITEM->text.value, "/dev/usb_focuser");
#endif
		// -------------------------------------------------------------------------------- INFO
		INFO_PROPERTY->count = 5;
		strcpy(INFO_DEVICE_MODEL_ITEM->text.value, "EFA Focuser");
		// -------------------------------------------------------------------------------- FOCUSER_TEMPERATURE
		FOCUSER_TEMPERATURE_PROPERTY->hidden = false;
		// -------------------------------------------------------------------------------- FOCUSER_SPEED
		FOCUSER_SPEED_PROPERTY->hidden = true;
		// -------------------------------------------------------------------------------- FOCUSER_STEPS
		FOCUSER_STEPS_ITEM->number.min = 0;
		FOCUSER_STEPS_ITEM->number.max = 3799422;
		FOCUSER_STEPS_ITEM->number.step = 1;
		// -------------------------------------------------------------------------------- FOCUSER_LIMITS
		FOCUSER_LIMITS_PROPERTY->hidden = false;
		FOCUSER_LIMITS_MIN_POSITION_ITEM->number.max = 3799422;
		FOCUSER_LIMITS_MIN_POSITION_ITEM->number.min = FOCUSER_LIMITS_MIN_POSITION_ITEM->number.value = FOCUSER_LIMITS_MIN_POSITION_ITEM->number.target = 0;
		FOCUSER_LIMITS_MAX_POSITION_ITEM->number.min = 0;
		FOCUSER_LIMITS_MAX_POSITION_ITEM->number.max = FOCUSER_LIMITS_MAX_POSITION_ITEM->number.value = FOCUSER_LIMITS_MAX_POSITION_ITEM->number.target = 3799422;
		// -------------------------------------------------------------------------------- FOCUSER_ON_POSITION_SET
		FOCUSER_ON_POSITION_SET_PROPERTY->hidden = false;
		// --------------------------------------------------------------------------------
		// TBD
		// --------------------------------------------------------------------------------
		pthread_mutex_init(&PRIVATE_DATA->mutex, NULL);
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return indigo_focuser_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result focuser_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property) {
	if (IS_CONNECTED) {
		if (indigo_property_match(X_FOCUSER_FANS_PROPERTY, property))
			indigo_define_property(device, X_FOCUSER_FANS_PROPERTY, NULL);
	}
	return indigo_focuser_enumerate_properties(device, NULL, NULL);
}

static void focuser_timer_callback(indigo_device *device) {
	if (!IS_CONNECTED)
		return;
	pthread_mutex_lock(&PRIVATE_DATA->mutex);
	uint8_t response_packet[10];
	uint8_t get_temp_packet[10] = { 0x3B, 0x04, 0x20, 0x12, 0x26, 0x00, 0 };
	if (efa_command(device, get_temp_packet, response_packet)) {
		int raw_temp = response_packet[6] << 8 | response_packet[7];
		// formula is definitely wrong :(
		bool neg = false;
		if (raw_temp > 32768) {
			raw_temp = 65536 - raw_temp;
			neg = true;
		}
		int int_part = raw_temp / 16;
		int fraction_part = (raw_temp - int_part) * 625 / 1000;
		double temp = int_part + fraction_part / 10.0;
		if (neg)
			temp = -temp;
		if (FOCUSER_TEMPERATURE_ITEM->number.value != temp) {
			FOCUSER_TEMPERATURE_ITEM->number.value = temp;
			FOCUSER_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, FOCUSER_TEMPERATURE_PROPERTY, NULL);
		}
	}
	bool moving = false;
	uint8_t check_state_packet[10] = { 0x3B, 0x03, 0x20, 0x12, 0x13, 0 };
	if (efa_command(device, check_state_packet, response_packet)) {
		moving = response_packet[5] == 0;
	}
	long position = 0;
	uint8_t get_position_packet[10] = { 0x3B, 0x03, 0x20, 0x12, 0x01, 0 };
	if (efa_command(device, get_position_packet, response_packet)) {
		position = response_packet[5] << 16 | response_packet[6] << 8 | response_packet[7];
	}
	bool update = false;
	if (moving) {
		if (FOCUSER_STEPS_PROPERTY->state != INDIGO_BUSY_STATE) {
			FOCUSER_STEPS_PROPERTY->state = INDIGO_BUSY_STATE;
			update = true;
		}
		if (FOCUSER_POSITION_PROPERTY->state != INDIGO_BUSY_STATE) {
			FOCUSER_POSITION_PROPERTY->state = INDIGO_BUSY_STATE;
			update = true;
		}
	} else {
		if (FOCUSER_STEPS_PROPERTY->state == INDIGO_BUSY_STATE) {
			FOCUSER_STEPS_PROPERTY->state = INDIGO_OK_STATE;
			update = true;
		}
		if (FOCUSER_POSITION_PROPERTY->state == INDIGO_BUSY_STATE) {
			FOCUSER_POSITION_PROPERTY->state = INDIGO_OK_STATE;
			update = true;
		}
	}
	if (FOCUSER_POSITION_ITEM->number.value != position) {
		FOCUSER_POSITION_ITEM->number.value = position;
		update = true;
	}
	if (update) {
		indigo_update_property(device, FOCUSER_STEPS_PROPERTY, NULL);
		indigo_update_property(device, FOCUSER_POSITION_PROPERTY, NULL);
	}
	indigo_reschedule_timer(device, moving ? 0.5 : 1, &PRIVATE_DATA->timer);
	pthread_mutex_unlock(&PRIVATE_DATA->mutex);
}

static void focuser_connection_handler(indigo_device *device) {
	pthread_mutex_lock(&PRIVATE_DATA->mutex);
	if (CONNECTION_CONNECTED_ITEM->sw.value) {
		uint8_t response_packet[10];
		CONNECTION_PROPERTY->state = INDIGO_BUSY_STATE;
		indigo_update_property(device, CONNECTION_PROPERTY, NULL);
		PRIVATE_DATA->handle = indigo_open_serial_with_speed(DEVICE_PORT_ITEM->text.value, 19200);
		if (PRIVATE_DATA->handle > 0) {
			uint8_t get_version_packet[10] = { 0x3B, 0x03, 0x20, 0x12, 0xFE, 0 };
			if (efa_command(device, get_version_packet, response_packet)) {
				INDIGO_DRIVER_LOG(DRIVER_NAME, "EFA %d.%d detected", response_packet[5], response_packet[6]);
			} else {
				INDIGO_DRIVER_ERROR(DRIVER_NAME, "EFA not detected");
				close(PRIVATE_DATA->handle);
				PRIVATE_DATA->handle = 0;
			}
		}
		if (PRIVATE_DATA->handle > 0) {
			uint8_t get_position_packet[10] = { 0x3B, 0x03, 0x20, 0x12, 0x01, 0 };
			if (efa_command(device, get_position_packet, response_packet)) {
				FOCUSER_POSITION_ITEM->number.value = response_packet[5] << 16 | response_packet[6] << 8 | response_packet[7];
			}
			uint8_t get_fans_packet[10] = { 0x3B, 0x03, 0x20, 0x13, 0x28, 0 };
			if (efa_command(device, get_fans_packet, response_packet)) {
				indigo_set_switch(X_FOCUSER_FANS_PROPERTY, X_FOCUSER_FANS_ON_ITEM, response_packet[5] == 0);
			}
			uint8_t set_stop_detect_packet[10] = { 0x3B, 0x04, 0x20, 0x12, 0xEF, 0x01, 0 };
			efa_command(device, set_stop_detect_packet, response_packet);
			indigo_define_property(device, X_FOCUSER_FANS_PROPERTY, NULL);
			PRIVATE_DATA->timer = indigo_set_timer(device, 0, focuser_timer_callback);
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		} else {
			CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
		}
	} else {
		if (PRIVATE_DATA->handle > 0) {
			indigo_cancel_timer(device, &PRIVATE_DATA->timer);
			indigo_delete_property(device, X_FOCUSER_FANS_PROPERTY, NULL);
			INDIGO_DRIVER_LOG(DRIVER_NAME, "Disconnected");
			close(PRIVATE_DATA->handle);
			PRIVATE_DATA->handle = 0;
		}
		CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
	}
	indigo_focuser_change_property(device, NULL, CONNECTION_PROPERTY);
	pthread_mutex_unlock(&PRIVATE_DATA->mutex);
}

static void focuser_steps_handler(indigo_device *device) {
	pthread_mutex_lock(&PRIVATE_DATA->mutex);
	long position = FOCUSER_POSITION_ITEM->number.value + (FOCUSER_DIRECTION_MOVE_OUTWARD_ITEM->sw.value ? 1 : -1) * FOCUSER_STEPS_ITEM->number.value;
	if (position < FOCUSER_LIMITS_MIN_POSITION_ITEM->number.value)
		position = FOCUSER_LIMITS_MIN_POSITION_ITEM->number.value;
	if (position > FOCUSER_LIMITS_MAX_POSITION_ITEM->number.value)
		position = FOCUSER_LIMITS_MAX_POSITION_ITEM->number.value;
	uint8_t response_packet[10];
	uint8_t goto_packet[10] = { 0x3B, 0x06, 0x20, 0x12, 0x17, (position >> 16) & 0xFF, (position >> 8) & 0xFF, position & 0xFF, 0 };
	if (efa_command(device, goto_packet, response_packet)) {
		FOCUSER_STEPS_PROPERTY->state = INDIGO_BUSY_STATE;
		FOCUSER_POSITION_PROPERTY->state = INDIGO_BUSY_STATE;
	}
	pthread_mutex_unlock(&PRIVATE_DATA->mutex);
}

static void focuser_position_handler(indigo_device *device) {
	pthread_mutex_lock(&PRIVATE_DATA->mutex);
	long position = FOCUSER_POSITION_ITEM->number.value;
	if (position < FOCUSER_LIMITS_MIN_POSITION_ITEM->number.value)
		position = FOCUSER_LIMITS_MIN_POSITION_ITEM->number.value;
	if (position > FOCUSER_LIMITS_MAX_POSITION_ITEM->number.value)
		position = FOCUSER_LIMITS_MAX_POSITION_ITEM->number.value;
	uint8_t response_packet[10];
	uint8_t goto_packet[10] = { 0x3B, 0x06, 0x20, 0x12, 0x17, (position >> 16) & 0xFF, (position >> 8) & 0xFF, position & 0xFF, 0 };
	if (FOCUSER_ON_POSITION_SET_SYNC_ITEM->sw.value)
		goto_packet[4] = 0x04;
	if (efa_command(device, goto_packet, response_packet)) {
		FOCUSER_STEPS_PROPERTY->state = INDIGO_BUSY_STATE;
		FOCUSER_POSITION_PROPERTY->state = INDIGO_BUSY_STATE;
	}
	pthread_mutex_unlock(&PRIVATE_DATA->mutex);
}

static void focuser_abort_motion_handler(indigo_device *device) {
	pthread_mutex_lock(&PRIVATE_DATA->mutex);
	uint8_t response_packet[10];
	uint8_t stop_packet[10] = { 0x3B, 0x06, 0x20, 0x12, 0x24, 0x00, 0 };
	if (efa_command(device, stop_packet, response_packet)) {
		FOCUSER_ABORT_MOTION_PROPERTY->state = INDIGO_OK_STATE;
	} else {
		FOCUSER_ABORT_MOTION_PROPERTY->state = INDIGO_ALERT_STATE;
	}
	indigo_update_property(device, FOCUSER_ABORT_MOTION_PROPERTY, NULL);
	pthread_mutex_unlock(&PRIVATE_DATA->mutex);
}

static void focuser_fans_handler(indigo_device *device) {
	pthread_mutex_lock(&PRIVATE_DATA->mutex);
	uint8_t response_packet[10];
	uint8_t fans_packet[10] = { 0x3B, 0x04, 0x20, 0x13, 0x27, X_FOCUSER_FANS_ON_ITEM->sw.value ? 0x01 : 0x00, 0 };
	if (efa_command(device, fans_packet, response_packet)) {
		X_FOCUSER_FANS_PROPERTY->state = INDIGO_OK_STATE;
	} else {
		X_FOCUSER_FANS_PROPERTY->state = INDIGO_ALERT_STATE;
	}
	indigo_update_property(device, X_FOCUSER_FANS_PROPERTY, NULL);
	pthread_mutex_unlock(&PRIVATE_DATA->mutex);
}

static indigo_result focuser_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		CONNECTION_PROPERTY->state = INDIGO_BUSY_STATE;
		indigo_set_timer(device, 0, focuser_connection_handler);
	} else if (indigo_property_match(FOCUSER_STEPS_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- FOCUSER_STEPS
		indigo_property_copy_values(FOCUSER_STEPS_PROPERTY, property, false);
		indigo_set_timer(device, 0, focuser_steps_handler);
		return INDIGO_OK;
	} else if (indigo_property_match(FOCUSER_POSITION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- FOCUSER_POSITION
		indigo_property_copy_values(FOCUSER_POSITION_PROPERTY, property, false);
		indigo_set_timer(device, 0, focuser_position_handler);
		return INDIGO_OK;
	} else if (indigo_property_match(FOCUSER_ABORT_MOTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- FOCUSER_ABORT_MOTION
		indigo_property_copy_values(FOCUSER_ABORT_MOTION_PROPERTY, property, false);
		indigo_set_timer(device, 0, focuser_abort_motion_handler);
		return INDIGO_OK;
	} else if (indigo_property_match(X_FOCUSER_FANS_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- X_FOCUSER_FANS
		indigo_property_copy_values(X_FOCUSER_FANS_PROPERTY, property, false);
		indigo_set_timer(device, 0, focuser_fans_handler);
		return INDIGO_OK;
	}
	return indigo_focuser_change_property(device, client, property);
}

static indigo_result focuser_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value) {
		indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
		focuser_connection_handler(device);
	}
	indigo_release_property(X_FOCUSER_FANS_PROPERTY);
	pthread_mutex_destroy(&PRIVATE_DATA->mutex);
	INDIGO_DEVICE_DETACH_LOG(DRIVER_NAME, device->name);
	return indigo_focuser_detach(device);
}

// -------------------------------------------------------------------------------- INDIGO driver implementation

indigo_result indigo_focuser_efa(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;
	static efa_private_data *private_data = NULL;
	static indigo_device *focuser = NULL;

	static indigo_device focuser_template = INDIGO_DEVICE_INITIALIZER(
		"EFA Focuser",
		focuser_attach,
		focuser_enumerate_properties,
		focuser_change_property,
		NULL,
		focuser_detach
	);

	SET_DRIVER_INFO(info, "PlaneWave EFA Focuser", __FUNCTION__, DRIVER_VERSION, false, last_action);

	if (action == last_action)
		return INDIGO_OK;

	switch (action) {
		case INDIGO_DRIVER_INIT:
			last_action = action;
			private_data = malloc(sizeof(efa_private_data));
			assert(private_data != NULL);
			memset(private_data, 0, sizeof(efa_private_data));
			focuser = malloc(sizeof(indigo_device));
			assert(focuser != NULL);
			memcpy(focuser, &focuser_template, sizeof(indigo_device));
			focuser->private_data = private_data;
			indigo_attach_device(focuser);
			break;

		case INDIGO_DRIVER_SHUTDOWN:
			last_action = action;
			if (focuser != NULL) {
				indigo_detach_device(focuser);
				free(focuser);
				focuser = NULL;
			}
			if (private_data != NULL) {
				free(private_data);
				private_data = NULL;
			}
			break;

		case INDIGO_DRIVER_INFO:
			break;
	}

	return INDIGO_OK;
}
