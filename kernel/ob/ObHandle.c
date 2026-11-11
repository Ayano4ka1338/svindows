/*
 * ob/ObHandle.c - process-local kernel handle tables.
 *
 * A handle is an index into the current process's table of
 * (object, granted-access) pairs. It is encoded as (index + 1) * 4. Before the
 * scheduler exists, bootstrap code uses a dedicated table which later becomes
 * the System process table.
 */
#include <svindows/Ob.h>
#include <svindows/Ex.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>
#include "Obp.h"

#define OB_MAX_HANDLES 4096

typedef struct _HANDLE_ENTRY {
    POBJECT     Object;
    ACCESS_MASK GrantedAccess;
} HANDLE_ENTRY;

struct _OB_HANDLE_TABLE {
    BOOLEAN      ExhaustionReported;
    ULONG        OpenCount;
    HANDLE_ENTRY Entries[OB_MAX_HANDLES];
};

static POB_HANDLE_TABLE g_bootstrap_table;

static ALWAYS_INLINE HANDLE index_to_handle(UINT32 index)
{
    return (HANDLE)(ULONG_PTR)((index + 1) * 4);
}

static BOOLEAN handle_to_index(HANDLE handle, UINT32 *out_index)
{
    ULONG_PTR value = (ULONG_PTR)handle;
    if (!value || (value & 3))
        return FALSE;
    ULONG_PTR index = value / 4 - 1;
    if (index >= OB_MAX_HANDLES)
        return FALSE;
    *out_index = (UINT32)index;
    return TRUE;
}

static POB_HANDLE_TABLE current_handle_table(void)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (thread && thread->Process && thread->Process->HandleTable)
        return thread->Process->HandleTable;
    return g_bootstrap_table;
}

POB_HANDLE_TABLE ObCreateHandleTable(void)
{
    POB_HANDLE_TABLE table = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(OB_HANDLE_TABLE), 'TbHO');
    if (table)
        memset(table, 0, sizeof(*table));
    return table;
}

POB_HANDLE_TABLE ObGetBootstrapHandleTable(void)
{
    return g_bootstrap_table;
}

void ObInitializeHandleTable(void)
{
    g_bootstrap_table = ObCreateHandleTable();
    if (!g_bootstrap_table)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "cannot allocate bootstrap handle table");
    KeLog("[ob]   bootstrap handle table: %u slots\n",
          (unsigned)OB_MAX_HANDLES);
}

ULONG ObCountOpenHandles(POB_HANDLE_TABLE table)
{
    return table ? __atomic_load_n(&table->OpenCount, __ATOMIC_RELAXED) : 0;
}

static void release_handle_entry(POB_HANDLE_TABLE table, HANDLE_ENTRY *entry)
{
    POBJECT object = entry->Object;
    POBJECT_HEADER header = ObHeaderFromObject(object);
    entry->Object = NULL;
    entry->GrantedAccess = 0;
    __atomic_sub_fetch(&table->OpenCount, 1, __ATOMIC_RELAXED);
    LONG handles = InterlockedDecrement(&header->HandleCount);
    if (header->Type->TotalHandles)
        header->Type->TotalHandles--;
    if (handles == 0 && header->Type->UnlinkNameOnLastHandle)
        ObRemoveObjectName(object);
    ObDereferenceObject(object);
}

void ObDestroyHandleTable(POB_HANDLE_TABLE table)
{
    if (!table || table == g_bootstrap_table)
        return;
    for (UINT32 i = 0; i < OB_MAX_HANDLES; i++) {
        if (table->Entries[i].Object)
            release_handle_entry(table, &table->Entries[i]);
    }
    ExFreePool(table);
}

static void report_exhaustion(POB_HANDLE_TABLE table)
{
    if (table->ExhaustionReported)
        return;
    table->ExhaustionReported = TRUE;
    KeLog("[ob]   process handle table exhausted (%u slots); census:\n",
          (unsigned)OB_MAX_HANDLES);
    for (UINT32 i = 0; i < OB_MAX_HANDLES; i++) {
        POBJECT_TYPE type = ObHeaderFromObject(table->Entries[i].Object)->Type;
        BOOLEAN counted = FALSE;
        for (UINT32 j = 0; j < i; j++) {
            if (ObHeaderFromObject(table->Entries[j].Object)->Type == type) {
                counted = TRUE;
                break;
            }
        }
        if (counted)
            continue;
        UINT32 count = 0;
        for (UINT32 j = 0; j < OB_MAX_HANDLES; j++) {
            if (ObHeaderFromObject(table->Entries[j].Object)->Type == type)
                count++;
        }
        KeLog("[ob]     %s: %u handle(s)\n", type->Name, (unsigned)count);
    }
}

