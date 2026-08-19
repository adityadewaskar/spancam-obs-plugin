/*
Spancam OBS Studio plugin — Android Open Accessory (AOA) transport
Copyright (C) 2026 Aditya Dewaskar <support@adewaskar.com>
SPDX-License-Identifier: GPL-2.0-or-later

The IOUSBLib half is adapted from the Spancam Mac app's CSpancamUSB.c, and the
AOA discovery/handshake sequence from its AOAReceiver.swift, so the cable behaves
identically in OBS and in the desktop app.
*/

#include "spancam-aoa.h"
#include <plugin-support.h>
#include <obs-module.h>

#include <stdlib.h>
#include <string.h>

#if !defined(__APPLE__)
// Non-Apple: no backend yet. The declarations still resolve so the source file
// needs no #ifdef around its call sites, and USB simply reports "unavailable".
bool spancam_aoa_device_present(void)
{
	return false;
}
spancam_aoa_t *spancam_aoa_open(char *err, size_t errsz)
{
	if (err && errsz)
		snprintf(err, errsz, "AOA is macOS-only in this build");
	return NULL;
}
int spancam_aoa_read(spancam_aoa_t *a, void *buf, uint32_t *len, uint32_t timeout_ms)
{
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(buf);
	UNUSED_PARAMETER(len);
	UNUSED_PARAMETER(timeout_ms);
	return -1;
}
bool spancam_aoa_write(spancam_aoa_t *a, const void *buf, uint32_t len, uint32_t timeout_ms)
{
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(buf);
	UNUSED_PARAMETER(len);
	UNUSED_PARAMETER(timeout_ms);
	return false;
}
void spancam_aoa_close(spancam_aoa_t *a)
{
	UNUSED_PARAMETER(a);
}
#else // __APPLE__

#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h> // explicit: do not rely on obs-module.h pulling it in
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOReturn.h>

// AOA control requests (Android Open Accessory 1.0).
#define AOA_GET_PROTOCOL 51
#define AOA_SEND_STRING 52
#define AOA_START_ACCESSORY 53

// Identity strings. Indices 0-3 are mandatory and MUST match the app's
// res/xml/accessory_filter.xml byte for byte
//   <usb-accessory manufacturer="Spancam" model="Receiver" version="1.0" />
// or the phone will not route the accessory to Spancam. 4 (uri) and 5 (serial)
// are sent empty, as the Mac app does.
static const char *const kIdentity[6] = {"Spancam", "Receiver", "Spancam Virtual Camera", "1.0", "", ""};

#define AOA_ACCESSORY_VID 0x18D1
static bool aoa_is_accessory_pid(uint16_t pid)
{
	return pid == 0x2D00 || pid == 0x2D01;
}

// Vendor IDs of phone makers, matching the Mac app's list.
static const uint16_t kAndroidVIDs[] = {0x04E8, 0x18D1, 0x2717, 0x22D9, 0x2A70, 0x1004, 0x0BB4, 0x2B4C,
					0x12D1, 0x19D2, 0x22B8, 0x0FCE, 0x2A47, 0x1949, 0x0E8D};

// 64 KiB reads: at 1080p/4K H.264 an access unit is tens of KB, so a big buffer
// keeps the bulk pipe ahead of the encoder instead of paying a syscall per chunk.
#define AOA_READ_CHUNK 65536

struct spancam_aoa {
	IOUSBInterfaceInterface500 **intf;
	uint8_t in_pipe, out_pipe;
};

// ---------------------------------------------------------------- IOKit utils

static uint16_t reg_u16(io_service_t svc, const char *key)
{
	CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
	if (!k)
		return 0;
	CFTypeRef prop = IORegistryEntryCreateCFProperty(svc, k, kCFAllocatorDefault, 0);
	CFRelease(k);
	if (!prop)
		return 0;
	uint16_t out = 0;
	if (CFGetTypeID(prop) == CFNumberGetTypeID())
		CFNumberGetValue((CFNumberRef)prop, kCFNumberSInt16Type, &out);
	CFRelease(prop);
	return out;
}

