// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * USB device: the same descriptors as the stock LED bar firmware, so the
 * mainline driver leds-crestron-stm32 and the vendor stack bind as before.
 *
 *   VID 0x14BE, PID 0x001B, one configuration, self-powered, 0 mA
 *   interface 0: class 0xFF, subclass 0, console text, EP 0x81 IN, 0x01 OUT
 *   interface 1: class 0xFF, subclass 1, Cresnet packets, EP 0x82 IN, 0x02 OUT
 *   strings: 1 manufacturer, 2 product, 3 device id, 4 firmware name
 *
 * The OTG FS core runs in device mode without VBUS sensing (the stock
 * firmware sets GCCFG the same way: the bar is bus-powered and PA9 is
 * not a VBUS input). Everything is polled from the main loop.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "tsx.h"

#define EP_CONSOLE_IN	0x81
#define EP_CONSOLE_OUT	0x01
#define EP_IO_IN	0x82
#define EP_IO_OUT	0x02
#define EP_SIZE		64

#define CONSOLE_RING	2048
#define IO_QUEUE	8

#ifndef TSX_QEMU
static uint8_t console_ring[CONSOLE_RING];
#endif
static volatile unsigned console_head, console_tail;

static uint8_t io_queue[IO_QUEUE][EP_SIZE];
static uint8_t io_len[IO_QUEUE];
static volatile unsigned io_head, io_tail;

static volatile bool configured;

void console_write(const char *s)
{
#ifdef TSX_QEMU
	qemu_uart_write(s);
#else
	while (*s) {
		unsigned next = (console_head + 1) % CONSOLE_RING;

		if (next == console_tail)
			break;		/* ring full: drop the rest */
		console_ring[console_head] = (uint8_t)*s++;
		console_head = next;
	}
#endif
}

void console_printf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	console_write(buf);
}

bool cresnet_send(const uint8_t *pkt, size_t n)
{
	unsigned next = (io_head + 1) % IO_QUEUE;

	if (n > EP_SIZE || next == io_tail)
		return false;
	memcpy(io_queue[io_head], pkt, n);
	io_len[io_head] = (uint8_t)n;
	io_head = next;
	return true;
}

bool usb_configured(void)
{
	return configured;
}

#ifndef TSX_QEMU

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/usb/dwc/otg_fs.h>
#include <libopencm3/usb/usbd.h>

static usbd_device *dev;
static uint8_t control_buf[128];

/* IN endpoint state, see "IN endpoint arming" below */
struct in_ep {
	uint8_t last[EP_SIZE];
	uint8_t last_len;
	uint32_t stuck_since;
	bool stuck_seen;
	bool resend;		/* send the last packet again when the endpoint is idle */
};

static struct in_ep in_eps[3];	/* index = endpoint number */

static const struct usb_device_descriptor dev_desc = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = 0,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = EP_SIZE,
	.idVendor = 0x14BE,
	.idProduct = 0x001B,
	.bcdDevice = 0x0100,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

static const struct usb_endpoint_descriptor console_eps[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = EP_CONSOLE_IN,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = EP_SIZE,
	.bInterval = 0,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = EP_CONSOLE_OUT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = EP_SIZE,
	.bInterval = 0,
}};

static const struct usb_endpoint_descriptor io_eps[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = EP_IO_IN,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = EP_SIZE,
	.bInterval = 0,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = EP_IO_OUT,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = EP_SIZE,
	.bInterval = 0,
}};

static const struct usb_interface_descriptor console_iface = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_VENDOR,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,
	.endpoint = console_eps,
};

static const struct usb_interface_descriptor io_iface = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_VENDOR,
	.bInterfaceSubClass = 1,
	.bInterfaceProtocol = 0,
	.iInterface = 0,
	.endpoint = io_eps,
};

static const struct usb_interface ifaces[] = {
	{ .num_altsetting = 1, .altsetting = &console_iface },
	{ .num_altsetting = 1, .altsetting = &io_iface },
};

static const struct usb_config_descriptor config_desc = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 2,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0xC0,	/* self-powered, as the stock descriptor */
	.bMaxPower = 0,
	.interface = ifaces,
};

static const char *const strings[] = {
	"tsx-mainline",
	"IO Processor",
	"0",
	TSX_FW_NAME,
};

static void console_out_cb(usbd_device *d, uint8_t ep)
{
	uint8_t buf[EP_SIZE];
	int n = usbd_ep_read_packet(d, ep, buf, sizeof(buf));

	if (n > 0)
		console_rx(buf, (size_t)n);
}

static void io_out_cb(usbd_device *d, uint8_t ep)
{
	uint8_t buf[EP_SIZE];
	int n = usbd_ep_read_packet(d, ep, buf, sizeof(buf));

	if (n > 0)
		cresnet_rx(buf, (size_t)n);
}

/*
 * SET_CONFIGURATION. The value 0 puts the device back in the address
 * state: the library has reset the endpoints, and the firmware must stop
 * the IN data. It is not a configuration for the guard either.
 */
