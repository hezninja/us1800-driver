/*
 * dump_descriptors.c - print the TASCAM US-1800 USB configuration descriptor (raw + decoded).
 *
 * Build: clang -O1 -o build/dump_descriptors tools/dump_descriptors.c -framework IOKit -framework CoreFoundation
 */
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdint.h>

#define VID 0x0644
#define PID 0x8030

static void hexdump(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) printf("%02x%s", p[i], (i % 16 == 15 || i == n - 1) ? "\n" : " ");
}

int main(void)
{
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    SInt32 v = VID, p = PID;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!svc) { fprintf(stderr, "US-1800 not found\n"); return 1; }

    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (!plug) { fprintf(stderr, "no plugin\n"); return 1; }
    IOUSBDeviceInterface300 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID300), (LPVOID *)&dev);
    (*plug)->Release(plug);

    IOUSBDeviceDescriptor *dd;
    UInt8 nconf = 0;
    (*dev)->GetNumberOfConfigurations(dev, &nconf);
    printf("configurations: %d\n", nconf);

    for (UInt8 c = 0; c < nconf; c++) {
        IOUSBConfigurationDescriptorPtr cd = NULL;
        (*dev)->GetConfigurationDescriptorPtr(dev, c, &cd);
        if (!cd) continue;
        int total = CFSwapInt16LittleToHost(cd->wTotalLength);
        printf("\n== config %d, wTotalLength=%d\n", c, total);
        const uint8_t *b = (const uint8_t *)cd;
        hexdump(b, total);
        printf("\n-- decoded\n");
        int off = 0;
        while (off + 2 <= total) {
            int len = b[off], type = b[off + 1];
            if (len < 2) break;
            switch (type) {
            case 0x02: printf("CONFIG  nIf=%d cfgVal=%d attr=0x%02x maxPower=%dmA\n", b[off+4], b[off+5], b[off+7], b[off+8]*2); break;
            case 0x04: printf("IFACE   #%d alt=%d nEp=%d class=0x%02x sub=0x%02x proto=0x%02x\n", b[off+2], b[off+3], b[off+4], b[off+5], b[off+6], b[off+7]); break;
            case 0x05: printf("  EP    0x%02x attr=0x%02x maxPkt=%d interval=%d%s\n", b[off+2], b[off+3],
                              b[off+4] | (b[off+5] << 8), b[off+6], len > 7 ? " (audio ep: +sync fields)" : ""); break;
            case 0x24: printf("  CS_IF len=%d sub=0x%02x: ", len, b[off+2]); hexdump(b + off, len); break;
            case 0x25: printf("  CS_EP len=%d sub=0x%02x: ", len, b[off+2]); hexdump(b + off, len); break;
            default:   printf("  type=0x%02x len=%d: ", type, len); hexdump(b + off, len); break;
            }
            off += len;
        }
    }
    (void)dd;
    (*dev)->Release(dev);
    return 0;
}