typedef bool (*svc_pred)(io_service_t svc);

static io_service_t find_service(const char *class_name, svc_pred pred)
{
	CFMutableDictionaryRef match = IOServiceMatching(class_name);
	if (!match)
		return 0;
	io_iterator_t iter = 0;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
		return 0;
	io_service_t found = 0, svc;
	while ((svc = IOIteratorNext(iter)) != 0) {
		if (pred(svc)) {
			found = svc;
			break;
		}
		IOObjectRelease(svc);
	}
	IOObjectRelease(iter);
	return found;
}

static bool pred_accessory(io_service_t svc)
{
	return reg_u16(svc, "idVendor") == AOA_ACCESSORY_VID && aoa_is_accessory_pid(reg_u16(svc, "idProduct"));
}

static bool pred_android(io_service_t svc)
{
	uint16_t vid = reg_u16(svc, "idVendor");
	if (vid == AOA_ACCESSORY_VID && aoa_is_accessory_pid(reg_u16(svc, "idProduct")))
		return false; // already switched — handled separately
	for (size_t i = 0; i < sizeof(kAndroidVIDs) / sizeof(kAndroidVIDs[0]); i++)
		if (kAndroidVIDs[i] == vid)
			return true;
	return false;
}

// The bulk interface whose parent USB device is the switched accessory.
static bool pred_accessory_iface(io_service_t svc)
{
	io_registry_entry_t entry = svc;
	IOObjectRetain(entry);
	io_registry_entry_t parent = 0;
	bool hit = false;
	while (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) == KERN_SUCCESS) {
		IOObjectRelease(entry);
		entry = parent;
		io_name_t cls;
		if (IOObjectGetClass(entry, cls) == KERN_SUCCESS && strcmp(cls, "IOUSBHostDevice") == 0) {
			hit = pred_accessory(entry);
			break;
		}
	}
	IOObjectRelease(entry);
	return hit;
}

// ------------------------------------------------------------- device (ep0)

static IOUSBDeviceInterface500 **open_device(io_service_t device, int32_t *out_kr)
{
	IOCFPlugInInterface **plugin = NULL;
	SInt32 score = 0;
	kern_return_t kr = IOCreatePlugInInterfaceForService(device, kIOUSBDeviceUserClientTypeID,
							     kIOCFPlugInInterfaceID, &plugin, &score);
	if (kr != kIOReturnSuccess || !plugin) {
		*out_kr = (int32_t)kr;
		return NULL;
	}
	IOUSBDeviceInterface500 **dev = NULL;
	HRESULT res = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID *)&dev);
	IODestroyPlugInInterface(plugin);
	if (res != 0 || !dev) {
		*out_kr = -101;
		return NULL;
	}
	// A phone in MTP/ADB mode is claimed by the kernel composite driver, so seize
	// it. Retry on exclusive access: the driver may still be re-attaching.
	kern_return_t open_kr = kIOReturnExclusiveAccess;
	for (int i = 0; i < 10; i++) {
		open_kr = (*dev)->USBDeviceOpenSeize(dev);
		if (open_kr == kIOReturnSuccess)
			break;
		if (open_kr != kIOReturnExclusiveAccess) {
			kern_return_t plain = (*dev)->USBDeviceOpen(dev);
			if (plain == kIOReturnSuccess) {
				open_kr = plain;
				break;
			}
		}
		usleep(150000);
	}
	if (open_kr != kIOReturnSuccess) {
		(*dev)->Release(dev);
		*out_kr = (int32_t)open_kr;
		return NULL;
	}
	*out_kr = 0;
	return dev;
}

