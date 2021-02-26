/*
 *  Copyright (c) Microsoft Corporation
 *  SPDX-License-Identifier: MIT
*/

#include <wdm.h>
#include <ntdef.h>
#include <netiodef.h>
#include <ntintsafe.h>

#include "types.h"
#include "protocol.h"

#include "ebpf_core.h"
#include "types.h"
#include "ebpf_maps.h"

#define RTL_COUNT_OF(arr) (sizeof(arr) / sizeof(arr[0]))

static DWORD _count_of_seh_raised = 0;

_Requires_lock_held_(_ebpf_core_code_entry_list_lock)
static LIST_ENTRY _ebpf_core_code_entry_list;
static KSPIN_LOCK _ebpf_core_code_entry_list_lock;

_Requires_lock_held_(_ebpf_core_map_entry_list_lock)
static LIST_ENTRY _ebpf_core_map_entry_list;
static KSPIN_LOCK _ebpf_core_map_entry_list_lock;

// TODO: Switch this to use real object manager handles
static UINT64 _next_pseudo_handle = 0;


typedef struct _ebpf_core_code_entry {
    LIST_ENTRY entry;

    // pointer to code buffer
    uint8_t* code;

    // handle returned to user mode application
    uint64_t handle;

    ebpf_hook_point_t hook_point;
} ebpf_core_code_entry_t;


static void* _ebpf_core_map_lookup_element(ebpf_core_map_t* map, const uint8_t* key);
static void _ebpf_core_map_update_element(ebpf_core_map_t* map, const uint8_t* key, const uint8_t* data);
static void _ebpf_core_map_delete_element(ebpf_core_map_t* map, const uint8_t* key);

static const void * _ebpf_program_helpers[] =
{
    NULL,
    (void*)&_ebpf_core_map_lookup_element,
    (void*)&_ebpf_core_map_update_element,
    (void*)&_ebpf_core_map_delete_element
};

_Requires_exclusive_lock_held_(_ebpf_core_map_entry_list_lock)
static ebpf_core_map_entry_t* _ebpf_core_find_map_entry(uint64_t handle)
{
    // TODO: Switch this to use real object manager handles
    LIST_ENTRY* list_entry = _ebpf_core_map_entry_list.Flink;
    while (list_entry != &_ebpf_core_map_entry_list)
    {
        ebpf_core_map_entry_t* map = CONTAINING_RECORD(list_entry, ebpf_core_map_entry_t, entry);
        if (handle == map->handle)
            return map;

        list_entry = list_entry->Flink;
    }
    return NULL;
}

_Requires_exclusive_lock_held_(_ebpf_core_code_entry_list_lock)
ebpf_core_code_entry_t* _ebpf_core_find_user_code(uint64_t handle)
{
    // TODO: Switch this to use real object manager handles
    LIST_ENTRY* list_entry = _ebpf_core_code_entry_list.Flink;
    while (list_entry != &_ebpf_core_code_entry_list)
    {
        ebpf_core_code_entry_t* code = CONTAINING_RECORD(list_entry, ebpf_core_code_entry_t, entry);
        if (handle == code->handle)
            return code;

        list_entry = list_entry->Flink;
    }
    return NULL;
}

NTSTATUS
ebpf_core_initialize()
{
    
    KeInitializeSpinLock(&_ebpf_core_code_entry_list_lock);
    InitializeListHead(&_ebpf_core_code_entry_list);

    KeInitializeSpinLock(&_ebpf_core_map_entry_list_lock);
    InitializeListHead(&_ebpf_core_map_entry_list);

    return STATUS_SUCCESS;
}

