/** General information not depending on Nordic libraries
 *
 * This file contains general fields, structs, etc. that do have very few dependencies.
 * 
 *   - This means that they can be used by unit tests without pulling in hardware specific details
 */

#pragma once

#include <stdint.h>

#if (defined HOST_TARGET)
// See rbc_mesh.h and src/radio_control.h, here it's just to set something more or less random
#define RADIO_PCNF1_MAXLEN             118
#define RBC_MESH_VALUE_MAX_LEN         (RADIO_PCNF1_MAXLEN - 14)
#else
#include <rbc_mesh.h>
#endif

typedef uint16_t id_type_t;

#define ENCRYPTED_HEADER_SIZE (sizeof(uint32_t) + sizeof(uint32_t))
#define MAX_ENCRYPTED_PAYLOAD_LENGTH ((RBC_MESH_VALUE_MAX_LEN - ENCRYPTED_HEADER_SIZE) - ((RBC_MESH_VALUE_MAX_LEN - ENCRYPTED_HEADER_SIZE) % 16))

#define PAYLOAD_HEADER_SIZE (sizeof(uint32_t))
#define MAX_MESH_MESSAGE_LENGTH (MAX_ENCRYPTED_PAYLOAD_LENGTH - PAYLOAD_HEADER_SIZE)