static void set_config_cb(usbd_device *d, uint16_t wValue)
{
	if (wValue == 0) {
		configured = false;
		return;
	}
	memset(in_eps, 0, sizeof(in_eps));	/* new endpoints: no packet to send again */
	usbd_ep_setup(d, EP_CONSOLE_OUT, USB_ENDPOINT_ATTR_BULK, EP_SIZE, console_out_cb);
	usbd_ep_setup(d, EP_CONSOLE_IN, USB_ENDPOINT_ATTR_BULK, EP_SIZE, NULL);
	usbd_ep_setup(d, EP_IO_OUT, USB_ENDPOINT_ATTR_BULK, EP_SIZE, io_out_cb);
	usbd_ep_setup(d, EP_IO_IN, USB_ENDPOINT_ATTR_BULK, EP_SIZE, NULL);
	configured = true;
	guard_usb_configured();
}

/*
 * The guard must know when a host runs on the bus. A bus reset alone does
 * not tell it: a host that powers up or goes down can give a short reset.
 * A running host sends a SOF packet every millisecond, and the core keeps
 * the frame number of the last SOF in DSTS. So a new frame number after a
 * bus reset tells that a host runs.
 */
static bool sof_wait;		/* a bus reset came, no SOF after it yet */
static uint32_t sof_frame;

static uint32_t frame_number(void)
{
	return (OTG_FS_DSTS >> 8) & 0x3FFF;	/* FNSOF */
}

static void reset_cb(void)
{
	configured = false;
	sof_wait = true;
	sof_frame = frame_number();
}

/*
 * SET_INTERFACE: the Linux host sends it when a userland program
 * releases an interface (usbfs, libusb), and then starts its data
 * toggles at DATA0 again. The core must do the same, or every second
 * packet of the interface is dropped as a repeat.
 */
static void altsetting_cb(usbd_device *d, uint16_t wIndex, uint16_t wValue)
{
	uint8_t ep = wIndex == 0 ? 1 : 2;

	(void)d;
	(void)wValue;
	if (wIndex > 1)
		return;
	OTG_FS_DIEPCTL(ep) |= OTG_DIEPCTLX_SD0PID;
	OTG_FS_DOEPCTL(ep) |= OTG_DOEPCTLX_SD0PID;
}

void usb_init(void)
{
	gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO11 | GPIO12);
	gpio_set_output_options(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_100MHZ, GPIO11 | GPIO12);
	gpio_set_af(GPIOA, GPIO_AF10, GPIO11 | GPIO12);

	dev = usbd_init(&otgfs_usb_driver, &dev_desc, &config_desc, strings,
			sizeof(strings) / sizeof(strings[0]), control_buf, sizeof(control_buf));
	/* no VBUS pin: the core must not wait for a VBUS level */
	OTG_FS_GCCFG |= OTG_GCCFG_NOVBUSSENS;
	usbd_register_set_config_callback(dev, set_config_cb);
	usbd_register_reset_callback(dev, reset_cb);
	usbd_register_set_altsetting_callback(dev, altsetting_cb);
}

/*
 * IN endpoint arming. The dwc core needs care here:
 *
 * 1. The poll loop of the library writes SNAK after each completed
 *    packet. A CNAK written before that NAK took effect is lost, and
 *    the endpoint then NAKs every IN token with EPENA set: stuck. So a
 *    new packet is armed only when EPENA is clear and NAKSTS reads 1.
 * 2. The library's write function handles a still-enabled endpoint with
 *    unbounded waits on the core, which never end while the host does
 *    not poll. The check happens here instead, with bounded waits.
 * 3. Should an endpoint still get stuck (EPENA and NAKSTS for more than
 *    IN_STUCK_MS), it is disabled and its FIFO flushed. A copy of the
 *    last packet is kept, and it goes out again when the endpoint is idle
 *    (EPENA clear, NAKSTS set), by rule 1. When the disable does not end
 *    within its wait, EPENA stays set, and the endpoint is stuck again
 *    after IN_STUCK_MS. The library write is never called with EPENA set.
 */
#define IN_STUCK_MS	2000

static bool wait_bit(volatile uint32_t *reg, uint32_t mask, bool set, uint32_t ms)
{
	uint32_t start = millis();

	while (((*reg & mask) != 0) != set) {
		if (millis() - start > ms)
			return false;
		guard_kick();
	}
	return true;
}

static void ep_in_recover(uint8_t ep)
{
	uint32_t fifo = (OTG_FS_DIEPCTL(ep) & OTG_DIEPCTL0_TXFNUM_MASK) >> 22;

	OTG_FS_DIEPCTL(ep) |= OTG_DIEPCTL0_SNAK;
	wait_bit(&OTG_FS_DIEPINT(ep), OTG_DIEPINTX_INEPNE, true, 2);
	OTG_FS_DIEPCTL(ep) |= OTG_DIEPCTL0_EPDIS | OTG_DIEPCTL0_SNAK;
	wait_bit(&OTG_FS_DIEPINT(ep), OTG_DIEPINTX_EPDISD, true, 2);
	OTG_FS_DIEPINT(ep) = OTG_DIEPINTX_EPDISD | OTG_DIEPINTX_INEPNE;
	wait_bit(&OTG_FS_GRSTCTL, OTG_GRSTCTL_AHBIDL, true, 2);
	OTG_FS_GRSTCTL = (fifo << 6) | OTG_GRSTCTL_TXFFLSH;
	wait_bit(&OTG_FS_GRSTCTL, OTG_GRSTCTL_TXFFLSH, false, 2);
	errlog_add(ERR_USB, ep);
}

