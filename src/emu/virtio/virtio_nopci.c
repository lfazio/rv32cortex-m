/* SPDX-License-Identifier: Apache-2.0 */
/*
 * virtio_nopci.c - the PCI transport, refused.
 *
 * TinyEMU's virtio.c implements two transports and this tree uses one.
 * **MMIO is the only one that can work here**: the guests are rv32 and
 * G4MH machines whose device trees place virtio at fixed addresses, and
 * there is no PCI host bridge for the other to sit on. Writing one is a
 * far larger job than the transport it would serve.
 *
 * The eight functions below are what the unused half references. They
 * exist because the linker wants them, and they **abort** rather than
 * return something plausible.
 *
 * That is the whole point of the file. A stub returning NULL or zero
 * would let `virtio_pci_init` appear to succeed and hand back a device
 * that never answers -- which presents as a guest whose driver probes
 * and finds nothing, three layers away from the cause. This tree has
 * recorded that shape often enough: a capability that depends on nobody
 * exercising it is not a capability, and the failure has to name the
 * mechanism. Reaching any of these means a caller asked for the PCI
 * transport, and the message says exactly that.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pci.h"

static void refuse(const char *what)
{
    fprintf(stderr,
            "virtio: %s was called, which means something asked for the PCI\n"
            "        transport. This build has MMIO only -- there is no PCI\n"
            "        host bridge -- so the device could never have answered.\n",
            what);
    abort();
}

PCIDevice *pci_register_device(PCIBus *b, const char *name, int devfn,
                               uint16_t vendor_id, uint16_t device_id,
                               uint8_t revision, uint16_t class_id)
{
    (void)b;
    (void)name;
    (void)devfn;
    (void)vendor_id;
    (void)device_id;
    (void)revision;
    (void)class_id;
    refuse("pci_register_device");
    return NULL;
}

void pci_register_bar(PCIDevice *d, unsigned int bar_num, uint32_t size,
                      int type, void *opaque, PCIBarSetFunc *bar_set)
{
    (void)d;
    (void)bar_num;
    (void)size;
    (void)type;
    (void)opaque;
    (void)bar_set;
    refuse("pci_register_bar");
}

int pci_add_capability(PCIDevice *d, const uint8_t *buf, int size)
{
    (void)d;
    (void)buf;
    (void)size;
    refuse("pci_add_capability");
    return -1;
}

IRQSignal *pci_device_get_irq(PCIDevice *d, unsigned int irq_num)
{
    (void)d;
    (void)irq_num;
    refuse("pci_device_get_irq");
    return NULL;
}

uint8_t *pci_device_get_dma_ptr(PCIDevice *d, uint64_t addr, BOOL is_rw)
{
    (void)d;
    (void)addr;
    (void)is_rw;
    refuse("pci_device_get_dma_ptr");
    return NULL;
}

PhysMemoryMap *pci_device_get_mem_map(PCIDevice *d)
{
    (void)d;
    refuse("pci_device_get_mem_map");
    return NULL;
}

void pci_device_set_config8(PCIDevice *d, uint8_t addr, uint8_t val)
{
    (void)d;
    (void)addr;
    (void)val;
    refuse("pci_device_set_config8");
}

void pci_device_set_config16(PCIDevice *d, uint8_t addr, uint16_t val)
{
    (void)d;
    (void)addr;
    (void)val;
    refuse("pci_device_set_config16");
}
