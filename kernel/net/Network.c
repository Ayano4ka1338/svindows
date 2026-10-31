/* Network subsystem entry point. Ethernet framing and the IP stack are
 * separate layers: the former owns PCI/VirtIO DMA, the latter owns all
 * protocol state and only receives frames from the passive worker. */
#include <svindows/Net.h>
#include <svindows/Ke.h>

BOOLEAN VirtioNetInitialize(void);
const char *VirtioNetGetDiagnostic(void);
BOOLEAN LwipInitialize(void);

static const char *g_net_diagnostic = "network initialization has not run";
static BOOLEAN g_stack_ready;

const char *NetGetInitStatus(void)
{
    return g_net_diagnostic;
}

BOOLEAN NetIsStackReady(void)
{
    return g_stack_ready;
}

BOOLEAN NetInitialize(void)
{
    if (g_stack_ready) return TRUE;
    /* Bring up the protocol core independently from the physical transport.
       This keeps AF_INET and the native loopback interface functional even
       when no supported PCI NIC is present. */
    if (!NetIsAvailable() && !VirtioNetInitialize()) {
        g_net_diagnostic = VirtioNetGetDiagnostic();
        KeLog("[net] no usable Ethernet controller: %s\n", g_net_diagnostic);
    }
    if (!LwipInitialize()) {
        g_net_diagnostic = "lwIP TCP/IP core initialization failed";
        KeLog("[net] lwIP TCP/IP core initialization failed\n");
        return FALSE;
    }
    g_stack_ready = TRUE;
    if (NetIsAvailable())
        g_net_diagnostic = "lwIP and VirtIO initialized; waiting for DHCP";
    else
        g_net_diagnostic = VirtioNetGetDiagnostic();
    return TRUE;
}
