// Copyright (c) Microsoft Corporation
// SPDX-License-Identifier: MIT

// This file contains eBPF definitions common to eBPF programs, core execution engine
// as well as eBPF API library.

#pragma once

#include <stdint.h>
#include "../external/ebpf-verifier/src/ebpf_base.h"

typedef enum _ebpf_map_type
{
    EBPF_MAP_TYPE_UNSPECIFIED = 0, ///< Unspecified map type.
    EBPF_MAP_TYPE_HASH = 1,        ///< Hash table.
    EBPF_MAP_TYPE_ARRAY = 2,       ///< Array, wehere the map key is the array index.
} ebpf_map_type_t;

/**
 * @brief eBPF Map Definition
 */
typedef struct _ebpf_map_definition
{
    uint32_t size;
    ebpf_map_type_t type;
    uint32_t key_size;
    uint32_t value_size;
    uint32_t max_entries;
} ebpf_map_definition_t;
