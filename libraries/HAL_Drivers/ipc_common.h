/******************************************************************************
 * Copyright 2020-2026 The RT-Thread Development Team. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#ifndef AI_KIT_IPC_COMMON_H
#define AI_KIT_IPC_COMMON_H

#include <stdint.h>
#include "cy_ipc_pipe.h"
#include "mtb_ipc_config.h"

#define AI_KIT_IPC_MAX_ENDPOINTS          (16UL)
#define AI_KIT_IPC_CLIENT_COUNT           (8UL)
#define AI_KIT_IPC_CM33_ENDPOINT          (9UL)
#define AI_KIT_IPC_CM55_ENDPOINT          (10UL)
#define AI_KIT_IPC_CM33_CLIENT_ID         (3UL)
#define AI_KIT_IPC_CM55_CLIENT_ID         (5UL)
#define AI_KIT_IPC_INTERRUPT_PRIORITY     (6UL)

#define AI_KIT_IPC_PIPE_CHANNEL_MASK \
    (CY_IPC_CH_MASK(AI_KIT_RTTHREAD_IPC_CM33_CHANNEL) | \
     CY_IPC_CH_MASK(AI_KIT_RTTHREAD_IPC_CM55_CHANNEL))

#define AI_KIT_IPC_PROTOCOL_VERSION       (2U)
#define AI_KIT_IPC_FRAME_MAGIC            (0x43504941UL) /* "AIPC" */
#define AI_KIT_IPC_RX_QUEUE_DEPTH         (16U)
#define AI_KIT_IPC_SEND_RETRY_COUNT       (20U)
#define AI_KIT_IPC_SEND_RETRY_DELAY_MS    (1U)
#define AI_KIT_IPC_TX_TIMEOUT_MS          (1000U)
#define AI_KIT_IPC_M33_SHARED_START       (0x240FE000UL)
#define AI_KIT_IPC_M33_SHARED_SIZE        (0x00001000UL)

typedef enum
{
    AI_KIT_IPC_KIND_REQUEST = 1,
    AI_KIT_IPC_KIND_RESPONSE = 2
} ai_kit_ipc_kind_t;

typedef enum
{
    AI_KIT_IPC_CORE_M33 = 33,
    AI_KIT_IPC_CORE_M55 = 55
} ai_kit_ipc_core_t;

typedef enum
{
    AI_KIT_IPC_COMMAND_PING = 1,
    AI_KIT_IPC_COMMAND_BENCHMARK_PUBLISH = 2,
    AI_KIT_IPC_COMMAND_BENCHMARK_START = 3
} ai_kit_ipc_command_t;

/*
 * The first word follows the PDL Pipe RRRRDDCC transport layout:
 * client_id, user byte, and the 16-bit release interrupt mask. The frame is
 * exactly one 32-byte cache line so full-frame cache maintenance is safe.
 */
typedef struct
{
    uint8_t client_id;
    uint8_t kind;
    uint16_t release_mask;
    uint32_t magic;
    uint16_t protocol_version;
    uint8_t source;
    uint8_t destination;
    uint32_t sequence;
    uint32_t command;
    uint32_t argument0;
    uint32_t argument1;
    uint32_t checksum;
} ai_kit_ipc_frame_t;

typedef char ai_kit_ipc_frame_size_must_be_32_bytes[
    (sizeof(ai_kit_ipc_frame_t) == 32U) ? 1 : -1];

static inline uint32_t ai_kit_ipc_frame_checksum(
    const ai_kit_ipc_frame_t *frame)
{
    uint32_t checksum = AI_KIT_IPC_FRAME_MAGIC;

    checksum ^= frame->kind;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= frame->magic;
    checksum ^= ((uint32_t)frame->protocol_version << 16);
    checksum ^= ((uint32_t)frame->source << 8) | frame->destination;
    checksum = (checksum << 5) | (checksum >> 27);
    checksum ^= frame->sequence;
    checksum ^= frame->command;
    checksum ^= frame->argument0;
    checksum ^= frame->argument1;
    return checksum;
}

static inline void ai_kit_ipc_frame_prepare(
    ai_kit_ipc_frame_t *frame,
    ai_kit_ipc_kind_t kind,
    ai_kit_ipc_core_t source,
    ai_kit_ipc_core_t destination,
    uint32_t sequence,
    ai_kit_ipc_command_t command,
    uint32_t argument0,
    uint32_t argument1)
{
    frame->client_id = 0U;
    frame->kind = (uint8_t)kind;
    frame->release_mask = 0U;
    frame->magic = AI_KIT_IPC_FRAME_MAGIC;
    frame->protocol_version = AI_KIT_IPC_PROTOCOL_VERSION;
    frame->source = (uint8_t)source;
    frame->destination = (uint8_t)destination;
    frame->sequence = sequence;
    frame->command = (uint32_t)command;
    frame->argument0 = argument0;
    frame->argument1 = argument1;
    frame->checksum = ai_kit_ipc_frame_checksum(frame);
}

#endif