void
ebpf_core_terminate()
{
    KIRQL old_irql;

    KeAcquireSpinLock(&_ebpf_core_code_entry_list_lock, &old_irql);
    LIST_ENTRY* list_entry = _ebpf_core_code_entry_list.Flink;
    while (list_entry != &_ebpf_core_code_entry_list)
    {
        ebpf_core_code_entry_t* code = CONTAINING_RECORD(list_entry, ebpf_core_code_entry_t, entry);
        list_entry = list_entry->Flink;
        RemoveEntryList(&code->entry);
        ExFreePool(code);
    }
    KeReleaseSpinLock(&_ebpf_core_code_entry_list_lock, old_irql);

    KeAcquireSpinLock(&_ebpf_core_map_entry_list_lock, &old_irql);
    list_entry = _ebpf_core_map_entry_list.Flink;
    while (list_entry != &_ebpf_core_map_entry_list)
    {
        ebpf_core_map_entry_t* map = CONTAINING_RECORD(list_entry, ebpf_core_map_entry_t, entry);
        list_entry = list_entry->Flink;
        RemoveEntryList(&map->entry);
        ExFreePool(map);
    }
    KeReleaseSpinLock(&_ebpf_core_map_entry_list_lock, old_irql);

}

NTSTATUS
ebpf_core_protocol_attach_code(
    _In_ const struct _ebpf_operation_attach_detach_request* request,
    _Inout_ void* reply
)
{
    NTSTATUS status = STATUS_INVALID_HANDLE;
    KIRQL old_irql;
    ebpf_core_code_entry_t* code = NULL;
    UNREFERENCED_PARAMETER(reply);

    switch (request->hook)
    {
    case EBPF_HOOK_XDP:
    case EBPF_HOOK_BIND:
        break;
    default:
        status = STATUS_NOT_SUPPORTED;
        goto Done;
    }

    // TODO: Switch this to use real object manager handles
    KeAcquireSpinLock(&_ebpf_core_code_entry_list_lock, &old_irql);
    code = _ebpf_core_find_user_code(request->handle);
    if (code)
    {
        code->hook_point = request->hook;
        status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&_ebpf_core_code_entry_list_lock, old_irql);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: AttachCodeToHook 0x%llx handle\n", request->handle));

Done:
    return status;
}

NTSTATUS
ebpf_core_protocol_detach_code(
    _In_ const struct _ebpf_operation_attach_detach_request* request,
    _Inout_ void* reply
)
{
    NTSTATUS status = STATUS_INVALID_HANDLE;
    KIRQL old_irql;
    ebpf_core_code_entry_t* code = NULL;
    UNREFERENCED_PARAMETER(reply);

    switch (request->hook)
    {
    case EBPF_HOOK_XDP:
    case EBPF_HOOK_BIND:
        break;
    default:
        status = STATUS_NOT_SUPPORTED;
        goto Done;
    }

    // TODO: Switch this to use real object manager handles
    KeAcquireSpinLock(&_ebpf_core_code_entry_list_lock, &old_irql);
    code = _ebpf_core_find_user_code(request->handle);
    if (code)
    {
        code->hook_point = EBPF_HOOK_NONE;
        status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&_ebpf_core_code_entry_list_lock, old_irql);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: DetachCodeFromHook 0x%llx handle\n", request->handle));

Done:
    return status;
}

NTSTATUS
ebpf_core_protocol_unload_code(
    _In_ const struct _ebpf_operation_unload_code_request* request,
    _Inout_ void* reply)
{
    NTSTATUS status = STATUS_INVALID_HANDLE;
    KIRQL old_irql;
    ebpf_core_code_entry_t* code = NULL;
    UNREFERENCED_PARAMETER(reply);

    KeAcquireSpinLock(&_ebpf_core_code_entry_list_lock, &old_irql);
    // TODO: Switch this to use real object manager handles
    code = _ebpf_core_find_user_code(request->handle);
    if (code)
    {
        RemoveEntryList(&code->entry);
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: UnloadCode: 0x%llp handle: 0x%llx\n", code, code->handle));
        ExFreePool(code);
        status = STATUS_SUCCESS;
    }

    KeReleaseSpinLock(&_ebpf_core_code_entry_list_lock, old_irql);
    if (status != STATUS_SUCCESS)
    {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "EbpfCore: UnloadCode: failed to find handle 0x%llx\n", request->handle));
    }
    return status;
}

