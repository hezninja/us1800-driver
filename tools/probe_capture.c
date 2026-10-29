/*
 * probe_capture.c - replay the known US-1800 init sequence, then read the bulk IN pipes and hex-dump
 * what arrives. Exploratory: used to learn the capture-data format.
 *
 * Build: clang -O1 -o build/probe_capture tools/probe_capture.c -framework IOKit -framework CoreFoundation
 * Run:   ./build/probe_capture [rate] [seconds] [reg49_mode: 0=skip 1=upstream(clear 0x20,set 0x10)]
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#define VID 0x0644
#define PID 0x8030

static IOUSBDeviceInterface300 **open_device(void)
{
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    SInt32 v = VID, p = PID;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!svc) return NULL;
    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (!plug) return NULL;
    IOUSBDeviceInterface300 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID300), (LPVOID *)&dev);
    (*plug)->Release(plug);
    return dev;
}

static IOUSBInterfaceInterface300 **open_interface(IOUSBDeviceInterface300 **dev, int want)
{
    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                      kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t it;
    (*dev)->CreateInterfaceIterator(dev, &req, &it);
    io_service_t s;
    int n = 0;
    IOUSBInterfaceInterface300 **iface = NULL;
    while ((s = IOIteratorNext(it))) {
        if (n == want) {
            IOCFPlugInInterface **plug = NULL;
            SInt32 score;
            IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
            if (plug) {
                (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&iface);
                (*plug)->Release(plug);
            }
        }
        IOObjectRelease(s);
        n++;
    }
    IOObjectRelease(it);
    return iface;
}

static IOReturn vendor(IOUSBDeviceInterface300 **dev, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, void *d, uint16_t len)
{
    IOUSBDevRequest r = { rt, req, val, idx, len, d, 0 };
    return (*dev)->DeviceRequest(dev, &r);
}

static void hexdump(const uint8_t *p, int n, int max)
{
    for (int i = 0; i < n && i < max; i++) printf("%02x%s", p[i], (i % 32 == 31) ? "\n" : " ");
    printf("\n");
}

/* Find pipeRef by endpoint address. */
static int pipe_for(IOUSBInterfaceInterface300 **ifc, uint8_t addr)
{
    UInt8 n;
    (*ifc)->GetNumEndpoints(ifc, &n);
    for (UInt8 i = 1; i <= n; i++) {
        UInt8 dir, num, tt, iv; UInt16 mp;
        (*ifc)->GetPipeProperties(ifc, i, &dir, &num, &tt, &mp, &iv);
        if ((((dir == kUSBIn) ? 0x80 : 0) | num) == addr) return i;
    }
    return -1;
}

int main(int argc, char **argv)
{
    int rate = argc > 1 ? atoi(argv[1]) : 48000;
    int secs = argc > 2 ? atoi(argv[2]) : 3;
    int regmode = argc > 3 ? atoi(argv[3]) : 1;

    IOUSBDeviceInterface300 **dev = open_device();
    if (!dev) { fprintf(stderr, "device not found\n"); return 1; }
    IOReturn kr = (*dev)->USBDeviceOpen(dev);
    printf("USBDeviceOpen: 0x%x\n", kr);
    (*dev)->SetConfiguration(dev, 1);

    IOUSBInterfaceInterface300 **if0 = open_interface(dev, 0), **if1 = open_interface(dev, 1);
    if (!if0 || !if1) { fprintf(stderr, "interfaces not found\n"); return 1; }
    printf("if0 open: 0x%x, if1 open: 0x%x\n", (*if0)->USBInterfaceOpen(if0), (*if1)->USBInterfaceOpen(if1));

    uint8_t fw[2] = {0};
    printf("fw read: 0x%x -> %d.%d\n", vendor(dev, 0xC0, 0x56, 0, 0, fw, 2), fw[1], fw[0]);

    printf("alt if0: 0x%x, alt if1: 0x%x\n", (*if0)->SetAlternateInterface(if0, 1), (*if1)->SetAlternateInterface(if1, 1));

    uint8_t freq[3] = { rate & 0xff, (rate >> 8) & 0xff, (rate >> 16) & 0xff };
    printf("rate 0x83: 0x%x, rate 0x02: 0x%x\n",
           vendor(dev, 0x22, 0x01, 0x0100, 0x83, freq, 3), vendor(dev, 0x22, 0x01, 0x0100, 0x02, freq, 3));
    usleep(50000);

    uint8_t reg = 0;
    IOReturn r = vendor(dev, 0xC0, 0x49, 0, 0, &reg, 1);
    printf("reg49 read: 0x%x -> 0x%02x\n", r, reg);
    if (regmode == 1) {
        uint8_t nr = (reg & ~0x20) | 0x10;
        printf("reg49 write 0x%02x: 0x%x\n", nr, vendor(dev, 0x40, 0x49, nr, 0, NULL, 0));
    }

    struct { IOUSBInterfaceInterface300 **ifc; uint8_t addr; const char *name; } eps[] = {
        { if0, 0x83, "bulk IN 0x83 (capture?)" },
        { if1, 0x86, "bulk IN 0x86" },
    };
    for (int e = 0; e < 2; e++) {
        int pr = pipe_for(eps[e].ifc, eps[e].addr);
        printf("\n=== %s pipeRef=%d\n", eps[e].name, pr);
        if (pr < 0) continue;
        (*eps[e].ifc)->ClearPipeStallBothEnds(eps[e].ifc, pr);
        long total = 0; int reads = 0;
        for (int i = 0; i < 40 * secs; i++) {
            uint8_t buf[512];
            UInt32 sz = sizeof buf;
            IOReturn rr = (*eps[e].ifc)->ReadPipeTO(eps[e].ifc, pr, buf, &sz, 50, 100);
            if (rr == kIOReturnSuccess && sz) {
                if (reads < 6) { printf("read #%d: %u bytes\n", reads, (unsigned)sz); hexdump(buf, sz, 96); }
                total += sz; reads++;
            } else if (reads == 0 && i < 3) {
                printf("read err 0x%x\n", rr);
            }
        }
        printf("-> %d reads, %ld bytes total over ~%ds\n", reads, total, secs);
    }

    (*if0)->USBInterfaceClose(if0);
    (*if1)->USBInterfaceClose(if1);
    (*dev)->USBDeviceClose(dev);
    return 0;
}
