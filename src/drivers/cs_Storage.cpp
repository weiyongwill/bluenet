/*
 * Author: Anne van Rossum
 * Copyright: Distributed Organisms B.V. (DoBots)
 * Date: 24 Nov., 2014
 * License: LGPLv3+, Apache, and/or MIT, your choice
 */

/*
 * TODO: Check https://devzone.nordicsemi.com/question/1745/how-to-handle-flashwrit-in-an-safe-way/
 *       Writing to persistent memory should be done between connection/advertisement events...
 */

/*
 * For more information see:
 * http://developer.nordicsemi.com/nRF51_SDK/doc/7.0.1/s110/html/a00763.html#ga0a57b964c8945eaca2d267835ef6688c
 */
#include "drivers/cs_Storage.h"

#include <climits>
#include <float.h>

#include <drivers/cs_Serial.h>
#include <storage/cs_Settings.h>
#include <storage/cs_State.h>
#include <events/cs_EventDispatcher.h>
#if BUILD_MESHING == 1
#include <mesh/cs_Mesh.h>
#endif

extern "C"  {

	static void pstorage_callback_handler(pstorage_handle_t * handle, uint8_t op_code, uint32_t result, uint8_t * p_data,
			uint32_t data_len) {
		// we might want to check if things are actually stored, by using this callback
		if (result != NRF_SUCCESS) {
//			if (op_code == PSTORAGE_LOAD_OP_CODE) {
//				LOGe("Error with loading data");
//			}

			LOGe("Opcode: %d, ERR_CODE: %d (%p)", op_code, result, result);
			APP_ERROR_CHECK(result);
		} else {
			LOGi("Opcode %d executed (no error)", op_code);
			if (op_code == PSTORAGE_UPDATE_OP_CODE) {
				Storage::getInstance().onUpdateDone();
			}
		}
	}

} // extern "C"

// NOTE: DO NOT CHANGE ORDER OF THE ELEMENTS OR THE FLASH
//   STORAGE WILL GET MESSED UP!! NEW ENTRIES ALWAYS AT THE END
static storage_config_t config[] {
	{PS_ID_CONFIGURATION, {}, sizeof(ps_configuration_t)},
	{PS_ID_GENERAL, {}, sizeof(ps_general_vars_t)},
	{PS_ID_STATE, {}, sizeof(ps_state_t)}
};

#define NR_CONFIG_ELEMENTS SIZEOF_ARRAY(config)

Storage::Storage() : EventListener(),
		_initialized(false), _scanning(false), writeBuffer(STORAGE_REQUEST_BUFFER_SIZE), _pending(0)
//		, pendingStorageRequests(0)
{
	LOGd(FMT_CREATE, "Storage");

	EventDispatcher::getInstance().addListener(this);

	memset(buffer, 0, sizeof(buffer));
	writeBuffer.assign((uint8_t*)buffer, sizeof(buffer));

}

void Storage::init() {
	// call once before using any other API calls of the persistent storage module
	BLE_CALL(pstorage_init, ());

	// todo: only create it if needed?
//	requestBuffer = new CircularBuffer<buffer_element_t>(STORAGE_REQUEST_BUFFER_SIZE, false);

	for (int i = 0; i < NR_CONFIG_ELEMENTS; i++) {
		LOGd("Init %i bytes persistent storage (FLASH) for id %d, handle: %p", config[i].storage_size, config[i].id, config[i].handle.block_id);
		initBlocks(config[i].storage_size, 1, config[i].handle);
	}

	_initialized = true;
}

void Storage::onUpdateDone() {
	if (_pending) {
		// track how many update requests are still pending
		_pending--;
		// if meshing is enabled and all update requests were handled by pstorage, start the mesh again
		if (!_pending) {
			LOGi("pstorage update done");
#if BUILD_MESHING == 1
			if (Settings::getInstance().isSet(CONFIG_MESH_ENABLED)) {
				Mesh::getInstance().resume();
			}
#endif
		}
	}
}