NTSTATUS
ebpf_core_protocol_load_code(
    _In_ const struct _ebpf_operation_load_code_request* request,
    _Inout_ struct _ebpf_operation_load_code_reply* reply)
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID  buffer = NULL;
    UINT16 codeSize = 0;
    KIRQL old_irql;
    ebpf_core_code_entry_t* code = NULL;

    // allocate
    codeSize = request->header.length;
    buffer = ExAllocatePool2(
        POOL_FLAG_NON_PAGED_EXECUTE,
        codeSize + sizeof(ebpf_core_code_entry_t),
        ebpfPoolTag
    );
    if (buffer == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    // copy and hang on to user code
    code = buffer;
    buffer = (uint8_t*)buffer + sizeof(ebpf_core_code_entry_t);
    RtlCopyMemory(buffer, (PUCHAR)request->machine_code, codeSize);
    code->code = buffer;

    // TODO: Switch this to use real object manager handles
    code->handle = (0xffff | _next_pseudo_handle++);

    KeAcquireSpinLock(&_ebpf_core_code_entry_list_lock, &old_irql);
    InsertTailList(&_ebpf_core_code_entry_list, &code->entry);
    KeReleaseSpinLock(&_ebpf_core_code_entry_list_lock, old_irql);

    // construct the response
    reply->handle = code->handle;
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: AllocateAndLoadCode code: 0x%llp handle: 0x%llx\n", code, code->handle));

Done:
    if (!NT_SUCCESS(status))
    {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "EbpfCore: AllocateAndLoadCode code failed %d\n", status));
    }
    return status;
}

NTSTATUS ebpf_core_protocol_resolve_helper(
    _In_ const struct _ebpf_operation_resolve_helper_request* request,
    _Out_ struct _ebpf_operation_resolve_helper_reply* reply)
{
    if (request->helper_id[0] >= EBPF_INVALID)
    {
        return STATUS_INVALID_PARAMETER;
    }
    reply->address[0] = (uint64_t)_ebpf_program_helpers[request->helper_id[0]];

    return STATUS_SUCCESS;;
}

NTSTATUS ebpf_core_protocol_resolve_map(
    _In_ const struct _ebpf_operation_resolve_map_request* request,
    _Out_ struct _ebpf_operation_resolve_map_reply* reply)
{
    NTSTATUS status = STATUS_NOT_FOUND;
    KIRQL old_irql;
    ebpf_core_map_entry_t* map = NULL;

    KeAcquireSpinLock(&_ebpf_core_map_entry_list_lock, &old_irql);
    // TODO: Switch this to use real object manager handles
    map = _ebpf_core_find_map_entry(request->map_handle[0]);
    if (map)
    {
        status = STATUS_SUCCESS;
        reply->address[0] = (uint64_t)&map->map;
    }
    KeReleaseSpinLock(&_ebpf_core_map_entry_list_lock, old_irql);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: ebpf_core_protocol_resolve_map 0x%llx handle\n", request->map_handle[0]));

    return status;
}

xdp_action_t
ebpf_core_invoke_xdp_hook(
    _In_ void* buffer,
    _In_ uint32_t buffer_length)
{
    KIRQL old_irql;
    ebpf_core_code_entry_t* code = NULL;
    xdp_hook_function function_pointer;
    xdp_action_t result = XDP_PASS;
    BOOLEAN found = FALSE;

    xdp_md_t ctx = { 0 };
    ctx.data = (UINT64)buffer;
    ctx.data_end = ctx.data + buffer_length;

    KeAcquireSpinLock(&_ebpf_core_code_entry_list_lock, &old_irql);

    // TODO: Switch this to use real object manager handles
    LIST_ENTRY* list_entry = _ebpf_core_code_entry_list.Flink;
    while (list_entry != &_ebpf_core_code_entry_list)
    {
        code = CONTAINING_RECORD(list_entry, ebpf_core_code_entry_t, entry);
        if (code->hook_point == EBPF_HOOK_XDP)
        {
            // find the first one and run.
            found = TRUE;
            break;
        }

        list_entry = list_entry->Flink;
    }

    if (found)
    {
        function_pointer = (xdp_hook_function)code->code;
        __try {
            result = (*function_pointer)(&ctx);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            _count_of_seh_raised++;
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: ExecuteCode. _count_of_seh_raised %d\n", _count_of_seh_raised));
        }
    }

    KeReleaseSpinLock(&_ebpf_core_code_entry_list_lock, old_irql);

    return (xdp_action_t)result;
}