static int32_t dev_control(IOUSBDeviceInterface500 **dev, uint8_t type, uint8_t req, uint16_t val, uint16_t idx,
			   void *data, uint16_t len, uint32_t timeout_ms)
{
	IOUSBDevRequestTO r;
	memset(&r, 0, sizeof(r));
	r.bmRequestType = type;
	r.bRequest = req;
	r.wValue = val;
	r.wIndex = idx;
	r.wLength = len;
	r.pData = data;
	r.noDataTimeout = timeout_ms;
	r.completionTimeout = timeout_ms;
	return (int32_t)(*dev)->DeviceRequestTO(dev, &r);
}

// AOA 51/52/53. On success the phone detaches and re-enumerates as 0x18D1:0x2D0x.
static bool aoa_handshake(io_service_t svc, char *err, size_t errsz)
{
	int32_t kr = 0;
	IOUSBDeviceInterface500 **dev = open_device(svc, &kr);
	if (!dev) {
		snprintf(err, errsz, "could not open the USB device (%d) — another process may hold it", kr);
		return false;
	}
	bool ok = false;
	uint16_t proto = 0;
	if (dev_control(dev, 0xC0, AOA_GET_PROTOCOL, 0, 0, &proto, sizeof(proto), 5000) != 0 || proto < 1) {
		snprintf(err, errsz, "phone did not answer AOA GET_PROTOCOL — it may be in charge-only mode");
		goto out;
	}
	for (uint16_t i = 0; i < 6; i++) {
		char s[64];
		size_t n = strlen(kIdentity[i]);
		memcpy(s, kIdentity[i], n);
		s[n] = 0; // NUL-terminated, as AOA requires
		if (dev_control(dev, 0x40, AOA_SEND_STRING, 0, i, s, (uint16_t)(n + 1), 5000) != 0) {
			snprintf(err, errsz, "AOA SEND_STRING[%u] failed", i);
			goto out;
		}
	}
	if (dev_control(dev, 0x40, AOA_START_ACCESSORY, 0, 0, NULL, 0, 5000) != 0) {
		snprintf(err, errsz, "AOA START_ACCESSORY failed");
		goto out;
	}
	ok = true;
out:
	(*dev)->USBDeviceClose(dev);
	(*dev)->Release(dev);
	return ok;
}

// ------------------------------------------------------------------- public

bool spancam_aoa_device_present(void)
{
	io_service_t svc = find_service("IOUSBHostDevice", pred_accessory);
	if (svc) {
		IOObjectRelease(svc);
		return true;
	}
	svc = find_service("IOUSBHostDevice", pred_android);
	if (svc) {
		IOObjectRelease(svc);
		return true;
	}
	return false;
}