void resume_requests(void* p_data, uint16_t len) {
	Storage::getInstance().resumeRequests();
}

void storage_sys_evt_handler(uint32_t evt) {
	switch(evt) {
	case NRF_EVT_RADIO_SESSION_IDLE: {
		// once mesh is stopped, the softdevice will trigger the NRF_EVT_RADIO_SESSION_IDLE,
		// now we can try to update the pstorage
//		LOGd("NRF_EVT_RADIO_SESSION_IDLE");
		uint32_t errorCode = app_sched_event_put(NULL, 0, resume_requests);
		APP_ERROR_CHECK(errorCode);
		break;
	}
	case NRF_EVT_RADIO_SESSION_CLOSED: {
//		LOGd("NRF_EVT_RADIO_SESSION_CLOSED");
//			Storage::getInstance().resumeRequests();
		uint32_t errorCode = app_sched_event_put(NULL, 0, resume_requests);
		APP_ERROR_CHECK(errorCode);
		break;
	}
	}
}

void Storage::resumeRequests() {
	if (!writeBuffer.empty()) {
#ifdef PRINT_STORAGE_VERBOSE
		LOGd("Resume pstorage write requests");
#endif

		while (!writeBuffer.empty()) {
			//! get the next buffered storage request
			buffer_element_t elem = writeBuffer.pop();

//			LOGd("elem:");
//			BLEutil::printArray((uint8_t*)&elem, sizeof(elem));

			if (!_scanning) {
#ifdef PRINT_STORAGE_VERBOSE
				LOGd("pstorage_update");
#endif
				// count number of pending updates to decide when mesh can be resumed (if needed)
				_pending++;
				//! TODO: got an error 4 here when spam toggling relay.
				BLE_CALL (pstorage_update, (&elem.storageHandle, elem.data, elem.dataSize, elem.storageOffset) );
			} else {
				// if scanning was started again, push it back on the buffer and try again during the next
				// scan break
				writeBuffer.push(elem);
//				LOGe("scan started before all pstorage write requests were processed!");
				return;
			}

#ifdef PRINT_STORAGE_VERBOSE
			LOGd("resume done");
#endif
		}
	}
}

void Storage::handleEvent(uint16_t evt, void* p_data, uint16_t length) {

	switch(evt) {
	case EVT_SCAN_STARTED: {
//		LOGd("EVT_SCAN_STARTED");
		_scanning = true;
		break;
	}
	case EVT_SCAN_STOPPED: {
//		LOGd("EVT_SCAN_STOPPED");
		_scanning = false;

		// if there are pstorage update requests buffered
		if (!writeBuffer.empty()) {
			// if meshing, need to stop the mesh first before updating pstorage
			if (Settings::getInstance().isSet(CONFIG_MESH_ENABLED))	{
#if BUILD_MESHING == 1
#ifdef PRINT_STORAGE_VERBOSE
				LOGd("pause mesh on scan stop");
#endif
				Mesh::getInstance().resume();
#endif
			} else {
				// otherwise, resume buffered pstorage update requests
				uint32_t errorCode = app_sched_event_put(NULL, 0, resume_requests);
				APP_ERROR_CHECK(errorCode);
			}
		}
		break;
	}
	}

}

storage_config_t* Storage::getStorageConfig(ps_storage_id storageID) {

	for (int i = 0; i < NR_CONFIG_ELEMENTS; i++) {
		if (config[i].id == storageID) {
			return &config[i];
		}
	}

	return NULL;
}

bool Storage::getHandle(ps_storage_id storageID, pstorage_handle_t& handle) {
	storage_config_t* config;

	if ((config = getStorageConfig(storageID))) {
		handle = config->handle;
		return true;
	} else {
		return false;
	}
}

/**
 * We allocate a single block of size "size". Biggest allocated size is 640 bytes.
 */