bind_action_t
ebpf_core_invoke_bind_hook(
    _In_ struct sockaddr* sockaddr,
    _In_ uint32_t sockaddr_length,
    _In_ uint8_t* app_id,
    _In_ uint32_t app_id_length,
    _In_ uint64_t process_id,
    _In_ bind_operation_t operation,
    _In_ uint8_t protocol)
{
    KIRQL old_irql;
    ebpf_core_code_entry_t* code = NULL;
    bind_hook_function function_pointer;
    bind_action_t result = BIND_PERMIT;
    BOOLEAN found = FALSE;

    bind_md_t ctx = {
        (uint64_t)sockaddr,
        (uint64_t)sockaddr + sockaddr_length,
        (uint64_t)app_id,
        (uint64_t)app_id + app_id_length,
        process_id,
        operation,
        protocol };


    KeAcquireSpinLock(&_ebpf_core_code_entry_list_lock, &old_irql);

    // TODO: Switch this to use real object manager handles
    LIST_ENTRY* list_entry = _ebpf_core_code_entry_list.Flink;
    while (list_entry != &_ebpf_core_code_entry_list)
    {
        code = CONTAINING_RECORD(list_entry, ebpf_core_code_entry_t, entry);
        if (code->hook_point == EBPF_HOOK_BIND)
        {
            // find the first one and run.
            found = TRUE;
            break;
        }

        list_entry = list_entry->Flink;
    }

    if (found)
    {
        function_pointer = (bind_hook_function)code->code;
        __try {
            result = (*function_pointer)(&ctx);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            _count_of_seh_raised++;
        }
    }

    KeReleaseSpinLock(&_ebpf_core_code_entry_list_lock, old_irql);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: ExecuteCode. _count_of_seh_raised %d\n", _count_of_seh_raised));

    return (bind_action_t)result;
}

NTSTATUS ebpf_core_protocol_create_map(
    _In_ const struct _ebpf_operation_create_map_request* request,
    _Inout_ struct _ebpf_operation_create_map_reply* reply)
{
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL old_irql;
    ebpf_core_map_entry_t* map = NULL;
    UINT32 type = request->ebpf_map_definition.type;

    if (type >= RTL_COUNT_OF(ebpf_map_function_tables))
    {
        status = STATUS_NOT_IMPLEMENTED;
        goto Done;
    }

    if (ebpf_map_function_tables[type].create_map == NULL)
    {
        status = STATUS_NOT_IMPLEMENTED;
        goto Done;
    }

    map = ebpf_map_function_tables[type].create_map(&request->ebpf_map_definition);
    if (map == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    // TODO: Switch this to use real object manager handles
    map->handle = (0xffff | _next_pseudo_handle++);

    KeAcquireSpinLock(&_ebpf_core_map_entry_list_lock, &old_irql);
    InsertTailList(&_ebpf_core_map_entry_list, &map->entry);
    KeReleaseSpinLock(&_ebpf_core_map_entry_list_lock, old_irql);

    // construct the response
    reply->handle = map->handle;
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: ebpf_core_protocol_create_map map: 0x%llp handle: 0x%llx\n", map, map->handle));

Done:
    if (!NT_SUCCESS(status))
    {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "EbpfCore: ebpf_core_protocol_create_map code failed %d\n", status));
    }
    return status;
}

