# USB input

Svindows currently supports USB HID boot-protocol keyboards and mice attached
to xHCI root ports at boot. The xHCI driver takes ownership from firmware,
sets up DMA command/event rings, enumerates the device and configures its
interrupt endpoint. It polls the event ring from the input worker and feeds
keyboard transitions and mouse reports into the same input queues used by the
PS/2 devices. Keyboard reports use the existing set-1 decoder; mouse reports
update the shared pointer state and Linux evdev stream.

The current USB scope is input. It does not yet enumerate devices behind USB
hubs, discover devices plugged in after boot, parse arbitrary HID report
descriptors, or support USB storage/audio/network devices. The host-controller
driver currently implements xHCI; legacy UHCI/OHCI/EHCI controllers are not
implemented. For now, use a boot-compatible USB keyboard or mouse connected
directly to an xHCI root port.

## QEMU smoke setup

To expose an xHCI controller with a USB keyboard and mouse, add these QEMU
arguments:

```sh
-machine q35 \
-device qemu-xhci,id=xhci \
-device usb-kbd,bus=xhci.0 \
-device usb-mouse,bus=xhci.0
```

The kernel log reports controller startup and each recognized HID device.