/* true when a packet may be armed now; handles a stuck endpoint */
static bool ep_in_ready(uint8_t addr)
{
	uint8_t ep = addr & 0x7F;
	struct in_ep *e = &in_eps[ep];
	uint32_t ctl = OTG_FS_DIEPCTL(ep);

	if (!(ctl & OTG_DIEPCTL0_EPENA)) {
		e->stuck_seen = false;
		if (!(ctl & OTG_DIEPCTL0_NAKSTS))
			return false;
		if (e->resend) {
			/* the packet that the recovery took off the endpoint */
			e->resend = false;
			usbd_ep_write_packet(dev, addr, e->last, e->last_len);
			return false;
		}
		return true;
	}
	if (!(ctl & OTG_DIEPCTL0_NAKSTS)) {
		e->stuck_seen = false;	/* packet in flight */
		return false;
	}
	if (!e->stuck_seen) {
		e->stuck_seen = true;
		e->stuck_since = millis();
		return false;
	}
	if (millis() - e->stuck_since < IN_STUCK_MS)
		return false;
	ep_in_recover(ep);
	e->stuck_seen = false;
	e->resend = e->last_len != 0;
	return false;
}

static bool ep_in_send(uint8_t addr, const uint8_t *buf, unsigned n)
{
	struct in_ep *e = &in_eps[addr & 0x7F];

	if (usbd_ep_write_packet(dev, addr, buf, (uint16_t)n) != n)
		return false;
	memcpy(e->last, buf, n);
	e->last_len = (uint8_t)n;
	return true;
}

/*
 * Console answers go out in packets of at most 63 bytes. Every packet is
 * then a short packet, and a host read of any size returns at once: the
 * stock tools read 512 bytes with a timeout and drop what a timed-out
 * read had collected.
 */
#define CONSOLE_CHUNK	(EP_SIZE - 1)

static void pump_console(void)
{
	uint8_t buf[EP_SIZE];
	unsigned n = 0, tail = console_tail;

	if (tail == console_head || !ep_in_ready(EP_CONSOLE_IN))
		return;
	while (n < CONSOLE_CHUNK && tail != console_head) {
		buf[n++] = console_ring[tail];
		tail = (tail + 1) % CONSOLE_RING;
	}
	if (ep_in_send(EP_CONSOLE_IN, buf, n))
		console_tail = tail;
}

static void pump_io(void)
{
	if (io_tail == io_head || !ep_in_ready(EP_IO_IN))
		return;
	if (ep_in_send(EP_IO_IN, io_queue[io_tail], io_len[io_tail]))
		io_tail = (io_tail + 1) % IO_QUEUE;
}

void usb_debug_state(uint32_t v[7])
{
	v[0] = OTG_FS_DIEPCTL(1);
	v[1] = OTG_FS_DIEPINT(1);
	v[2] = OTG_FS_DIEPTSIZ(1);
	v[3] = OTG_FS_GINTSTS;
	v[4] = OTG_FS_DAINT;
	v[5] = console_head;
	v[6] = console_tail;
}

void usb_poll(void)
{
	usbd_poll(dev);
	if (sof_wait && frame_number() != sof_frame) {
		sof_wait = false;
		guard_usb_host();
	}
	if (!configured)
		return;
	pump_console();
	pump_io();
}

/*
 * Before a planned reset: poll USB for at most ms, until the console text
 * has left the bar (the ring is empty and the IN endpoint is idle). Only
 * usb_poll moves text to the endpoint, so a reset right after a console
 * answer would lose the answer.
 */
void usb_flush(uint32_t ms)
{
	uint32_t start = millis();

	while (millis() - start < ms) {
		usb_poll();
		guard_kick();
		if (!configured)
			return;
		if (console_tail == console_head &&
		    !(OTG_FS_DIEPCTL(EP_CONSOLE_IN & 0x7F) & OTG_DIEPCTL0_EPENA))
			return;
	}
}

#else /* TSX_QEMU: no USB model, the console runs on USART1 */

void usb_flush(uint32_t ms)
{
	(void)ms;	/* console_write sends to the UART at once */
}

void usb_init(void)
{
	configured = true;
}

void usb_debug_state(uint32_t v[7])
{
	for (int i = 0; i < 7; i++)
		v[i] = 0;
}

void usb_poll(void)
{
	int c;

	/* drop the ring content: console_printf already wrote it to the UART */
	console_tail = console_head;
	io_tail = io_head;
	while ((c = qemu_uart_read()) >= 0) {
		uint8_t b = (uint8_t)c;

		console_rx(&b, 1);
	}
}

#endif