NTSTATUS ebpf_core_protocol_map_lookup_element(
    _In_ const struct _ebpf_operation_map_lookup_element_request* request,
    _Inout_ struct _ebpf_operation_map_lookup_element_reply* reply)
{
    NTSTATUS status = STATUS_NOT_FOUND;
    KIRQL old_irql;
    ebpf_core_map_entry_t* map = NULL;

    KeAcquireSpinLock(&_ebpf_core_map_entry_list_lock, &old_irql);
    // TODO: Switch this to use real object manager handles
    map = _ebpf_core_find_map_entry(request->handle);
    if (map)
    {
        UINT32 type = map->map.ebpf_map_definition.type;
        void* value = NULL;

        if ((reply->header.length - sizeof(struct _ebpf_operation_map_lookup_element_reply) + 1) != (map->map.ebpf_map_definition.value_size))
        {
            status = STATUS_INVALID_PARAMETER;
        }
        else if ((value = ebpf_map_function_tables[type].lookup_entry(&map->map, request->key)) != NULL)
        {
            memcpy(reply->value, value, map->map.ebpf_map_definition.value_size);
            status = STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&_ebpf_core_map_entry_list_lock, old_irql);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: ebpf_core_protocol_map_lookup_element 0x%llx handle\n", request->handle));

    return status;
}

NTSTATUS ebpf_core_protocol_map_update_element(
    _In_ const struct _ebpf_operation_map_update_element_request* request,
    _Inout_ void* reply)
{
    NTSTATUS status = STATUS_NOT_FOUND;
    KIRQL old_irql;
    ebpf_core_map_entry_t* map = NULL;
    const uint8_t* key;
    const uint8_t* value;

    UNREFERENCED_PARAMETER(reply);

    KeAcquireSpinLock(&_ebpf_core_map_entry_list_lock, &old_irql);
    // TODO: Switch this to use real object manager handles
    map = _ebpf_core_find_map_entry(request->handle);
    if (map)
    {
        UINT32 type = map->map.ebpf_map_definition.type;

        // Is the request big enough to contain both key + value?
        status = (request->header.length - sizeof(struct _ebpf_operation_map_update_element_request) + 1) == ((size_t)map->map.ebpf_map_definition.key_size + (size_t)map->map.ebpf_map_definition.value_size) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;

        // If success, then extract key
        key = (status == STATUS_SUCCESS) ? request->data : NULL;
        
        // If success, then extract value
        value = (status == STATUS_SUCCESS) ? request->data + map->map.ebpf_map_definition.key_size : NULL;
        
        // If success, then update map
        status = (status == STATUS_SUCCESS) ? ebpf_map_function_tables[type].update_entry(&map->map, key, value) : status;
    }
    KeReleaseSpinLock(&_ebpf_core_map_entry_list_lock, old_irql);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: ebpf_core_protocol_map_lookup_element 0x%llx handle\n", request->handle));

    return status;
}

NTSTATUS ebpf_core_protocol_map_delete_element(
    _In_ const struct _ebpf_operation_map_delete_element_request* request,
    _Inout_ void* reply)
{
    NTSTATUS status = STATUS_NOT_FOUND;
    KIRQL old_irql;
    ebpf_core_map_entry_t* map = NULL;
    UNREFERENCED_PARAMETER(reply);

    KeAcquireSpinLock(&_ebpf_core_map_entry_list_lock, &old_irql);
    // TODO: Switch this to use real object manager handles
    map = _ebpf_core_find_map_entry(request->handle);
    if (map)
    {
        UINT32 type = map->map.ebpf_map_definition.type;

        // Is the request big enough to contain key?
        status = (request->header.length - sizeof(struct _ebpf_operation_map_update_element_request) + 1) == ((size_t)map->map.ebpf_map_definition.key_size) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;

        // If success, then update map
        status = (status == STATUS_SUCCESS) ? ebpf_map_function_tables[type].delete_entry(&map->map, request->key) : status;
    }
    KeReleaseSpinLock(&_ebpf_core_map_entry_list_lock, old_irql);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "EbpfCore: ebpf_core_protocol_map_lookup_element 0x%llx handle\n", request->handle));

    return status;
}

void* _ebpf_core_map_lookup_element(ebpf_core_map_t* map, const uint8_t* key)
{
    UINT32 type = map->ebpf_map_definition.type;
    return ebpf_map_function_tables[type].lookup_entry(map, key);
}

void _ebpf_core_map_update_element(ebpf_core_map_t* map, const uint8_t* key, const uint8_t* value)
{
    UINT32 type = map->ebpf_map_definition.type;
    ebpf_map_function_tables[type].update_entry(map, key, value);
}

void _ebpf_core_map_delete_element(ebpf_core_map_t* map, const uint8_t* key)
{
    UINT32 type = map->ebpf_map_definition.type;
    ebpf_map_function_tables[type].delete_entry(map, key);
}