void Storage::initBlocks(pstorage_size_t size, pstorage_size_t count, pstorage_handle_t& handle) {
	// set parameter
	pstorage_module_param_t param;
	param.block_size = size;
	param.block_count = 1;
	param.cb = pstorage_callback_handler;

	// register
	BLE_CALL ( pstorage_register, (&param, &handle) );
}

/**
 * Nordic bug: We have to clear the entire block!
 */
void Storage::clearStorage(ps_storage_id storageID) {

	storage_config_t* config;
	if (!(config = getStorageConfig(storageID))) {
		// if no config was found for the given storage ID return
		return;
	}

	pstorage_handle_t block_handle;
	BLE_CALL ( pstorage_block_identifier_get, (&config->handle, 0, &block_handle) );

	BLE_CALL (pstorage_clear, (&block_handle, config->storage_size) );
}

void Storage::readStorage(pstorage_handle_t handle, ps_storage_base_t* item, uint16_t size) {
	pstorage_handle_t block_handle;

	BLE_CALL ( pstorage_block_identifier_get, (&handle, 0, &block_handle) );

	BLE_CALL (pstorage_load, ((uint8_t*)item, &block_handle, size, 0) );

#ifdef PRINT_ITEMS
	_log(SERIAL_INFO, "get struct: \r\n");
	BLEutil::printArray((uint8_t*)item, size);
#endif

}

void Storage::writeStorage(pstorage_handle_t handle, ps_storage_base_t* item, uint16_t size) {
	writeItem(handle, 0, (uint8_t*)item, size);
}

void Storage::readItem(pstorage_handle_t handle, pstorage_size_t offset, uint8_t* item, uint16_t size) {
	pstorage_handle_t block_handle;

	BLE_CALL ( pstorage_block_identifier_get, (&handle, 0, &block_handle) );

	BLE_CALL (pstorage_load, (item, &block_handle, size, offset) );

#ifdef PRINT_ITEMS
	_log(SERIAL_INFO, "read item: \r\n");
	BLEutil::printArray(item, size);
#endif

}

void Storage::writeItem(pstorage_handle_t handle, pstorage_size_t offset, uint8_t* item, uint16_t size) {
	pstorage_handle_t block_handle;

#ifdef PRINT_ITEMS
	_log(SERIAL_INFO, "write item: \r\n");
	BLEutil::printArray((uint8_t*)item, size);
#endif

	BLE_CALL ( pstorage_block_identifier_get, (&handle, 0, &block_handle) );

	// [23.06.16] If the device is scanning or meshing, then pstorage_updates are queued by the softdevice.
	//    unfortunately, they are not automatically resumed once the softdevice has time to process them. they
	//    are staying queued until the first pstorage_update is called while the device is not scanning. to
	//    avoid this, we queue the write calls ourselves, and only trigger the pstorage_update calls once the
	//    device stopped scanning and meshing
#if BUILD_MESHING == 1
	bool meshRunning = Mesh::getInstance().isRunning();
#else
	bool meshRunning = false;
#endif
	// if scanning or meshing, we need to buffer the storage update requests until the softdevice has time to process
	// the pstorage. this is only possible if not scanning and mesh is stopped.
	if (_scanning || meshRunning) {
		LOGd("buffer storage request")
		if (!writeBuffer.full()) {
			buffer_element_t elem;
			elem.storageHandle = block_handle;
			elem.storageOffset = offset;
			elem.dataSize = size;
			elem.data = item;
			writeBuffer.push(elem);
		} else {
			LOGe("storage request buffer is full!");
		}

		// if not scanning, stop the mesh and wait for the NRF_EVT_RADIO_SESSION_IDLE to arrive to access pstorage
		// if scannig, wait for the EVT_SCAN_STOPPED
		if (meshRunning && !_scanning) {
#if BUILD_MESHING == 1
#ifdef PRINT_STORAGE_VERBOSE
			LOGd("pause mesh on pstorage update");
#endif
			Mesh::getInstance().pause();
#endif
		}
	} else {
		// if neither scanning nor meshing, call the update directly
		BLE_CALL (pstorage_update, (&block_handle, item, size, offset) );
	}

}