NTSTATUS ObCreateHandleInTable(POB_HANDLE_TABLE table, POBJECT object,
                               ACCESS_MASK access, HANDLE *out_handle)
{
    if (!object || !out_handle || !table)
        return STATUS_INVALID_PARAMETER;

    UINT64 flags = KiIrqSave();
    for (UINT32 i = 0; i < OB_MAX_HANDLES; i++) {
        HANDLE_ENTRY *entry = &table->Entries[i];
        if (!entry->Object) {
            POBJECT_HEADER header = ObHeaderFromObject(object);
            entry->Object = object;
            entry->GrantedAccess = access;
            __atomic_add_fetch(&table->OpenCount, 1, __ATOMIC_RELAXED);
            ObReferenceObject(object);
            InterlockedIncrement(&header->HandleCount);
            header->Type->TotalHandles++;
            *out_handle = index_to_handle(i);
            KiIrqRestore(flags);
            return STATUS_SUCCESS;
        }
    }
    report_exhaustion(table);
    KiIrqRestore(flags);
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS ObCreateHandle(POBJECT object, ACCESS_MASK access, HANDLE *out_handle)
{
    return ObCreateHandleInTable(current_handle_table(), object, access,
                                 out_handle);
}

NTSTATUS ObDuplicateHandleToTable(HANDLE source,
                                  POB_HANDLE_TABLE destination,
                                  HANDLE *out_handle)
{
    POBJECT object;
    NTSTATUS status = ObReferenceObjectByHandle(source, 0, NULL, &object);
    if (!NT_SUCCESS(status))
        return status;
    UINT32 index;
    ACCESS_MASK access = GENERIC_ALL;
    POB_HANDLE_TABLE source_table = current_handle_table();
    if (source_table && handle_to_index(source, &index))
        access = source_table->Entries[index].GrantedAccess;
    status = ObCreateHandleInTable(destination, object, access, out_handle);
    ObDereferenceObject(object);
    return status;
}

NTSTATUS ObReferenceObjectByHandle(HANDLE handle, ACCESS_MASK desired,
                                   POBJECT_TYPE type, POBJECT *out_object)
{
    return ObReferenceObjectByHandleInTable(current_handle_table(), handle,
                                             desired, type, out_object, NULL);
}

NTSTATUS ObReferenceObjectByHandleInTable(POB_HANDLE_TABLE table,
                                          HANDLE handle, ACCESS_MASK desired,
                                          POBJECT_TYPE type,
                                          POBJECT *out_object,
                                          ACCESS_MASK *granted_access)
{
    UINT32 index;
    if (!out_object || !table || !handle_to_index(handle, &index))
        return STATUS_INVALID_HANDLE;

    UINT64 flags = KiIrqSave();
    HANDLE_ENTRY *entry = &table->Entries[index];
    if (!entry->Object) {
        KiIrqRestore(flags);
        return STATUS_INVALID_HANDLE;
    }
    POBJECT_HEADER header = ObHeaderFromObject(entry->Object);
    if (type && header->Type != type) {
        KiIrqRestore(flags);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    ACCESS_MASK granted = entry->GrantedAccess;
    if (granted & GENERIC_ALL)
        granted = (ACCESS_MASK)~0u;
    if (desired && (granted & desired) != desired) {
        KiIrqRestore(flags);
        return STATUS_ACCESS_DENIED;
    }
    ObReferenceObject(entry->Object);
    *out_object = entry->Object;
    if (granted_access) *granted_access = entry->GrantedAccess;
    KiIrqRestore(flags);
    return STATUS_SUCCESS;
}

NTSTATUS ObDuplicateHandleBetweenTables(POB_HANDLE_TABLE source_table,
                                         HANDLE source_handle,
                                         POB_HANDLE_TABLE target_table,
                                         ACCESS_MASK desired_access,
                                         BOOLEAN same_access,
                                         BOOLEAN close_source,
                                         HANDLE *out_handle)
{
    if (!target_table || !out_handle) return STATUS_INVALID_PARAMETER;
    POBJECT object;
    ACCESS_MASK source_access;
    NTSTATUS status = ObReferenceObjectByHandleInTable(
        source_table, source_handle, 0, NULL, &object, &source_access);
    if (!NT_SUCCESS(status)) return status;
    ACCESS_MASK granted = source_access & GENERIC_ALL ? ~(ACCESS_MASK)0 :
                                                    source_access;
    ACCESS_MASK target_access = same_access ? source_access : desired_access;
    if ((target_access & granted) != target_access)
        status = STATUS_ACCESS_DENIED;
    else
        status = ObCreateHandleInTable(target_table, object,
                                        target_access, out_handle);
    if (NT_SUCCESS(status) && close_source)
        ObCloseHandleForObject(source_table, source_handle, object);
    ObDereferenceObject(object);
    return status;
}

NTSTATUS ObCloseHandle(HANDLE handle)
{
    return ObCloseHandleForObject(current_handle_table(), handle, NULL);
}

NTSTATUS ObCloseHandleForObject(POB_HANDLE_TABLE table, HANDLE handle,
                                POBJECT expected)
{
    UINT32 index;
    if (!table || !handle_to_index(handle, &index))
        return STATUS_INVALID_HANDLE;

    UINT64 flags = KiIrqSave();
    HANDLE_ENTRY *entry = &table->Entries[index];
    if (!entry->Object || (expected && entry->Object != expected)) {
        KiIrqRestore(flags);
        return STATUS_INVALID_HANDLE;
    }
    release_handle_entry(table, entry);
    KiIrqRestore(flags);
    return STATUS_SUCCESS;
}
