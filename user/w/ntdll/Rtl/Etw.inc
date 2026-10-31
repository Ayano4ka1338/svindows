#include <stdarg.h>
#include <stddef.h>

/* Process-scoped ETW provider registry and bounded diagnostic consumer.
 * The private SvEtw* consumer can enable a provider and read its events.
 * Without a consumer, EventWrite is a successful filtered-out write, as ETW
 * specifies. Cross-process sessions are not supported. */
#define ETW_MAX_PROVIDERS 64
#define ETW_MAX_EVENTS 64
#define ETW_MAX_EVENT_DATA 512
#define ETW_ERROR_INVALID_HANDLE 6u
#define ETW_ERROR_NOT_ENOUGH_MEMORY 8u
#define ETW_ERROR_INVALID_PARAMETER 87u
#define ETW_ERROR_NOT_SUPPORTED 50u
#define ETW_ERROR_MORE_DATA 234u
#define ETW_ERROR_ALREADY_EXISTS 183u
#define ETW_ERROR_INSTANCE_NOT_FOUND 4201u
#define ETW_TRACE_MESSAGE_GUID 0x00000002u
#define ETW_TRACE_MESSAGE_COMPONENTID 0x00000004u

typedef struct _ETW_EVENT_DESCRIPTOR {
    USHORT Id;
    BYTE Version, Channel, Level, Opcode;
    USHORT Task;
    ULONGLONG Keyword;
} ETW_EVENT_DESCRIPTOR;
typedef struct _ETW_DATA_DESCRIPTOR {
    ULONGLONG Ptr;
    ULONG Size;
    ULONG Reserved;
} ETW_DATA_DESCRIPTOR;
typedef void (*ETW_ENABLE_CALLBACK)(const void *, ULONG, BYTE, ULONGLONG,
                                     ULONGLONG, const void *, void *);
typedef ULONG (*ETW_CLASSIC_CALLBACK)(ULONG, void *, ULONG *, void *);
typedef struct _ETW_WNODE_HEADER {
    ULONG BufferSize, ProviderId;
    ULONGLONG HistoricalContext, TimeStamp;
    BYTE Guid[16];
    ULONG ClientContext, Flags;
} ETW_WNODE_HEADER;
typedef struct _ETW_PROVIDER {
    BYTE Guid[16];
    ETW_ENABLE_CALLBACK Callback;
    ETW_CLASSIC_CALLBACK ClassicCallback;
    void *Context;
    ULONGLONG MatchAny, MatchAll;
    ULONG Generation;
    BYTE Level;
    BYTE Active, Enabled, Classic;
} ETW_PROVIDER;
typedef struct _ETW_CAPTURED_EVENT {
    BYTE Provider[16];
    BYTE Activity[16];
    BYTE RelatedActivity[16];
    ETW_EVENT_DESCRIPTOR Descriptor;
    ULONG Size;
    BYTE Data[ETW_MAX_EVENT_DATA];
} ETW_CAPTURED_EVENT;

static ETW_PROVIDER g_etw_providers[ETW_MAX_PROVIDERS];
static ETW_CAPTURED_EVENT g_etw_events[ETW_MAX_EVENTS];
static ULONG g_etw_event_head, g_etw_event_count;
static volatile ULONG g_etw_lock;
typedef struct _ETW_TRACE_PROPERTIES {
    ETW_WNODE_HEADER Wnode;
    ULONG BufferSize, MinimumBuffers, MaximumBuffers, MaximumFileSize;
    ULONG LogFileMode, FlushTimer, EnableFlags, AgeLimit;
    ULONG NumberOfBuffers, FreeBuffers, EventsLost, BuffersWritten;
    ULONG LogBuffersLost, RealTimeBuffersLost;
    void *LoggerThreadId;
    ULONG LogFileNameOffset, LoggerNameOffset;
} ETW_TRACE_PROPERTIES;
typedef struct _ETW_SESSION {
    WCHAR Name[128];
    ULONGLONG Handle;
    BYTE Guid[16];
    BYTE EnabledGuids[16][16];
    ULONG EnabledCount;
    ULONG Generation;
    BYTE Active;
} ETW_SESSION;
static ETW_SESSION g_etw_session;
ULONG SvEtwEnableProvider(const void *guid, BOOLEAN enabled, BYTE level,
                          ULONGLONG match_any, ULONGLONG match_all);

