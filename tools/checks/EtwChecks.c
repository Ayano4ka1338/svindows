/* Process-local ETW provider lifecycle and captured payloads. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t BYTE;
typedef uint16_t USHORT, WCHAR;
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#include "../../user/w/ntdll/Rtl/Etw.inc"

static ULONG callbacks;
static ULONG classic_callbacks;
static ULONGLONG classic_logger;
static void enabled(const void *guid, ULONG control, BYTE level,
                    ULONGLONG any, ULONGLONG all, const void *filter,
                    void *context)
{
    assert(guid && (control == 0 || control == 1));
    assert(level == 4 && any == 2 && all == 0 && !filter);
    assert(context == &callbacks);
    callbacks++;
}

static ULONG classic_enabled(ULONG code, void *context, ULONG *size,
                             void *buffer)
{
    assert(context == &classic_callbacks && size && *size == 48);
    assert(code == 4 || code == 5);
    classic_logger = EtwGetTraceLoggerHandle(buffer);
    if (code == 4) assert(classic_logger != ~(ULONGLONG)0);
    classic_callbacks++;
    return 0;
}

int main(void)
{
    BYTE guid[16] = {1, 2, 3};
    ULONGLONG handle = 0, replacement = 0;
    ETW_EVENT_DESCRIPTOR descriptor = {.Id = 17, .Level = 3, .Keyword = 2};
    const char payload[] = "hello";
    ETW_DATA_DESCRIPTOR data = {(ULONGLONG)(uintptr_t)payload,
                                sizeof(payload), 0};
    ETW_CAPTURED_EVENT event;
    assert(EtwEventRegister(NULL, enabled, &callbacks, &handle) == 87);
    assert(handle == 0);
    assert(EtwEventRegister(guid, enabled, &callbacks, &handle) == 0);
    assert(handle && !EtwEventEnabled(handle, &descriptor));
    assert(EtwEventWrite(handle, &descriptor, 1, &data) == 0);
    assert(SvEtwReadEvent(&event) == 1168);
    assert(SvEtwEnableProvider(guid, TRUE, 4, 2, 0) == 0);
    assert(callbacks == 1 && EtwEventEnabled(handle, &descriptor));
    assert(EtwEventWrite(handle, &descriptor, 1, &data) == 0);
    assert(SvEtwReadEvent(&event) == 0);
    assert(event.Size == sizeof(payload) && event.Descriptor.Id == 17);
    assert(!memcmp(event.Provider, guid, 16));
    assert(!memcmp(event.Data, payload, sizeof(payload)));
    descriptor.Keyword = 4;
    assert(!EtwEventEnabled(handle, &descriptor));
    assert(EtwEventUnregister(handle) == 0);
    assert(EtwEventUnregister(handle) == ETW_ERROR_INVALID_HANDLE);
    assert(EtwEventRegister(guid, NULL, NULL, &replacement) == 0);
    assert(replacement != handle);
    assert(EtwEventWrite(handle, &descriptor, 0, NULL) ==
           ETW_ERROR_INVALID_HANDLE);
    assert(SvEtwEnableProvider(guid, TRUE, 4, 2, 0) == 0);
    descriptor.Keyword = 2;
    data.Size = ETW_MAX_EVENT_DATA + 1;
    assert(EtwEventWrite(replacement, &descriptor, 1, &data) ==
           ETW_ERROR_MORE_DATA);
    assert(EtwEventUnregister(replacement) == 0);
    ETW_TRACE_GUID_REGISTRATION classes[1] = {{guid, 0}};
    assert(EtwRegisterTraceGuidsW(classic_enabled, &classic_callbacks, guid,
                                  1, classes, NULL, NULL, &handle) == 0);
    assert(classes[0].RegHandle == handle);
    assert(EtwGetTraceEnableLevel(handle) == 0);
    assert(SvEtwEnableProvider(guid, TRUE, 4, 2, 0) == 0);
    assert(classic_callbacks == 1 && classic_logger == handle);
    assert(EtwGetTraceEnableLevel(handle) == 4);
    assert(EtwGetTraceEnableFlags(handle) == 2);
    const char first[] = "one", second[] = "two";
    assert(EtwTraceMessage(handle, ETW_TRACE_MESSAGE_GUID, guid, 49,
                           first, sizeof(first), second, sizeof(second),
                           NULL, (size_t)0) == 0);
    assert(SvEtwReadEvent(&event) == 0);
    assert(event.Descriptor.Id == 49 && event.Size == 16 + 8);
    assert(!memcmp(event.Data, guid, 16));
    assert(!memcmp(event.Data + 16, first, sizeof(first)));
    assert(!memcmp(event.Data + 20, second, sizeof(second)));
    assert(EtwTraceMessage(handle, 0x80000000u, guid, 1,
                           NULL, (size_t)0) == ETW_ERROR_INVALID_PARAMETER);
    assert(EtwTraceMessage(handle, ETW_TRACE_MESSAGE_GUID, guid, 1,
                           first, (size_t)ETW_MAX_EVENT_DATA,
                           NULL, (size_t)0) == ETW_ERROR_MORE_DATA);
    assert(SvEtwEnableProvider(guid, FALSE, 0, 0, 0) == 0);
    assert(classic_callbacks == 2 && EtwGetTraceEnableLevel(handle) == 0);
    assert(EtwUnregisterTraceGuids(handle) == 0);
    assert(EtwTraceMessage(handle, 0, NULL, 1, NULL, (size_t)0) ==
           ETW_ERROR_INVALID_HANDLE);
    struct { ETW_TRACE_PROPERTIES Properties; WCHAR Name[32]; } session = {0};
    session.Properties.Wnode.BufferSize = sizeof(session);
    session.Properties.LogFileMode = 0x400;
    session.Properties.LoggerNameOffset = sizeof(ETW_TRACE_PROPERTIES);
    ULONGLONG trace = 0;
    assert(EtwStartTraceW(&trace, (const WCHAR *)u"ShellTrace",
                          &session.Properties) == 0 && trace);
    assert(EtwStartTraceW(&replacement, (const WCHAR *)u"ShellTrace",
                          &session.Properties) == ETW_ERROR_ALREADY_EXISTS);
    assert(EtwEventRegister(guid, NULL, NULL, &replacement) == 0);
    assert(EtwEnableTraceEx(guid, NULL, trace, 1, 4, 2, 0, 0, NULL) == 0);
    assert(EtwEventEnabled(replacement, &descriptor));
    data.Size = sizeof(payload);
    assert(EtwEventWrite(replacement, &descriptor, 1, &data) == 0);
    assert(SvEtwReadEvent(&event) == 0);
    assert(event.Size == sizeof(payload));
    assert(EtwStopTraceW(trace, NULL, &session.Properties) == 0);
    assert(!EtwEventEnabled(replacement, &descriptor));
    assert(EtwEnableTraceEx(guid, NULL, trace, 1, 4, 2, 0, 0, NULL) ==
           ETW_ERROR_INVALID_HANDLE);
    assert(EtwEventUnregister(replacement) == 0);
    puts("PASS ETW provider handles, filtering, capture, classic messages, callback and bounds");
    return 0;
}