spancam_aoa_t *spancam_aoa_open(char *err, size_t errsz)
{
	err[0] = 0;

	// Already in accessory mode? Re-running the handshake on a switched device
	// resets its accessory fd, so reuse it.
	io_service_t acc = find_service("IOUSBHostDevice", pred_accessory);
	if (acc) {
		IOObjectRelease(acc);
	} else {
		io_service_t dev = find_service("IOUSBHostDevice", pred_android);
		if (!dev) {
			snprintf(err, errsz, "no Android device on the USB bus");
			return NULL;
		}
		bool hs = aoa_handshake(dev, err, errsz);
		IOObjectRelease(dev);
		if (!hs)
			return NULL;
		// Wait for re-enumeration (~10 s, as the Mac app does).
		bool switched = false;
		for (int i = 0; i < 33; i++) {
			usleep(300000);
			io_service_t s = find_service("IOUSBHostDevice", pred_accessory);
			if (s) {
				IOObjectRelease(s);
				switched = true;
				break;
			}
		}
		if (!switched) {
			snprintf(err, errsz, "phone did not switch to accessory mode — is Spancam open?");
			return NULL;
		}
	}

	usleep(500000); // let the interface publish
	io_service_t iface = find_service("IOUSBHostInterface", pred_accessory_iface);
	if (!iface) {
		snprintf(err, errsz, "accessory bulk interface not found");
		return NULL;
	}

	IOCFPlugInInterface **plugin = NULL;
	SInt32 score = 0;
	kern_return_t kr = IOCreatePlugInInterfaceForService(iface, kIOUSBInterfaceUserClientTypeID,
							     kIOCFPlugInInterfaceID, &plugin, &score);
	IOObjectRelease(iface);
	if (kr != kIOReturnSuccess || !plugin) {
		snprintf(err, errsz, "could not create the interface plugin (%d)", kr);
		return NULL;
	}
	IOUSBInterfaceInterface500 **intf = NULL;
	HRESULT res =
		(*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID *)&intf);
	IODestroyPlugInInterface(plugin);
	if (res != 0 || !intf) {
		snprintf(err, errsz, "could not query the USB interface");
		return NULL;
	}
	// A just-closed tunnel's interface can stay open until its in-flight read
	// times out, so a reconnect races the release. Retry ~6 s to outlast it.
	kern_return_t open_kr = (*intf)->USBInterfaceOpen(intf);
	for (int i = 0; open_kr == kIOReturnExclusiveAccess && i < 40; i++) {
		usleep(150000);
		open_kr = (*intf)->USBInterfaceOpen(intf);
	}
	if (open_kr != kIOReturnSuccess) {
		(*intf)->Release(intf);
		snprintf(err, errsz, "could not open the accessory interface (%d)", open_kr);
		return NULL;
	}

	UInt8 num_ep = 0;
	(*intf)->GetNumEndpoints(intf, &num_ep);
	uint8_t in_pipe = 0, out_pipe = 0;
	for (UInt8 p = 1; p <= num_ep; p++) { // ref 0 is the control pipe
		UInt8 dir = 0, num = 0, tt = 0, interval = 0;
		UInt16 max_packet = 0;
		if ((*intf)->GetPipeProperties(intf, p, &dir, &num, &tt, &max_packet, &interval) != kIOReturnSuccess)
			continue;
		if (tt != kUSBBulk)
			continue;
		if (dir == kUSBIn && in_pipe == 0)
			in_pipe = p;
		else if (dir == kUSBOut && out_pipe == 0)
			out_pipe = p;
	}
	if (in_pipe == 0 || out_pipe == 0) {
		(*intf)->USBInterfaceClose(intf);
		(*intf)->Release(intf);
		snprintf(err, errsz, "accessory has no bulk endpoints");
		return NULL;
	}

	spancam_aoa_t *a = bzalloc(sizeof(*a));
	a->intf = intf;
	a->in_pipe = in_pipe;
	a->out_pipe = out_pipe;
	obs_log(LOG_INFO, "Spancam: AOA interface open (bulk in=%u out=%u)", in_pipe, out_pipe);
	return a;
}

int spancam_aoa_read(spancam_aoa_t *a, void *buf, uint32_t *len, uint32_t timeout_ms)
{
	if (!a || !a->intf || !len)
		return -1;
	UInt32 size = *len;
	IOReturn r = (*a->intf)->ReadPipeTO(a->intf, a->in_pipe, buf, &size, timeout_ms, timeout_ms);
	*len = (uint32_t)size;
	if (r == kIOReturnSuccess)
		return 1;
	// A timeout with no data is the normal quiet gap between frames; anything
	// else means the cable or the phone went away.
	if (r == kIOUSBTransactionTimeout || r == kIOReturnTimeout)
		return 0;
	return -1;
}

bool spancam_aoa_write(spancam_aoa_t *a, const void *buf, uint32_t len, uint32_t timeout_ms)
{
	if (!a || !a->intf)
		return false;
	// WritePipeTO takes a non-const buffer but does not modify it on an OUT write.
	return (*a->intf)->WritePipeTO(a->intf, a->out_pipe, (void *)(uintptr_t)buf, len, timeout_ms, timeout_ms) ==
	       kIOReturnSuccess;
}

void spancam_aoa_close(spancam_aoa_t *a)
{
	if (!a)
		return;
	if (a->intf) {
		(*a->intf)->USBInterfaceClose(a->intf);
		(*a->intf)->Release(a->intf);
	}
	bfree(a);
}

#endif // __APPLE__
