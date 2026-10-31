/* Read-only Linux sysfs views for native devices exposed by Svindows. */
#include "Internal.h"
#include <svindows/Io.h>
#include <svindows/Rtl.h>

static BOOLEAN linux_path_equals(const char *actual, const char *expected)
{
    for (;;) {
        char a = *actual++, b = *expected++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return FALSE;
        if (!a) return TRUE;
    }
}

UINT64 LinuxReadlinkAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                       UINT64 buffer, UINT64 size)
{
    if (!size) return LinuxError(LINUX_EINVAL);
    char dos_path[260];
    UINT64 error = LinuxCapturePathAt(process, directory_fd, path, dos_path);
    if (error) return error;
    UINT8 proc = LinuxProcPathKind(dos_path);
    if (proc == LINUX_PROC_EXE || proc == LINUX_PROC_CWD)
        return LinuxProcReadlink(process, proc, buffer, size);
    if (proc == LINUX_PROC_UNKNOWN) return LinuxError(LINUX_ENOENT);
    if (proc != LINUX_PROC_NONE) return LinuxError(LINUX_EINVAL);
    static const char framebuffer_link[] =
        "../../devices/virtual/graphics/fb0";
    static const char framebuffer_bus[] = "../../../../../bus/platform";
    const char *target = NULL;
    UINT64 target_length = 0;
    if (LinuxFramebufferSize() &&
        linux_path_equals(dos_path, "C:\\sys\\class\\graphics\\fb0")) {
        target = framebuffer_link;
        target_length = sizeof(framebuffer_link) - 1;
    } else if (LinuxFramebufferSize() && linux_path_equals(
            dos_path, "C:\\sys\\class\\graphics\\fb0\\device\\subsystem")) {
        target = framebuffer_bus;
        target_length = sizeof(framebuffer_bus) - 1;
    }

    /* Input class links: /sys/class/input/eventN points at the owning input
     * device, and its "device" link points at the virtual input node. libinput
     * follows these when it enumerates evdev devices. event's own directory
     * link is a static relative path; the device link is emitted with the
     * concrete index filled in. */
    {
        static const char device_prefix[] =
            "../../devices/virtual/input/input";
        /* Number of devices we advertise; index 0..2. */
        for (UINT32 i = 0; i < LINUX_EVENT_DEVICE_COUNT && !target; i++) {
            if (!LinuxEventDevice(i)) continue;
            char class_path[64];
            SIZE_T n = 0;
            const char *stem = "C:\\sys\\class\\input\\event";
            for (; stem[n]; n++) class_path[n] = stem[n];
            class_path[n++] = (char)('0' + i);
            class_path[n] = 0;
            if (linux_path_equals(dos_path, class_path)) {
                /* /sys/class/input/eventN ->
                 * /sys/devices/virtual/input/inputN/eventN */
                char link[96];
                SIZE_T k = 0;
                for (; device_prefix[k]; k++) link[k] = device_prefix[k];
                link[k++] = (char)('0' + i);
                const char *event_suffix = "/event";
                for (SIZE_T s = 0; event_suffix[s]; s++) link[k++] = event_suffix[s];
                link[k++] = (char)('0' + i);
                link[k] = 0;
                UINT64 copied = k;
                if (copied > size) copied = size;
                if (!MmProbeForWrite(buffer, copied))
                    return LinuxError(LINUX_EFAULT);
                memcpy((void *)(ULONG_PTR)buffer, link, (SIZE_T)copied);
                return copied;
            }
            /* .../eventN/device -> ../../../../../virtual/input/inputN */
            char device_path[80];
            for (n = 0; class_path[n] && n + 8 < sizeof(device_path); n++)
                device_path[n] = class_path[n];
            const char *device_suffix = "\\device";
            for (SIZE_T s = 0; device_suffix[s]; s++)
                device_path[n++] = device_suffix[s];
            device_path[n] = 0;
            if (linux_path_equals(dos_path, device_path)) {
                char link[96];
                SIZE_T k = 0;
                const char *dp = "/sys/devices/virtual/input/input";
                for (; dp[k]; k++) link[k] = dp[k];
                link[k++] = (char)('0' + i);
                link[k] = 0;
                UINT64 copied = k;
                if (copied > size) copied = size;
                if (!MmProbeForWrite(buffer, copied))
                    return LinuxError(LINUX_EFAULT);
                memcpy((void *)(ULONG_PTR)buffer, link, (SIZE_T)copied);
                return copied;
            }
        }
    }

    if (target) {
        UINT64 copied = target_length;
        if (copied > size) copied = size;
        if (!MmProbeForWrite(buffer, copied)) return LinuxError(LINUX_EFAULT);
        memcpy((void *)(ULONG_PTR)buffer, target, (SIZE_T)copied);
        return copied;
    }

    FAT_FIND_DATA info;
    NTSTATUS status = FatQueryPath(dos_path, &info);
    return NT_SUCCESS(status) ? LinuxError(LINUX_EINVAL) :
                                LinuxError(LINUX_ENOENT);
}