static void etw_lock(void)
{
    while (__atomic_exchange_n(&g_etw_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static void etw_unlock(void)
{ __atomic_store_n(&g_etw_lock, 0, __ATOMIC_RELEASE); }

static BOOLEAN etw_same_name(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        WCHAR x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return FALSE;
        if (!x) return TRUE;
    }
}

ULONG EtwStartTraceW(ULONGLONG *out, const WCHAR *name,
                     ETW_TRACE_PROPERTIES *properties)
{
    if (!out || !name || !*name || !properties ||
        properties->Wnode.BufferSize < sizeof(*properties))
        return ETW_ERROR_INVALID_PARAMETER;
    ULONG length = 0;
    while (name[length] && length < 127) length++;
    if (!length || name[length]) return ETW_ERROR_INVALID_PARAMETER;
    /* The captured ring is an in-memory circular session; file and realtime
     * delivery modes require independent sinks and are rejected. */
    if (properties->LogFileMode != 0x00000400u ||
        properties->LogFileNameOffset)
        return ETW_ERROR_NOT_SUPPORTED;
    etw_lock();
    if (g_etw_session.Active) {
        ULONG result = etw_same_name(g_etw_session.Name, name) ?
                       ETW_ERROR_ALREADY_EXISTS : ETW_ERROR_NOT_ENOUGH_MEMORY;
        etw_unlock();
        return result;
    }
    if (!++g_etw_session.Generation) ++g_etw_session.Generation;
    for (ULONG i = 0; i <= length; i++) g_etw_session.Name[i] = name[i];
    memcpy(g_etw_session.Guid, properties->Wnode.Guid, 16);
    g_etw_session.EnabledCount = 0;
    g_etw_session.Handle = ((ULONGLONG)g_etw_session.Generation << 32) | 0x4554;
    g_etw_session.Active = TRUE;
    *out = g_etw_session.Handle;
    if (properties->LoggerNameOffset >= sizeof(*properties) &&
        properties->LoggerNameOffset + (length + 1) * 2 <=
        properties->Wnode.BufferSize) {
        WCHAR *target = (WCHAR *)((BYTE *)properties +
                                   properties->LoggerNameOffset);
        for (ULONG i = 0; i <= length; i++) target[i] = name[i];
    }
    etw_unlock();
    return 0;
}

ULONG EtwEnableTraceEx(const void *provider, const void *source,
                        ULONGLONG session, ULONG enabled, BYTE level,
                        ULONGLONG match_any, ULONGLONG match_all,
                        ULONG property, const void *filter)
{
    (void)source;
    if (!provider || enabled > 1) return ETW_ERROR_INVALID_PARAMETER;
    if (property || filter) return ETW_ERROR_NOT_SUPPORTED;
    etw_lock();
    if (!g_etw_session.Active || session != g_etw_session.Handle) {
        etw_unlock(); return ETW_ERROR_INVALID_HANDLE;
    }
    if (enabled && g_etw_session.EnabledCount == 16) {
        BOOLEAN present = FALSE;
        for (ULONG i = 0; i < 16; i++)
            if (!memcmp(g_etw_session.EnabledGuids[i], provider, 16))
                present = TRUE;
        if (!present) { etw_unlock(); return ETW_ERROR_NOT_ENOUGH_MEMORY; }
    }
    etw_unlock();
    ULONG result = SvEtwEnableProvider(provider, (BOOLEAN)enabled, level,
                                       match_any, match_all);
    if (result) return result;
    etw_lock();
    if (g_etw_session.Active && session == g_etw_session.Handle) {
        ULONG i;
        for (i = 0; i < g_etw_session.EnabledCount; i++)
            if (!memcmp(g_etw_session.EnabledGuids[i], provider, 16)) break;
        if (enabled && i == g_etw_session.EnabledCount && i < 16) {
            memcpy(g_etw_session.EnabledGuids[i], provider, 16);
            g_etw_session.EnabledCount++;
        } else if (!enabled && i < g_etw_session.EnabledCount) {
            g_etw_session.EnabledCount--;
            memcpy(g_etw_session.EnabledGuids[i],
                   g_etw_session.EnabledGuids[g_etw_session.EnabledCount], 16);
        }
    }
    etw_unlock();
    return 0;
}

ULONG EtwStopTraceW(ULONGLONG session, const WCHAR *name,
                    ETW_TRACE_PROPERTIES *properties)
{
    if (!properties || properties->Wnode.BufferSize < sizeof(*properties))
        return ETW_ERROR_INVALID_PARAMETER;
    BYTE guids[16][16];
    ULONG count;
    etw_lock();
    if (!g_etw_session.Active ||
        (session ? session != g_etw_session.Handle :
                   !name || !etw_same_name(name, g_etw_session.Name))) {
        etw_unlock(); return ETW_ERROR_INSTANCE_NOT_FOUND;
    }
    count = g_etw_session.EnabledCount;
    for (ULONG i = 0; i < count; i++)
        memcpy(guids[i], g_etw_session.EnabledGuids[i], 16);
    memcpy(properties->Wnode.Guid, g_etw_session.Guid, 16);
    properties->LogFileMode = 0x00000400u;
    properties->EventsLost = 0;
    g_etw_session.Active = FALSE;
    g_etw_session.EnabledCount = 0;
    etw_unlock();
    for (ULONG i = 0; i < count; i++)
        SvEtwEnableProvider(guids[i], FALSE, 0, 0, 0);
    return 0;
}

static ETW_PROVIDER *etw_provider(ULONGLONG handle)
{
    ULONG index = (ULONG)handle;
    ULONG generation = (ULONG)(handle >> 32);
    if (!index || index > ETW_MAX_PROVIDERS || !generation)
        return 0;
    ETW_PROVIDER *provider = &g_etw_providers[index - 1];
    return provider->Active && provider->Generation == generation
        ? provider : 0;
}

static ULONG etw_register(const void *guid, ETW_ENABLE_CALLBACK callback,
                          void *context, BOOLEAN classic,
                          ULONGLONG *out_handle)
{
    if (out_handle) *out_handle = 0;
    if (!guid || !out_handle)
        return ETW_ERROR_INVALID_PARAMETER;
    etw_lock();
    for (ULONG i = 0; i < ETW_MAX_PROVIDERS; ++i) {
        ETW_PROVIDER *p = &g_etw_providers[i];
        if (p->Active) continue;
        memcpy(p->Guid, guid, 16);
        p->Callback = callback;
        p->ClassicCallback = 0;
        p->Context = context;
        p->MatchAny = p->MatchAll = 0;
        p->Level = p->Enabled = 0;
        p->Classic = classic;
        if (!++p->Generation) ++p->Generation;
        p->Active = 1;
        *out_handle = ((ULONGLONG)p->Generation << 32) | (i + 1);
        etw_unlock();
        return 0;
    }
    etw_unlock();
    return ETW_ERROR_NOT_ENOUGH_MEMORY;
}

ULONG EtwEventRegister(const void *id, ETW_ENABLE_CALLBACK callback,
                       void *context, ULONGLONG *reg)
{ return etw_register(id, callback, context, FALSE, reg); }

ULONG EtwEventUnregister(ULONGLONG handle)
{
    if (!handle) return 0;
    etw_lock();
    ETW_PROVIDER *p = etw_provider(handle);
    if (!p) { etw_unlock(); return ETW_ERROR_INVALID_HANDLE; }
    p->Active = p->Enabled = 0;
    p->Callback = 0;
    p->ClassicCallback = 0;
    etw_unlock();
    return 0;
}

/* Private, process-local trace controller. Its callbacks use the documented
 * enable/disable signature and receive the provider's registration context. */
ULONG SvEtwEnableProvider(const void *guid, BOOLEAN enabled, BYTE level,
                          ULONGLONG match_any, ULONGLONG match_all)
{
    if (!guid) return ETW_ERROR_INVALID_PARAMETER;
    ULONG found = 0;
    for (ULONG i = 0; i < ETW_MAX_PROVIDERS; ++i) {
        ETW_ENABLE_CALLBACK callback;
        ETW_CLASSIC_CALLBACK classic_callback;
        void *context;
        ETW_WNODE_HEADER wnode;
        ULONG request_size = sizeof(wnode);
        etw_lock();
        ETW_PROVIDER *p = &g_etw_providers[i];
        if (!p->Active ||
            memcmp(p->Guid, guid, 16) != 0) {
            etw_unlock();
            continue;
        }
        found = 1;
        p->Enabled = enabled;
        p->Level = level;
        p->MatchAny = match_any;
        p->MatchAll = match_all;
        callback = p->Callback;
        classic_callback = p->ClassicCallback;
        context = p->Context;
        memset(&wnode, 0, sizeof(wnode));
        wnode.BufferSize = sizeof(wnode);
        wnode.HistoricalContext = ((ULONGLONG)p->Generation << 32) | (i + 1);
        memcpy(wnode.Guid, p->Guid, 16);
        etw_unlock();
        if (classic_callback)
            classic_callback(enabled ? 4u : 5u, context, &request_size,
                             &wnode);
        else if (callback)
            callback(guid, enabled ? 1u : 0u, level, match_any,
                     match_all, 0, context);
    }
    return found ? 0 : ETW_ERROR_INVALID_HANDLE;
}

static BOOLEAN etw_enabled(const ETW_PROVIDER *p,
                            const ETW_EVENT_DESCRIPTOR *desc)
{
    if (!p || !p->Enabled || !desc) return FALSE;
    if (p->Level && desc->Level > p->Level) return FALSE;
    if (desc->Keyword && p->MatchAny &&
        !(desc->Keyword & p->MatchAny)) return FALSE;
    if ((desc->Keyword & p->MatchAll) != p->MatchAll) return FALSE;
    return TRUE;
}

BOOLEAN EtwEventEnabled(ULONGLONG handle, const ETW_EVENT_DESCRIPTOR *desc)
{
    etw_lock();
    BOOLEAN enabled = etw_enabled(etw_provider(handle), desc);
    etw_unlock();
    return enabled;
}

static ULONG etw_write(ULONGLONG handle, const ETW_EVENT_DESCRIPTOR *desc,
                       const void *activity, const void *related,
                       ULONG count, const ETW_DATA_DESCRIPTOR *data,
                       BOOLEAN classic)
{
    if (!handle) return 0;
    if (!desc || count > 128 || (count && !data))
        return ETW_ERROR_INVALID_PARAMETER;
    etw_lock();
    ETW_PROVIDER *p = etw_provider(handle);
    if (!p || p->Classic != classic) {
        etw_unlock();
        return ETW_ERROR_INVALID_HANDLE;
    }
    if (!etw_enabled(p, desc)) {
        etw_unlock();
        return 0;
    }
    ULONG size = 0;
    for (ULONG i = 0; i < count; ++i) {
        if ((data[i].Size && !data[i].Ptr) ||
            data[i].Size > ETW_MAX_EVENT_DATA - size) {
            etw_unlock();
            return ETW_ERROR_MORE_DATA;
        }
        size += data[i].Size;
    }
    ULONG tail = (g_etw_event_head + g_etw_event_count) % ETW_MAX_EVENTS;
    if (g_etw_event_count == ETW_MAX_EVENTS) {
        g_etw_event_head = (g_etw_event_head + 1) % ETW_MAX_EVENTS;
        tail = (g_etw_event_head + g_etw_event_count - 1) % ETW_MAX_EVENTS;
    } else ++g_etw_event_count;
    ETW_CAPTURED_EVENT *event = &g_etw_events[tail];
    memcpy(event->Provider, p->Guid, 16);
    if (activity) memcpy(event->Activity, activity, 16);
    else memset(event->Activity, 0, 16);
    if (related) memcpy(event->RelatedActivity, related, 16);
    else memset(event->RelatedActivity, 0, 16);
    event->Descriptor = *desc;
    event->Size = size;
    BYTE *out = event->Data;
    for (ULONG i = 0; i < count; ++i) {
        if (data[i].Size) memcpy(out, (const void *)data[i].Ptr, data[i].Size);
        out += data[i].Size;
    }
    etw_unlock();
    return 0;
}

ULONG EtwEventWrite(ULONGLONG handle, const ETW_EVENT_DESCRIPTOR *desc,
                    ULONG count, const ETW_DATA_DESCRIPTOR *data)
{ return etw_write(handle, desc, 0, 0, count, data, FALSE); }

ULONG EtwEventWriteTransfer(ULONGLONG handle,
                            const ETW_EVENT_DESCRIPTOR *desc,
                            const void *activity, const void *related,
                            ULONG count, const ETW_DATA_DESCRIPTOR *data)
{
    return etw_write(handle, desc, activity, related, count, data, FALSE);
}

ULONG SvEtwReadEvent(ETW_CAPTURED_EVENT *out)
{
    if (!out) return ETW_ERROR_INVALID_PARAMETER;
    etw_lock();
    if (!g_etw_event_count) { etw_unlock(); return 1168u; }
    *out = g_etw_events[g_etw_event_head];
    g_etw_event_head = (g_etw_event_head + 1) % ETW_MAX_EVENTS;
    --g_etw_event_count;
    etw_unlock();
    return 0;
}

typedef struct _ETW_TRACE_GUID_REGISTRATION {
    const void *Guid;
    ULONGLONG RegHandle;
} ETW_TRACE_GUID_REGISTRATION;

ULONG EtwRegisterTraceGuidsW(void *callback, void *context, const void *guid,
                             ULONG count, ETW_TRACE_GUID_REGISTRATION *classes,
                             const WCHAR *mof_path, const WCHAR *mof_name,
                             ULONGLONG *handle)
{
    (void)mof_path; (void)mof_name;
    if (!callback || !guid || !handle || (count && !classes))
        return ETW_ERROR_INVALID_PARAMETER;
    ULONG status = etw_register(guid, 0, context, TRUE, handle);
    if (!status) {
        etw_lock();
        ETW_PROVIDER *provider = etw_provider(*handle);
        if (provider) provider->ClassicCallback = (ETW_CLASSIC_CALLBACK)callback;
        etw_unlock();
    }
    if (!status)
        for (ULONG i = 0; i < count; ++i)
            classes[i].RegHandle = *handle;
    return status;
}

ULONG EtwUnregisterTraceGuids(ULONGLONG handle)
{ return EtwEventUnregister(handle); }

ULONG EtwEventSetInformation(ULONGLONG handle, ULONG cls,
                             const void *info, ULONG size)
{
    (void)cls; (void)info; (void)size;
    return etw_provider(handle) ? ETW_ERROR_NOT_SUPPORTED :
                                  ETW_ERROR_INVALID_HANDLE;
}

ULONG EtwGetTraceEnableLevel(ULONGLONG handle)
{
    etw_lock();
    ETW_PROVIDER *p = etw_provider(handle);
    ULONG level = p && p->Classic && p->Enabled ? p->Level : 0;
    etw_unlock();
    return level;
}
ULONG EtwGetTraceEnableFlags(ULONGLONG handle)
{
    etw_lock();
    ETW_PROVIDER *p = etw_provider(handle);
    ULONG flags = p && p->Classic && p->Enabled ? (ULONG)p->MatchAny : 0;
    etw_unlock();
    return flags;
}
ULONGLONG EtwGetTraceLoggerHandle(const void *buffer)
{
    if (!buffer) return ~(ULONGLONG)0;
    const ETW_WNODE_HEADER *wnode = buffer;
    if (wnode->BufferSize < sizeof(*wnode)) return ~(ULONGLONG)0;
    ULONGLONG handle = wnode->HistoricalContext;
    etw_lock();
    ETW_PROVIDER *p = etw_provider(handle);
    BOOLEAN valid = p && p->Classic && p->Enabled &&
                    memcmp(p->Guid, wnode->Guid, 16) == 0;
    etw_unlock();
    return valid ? handle : ~(ULONGLONG)0;
}
ULONG EtwLogTraceEvent(const void *event)
{ (void)event; return ETW_ERROR_INVALID_HANDLE; }
ULONG EtwTraceMessageVa(ULONGLONG logger, ULONG flags, const void *guid,
                        USHORT number, va_list args)
{
    if (!logger) return ETW_ERROR_INVALID_HANDLE;
    if ((flags & ~(ETW_TRACE_MESSAGE_GUID |
                   ETW_TRACE_MESSAGE_COMPONENTID)) ||
        (flags & (ETW_TRACE_MESSAGE_GUID | ETW_TRACE_MESSAGE_COMPONENTID)) ==
        (ETW_TRACE_MESSAGE_GUID | ETW_TRACE_MESSAGE_COMPONENTID) ||
        ((flags & (ETW_TRACE_MESSAGE_GUID |
                   ETW_TRACE_MESSAGE_COMPONENTID)) && !guid))
        return ETW_ERROR_INVALID_PARAMETER;
    ETW_DATA_DESCRIPTOR parts[65];
    ULONG count = 0, size = 0;
    if (flags & (ETW_TRACE_MESSAGE_GUID | ETW_TRACE_MESSAGE_COMPONENTID)) {
        ULONG prefix = flags & ETW_TRACE_MESSAGE_GUID ? 16 : 4;
        parts[count++] = (ETW_DATA_DESCRIPTOR){(ULONGLONG)guid, prefix, 0};
        size = prefix;
    }
    for (;;) {
        const void *data = va_arg(args, const void *);
        size_t bytes = va_arg(args, size_t);
        if (!data) {
            if (bytes) return ETW_ERROR_INVALID_PARAMETER;
            break;
        }
        if (count == 65 || bytes > ETW_MAX_EVENT_DATA - size)
            return ETW_ERROR_MORE_DATA;
        parts[count++] = (ETW_DATA_DESCRIPTOR){(ULONGLONG)data,
                                               (ULONG)bytes, 0};
        size += (ULONG)bytes;
    }
    ETW_EVENT_DESCRIPTOR descriptor = {0};
    descriptor.Id = number;
    return etw_write(logger, &descriptor, 0, 0, count, parts, TRUE);
}
ULONG EtwTraceMessage(ULONGLONG logger, ULONG flags, const void *guid,
                      ULONG number, ...)
{
    va_list args;
    va_start(args, number);
    ULONG status = EtwTraceMessageVa(logger, flags, guid, (USHORT)number,
                                     args);
    va_end(args);
    return status;
}
