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

static void set_config_cb(usbd_device *d, uint16_t wValue)
{
	(void)wValue;
	usbd_ep_setup(d, EP_CONSOLE_OUT, USB_ENDPOINT_ATTR_BULK, EP_SIZE, console_out_cb);
	usbd_ep_setup(d, EP_CONSOLE_IN, USB_ENDPOINT_ATTR_BULK, EP_SIZE, NULL);
	usbd_ep_setup(d, EP_IO_OUT, USB_ENDPOINT_ATTR_BULK, EP_SIZE, io_out_cb);
	usbd_ep_setup(d, EP_IO_IN, USB_ENDPOINT_ATTR_BULK, EP_SIZE, NULL);
	configured = true;
	guard_usb_configured();
}

static void reset_cb(void)
{
	configured = false;
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
}

static void pump_console(void)
{
	uint8_t buf[EP_SIZE];
	unsigned n = 0, tail = console_tail;

	while (n < EP_SIZE && tail != console_head) {
		buf[n++] = console_ring[tail];
		tail = (tail + 1) % CONSOLE_RING;
	}
	if (n == 0)
		return;
	if (usbd_ep_write_packet(dev, EP_CONSOLE_IN, buf, (uint16_t)n) == n)
		console_tail = tail;
}

static void pump_io(void)
{
	if (io_tail == io_head)
		return;
	if (usbd_ep_write_packet(dev, EP_IO_IN, io_queue[io_tail], io_len[io_tail]) == io_len[io_tail])
		io_tail = (io_tail + 1) % IO_QUEUE;
}

void usb_poll(void)
{
	usbd_poll(dev);
	if (!configured)
		return;
	pump_console();
	pump_io();
}

#else /* TSX_QEMU: no USB model, the console runs on USART1 */

void usb_init(void)
{
	configured = true;
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
