/*
 * QEMU ARM Xen PV Machine
 *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/qapi-commands-migration.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "sysemu/block-backend.h"
#include "sysemu/tpm_backend.h"
#include "sysemu/sysemu.h"
#include "hw/xen/xen-legacy-backend.h"
#include "hw/xen/xen-hvm-common.h"
#include "sysemu/tpm.h"
#include "hw/xen/arch_hvm.h"
#include "exec/address-spaces.h"

#define TYPE_XEN_ARM  MACHINE_TYPE_NAME("xenpv")
OBJECT_DECLARE_SIMPLE_TYPE(XenArmState, XEN_ARM)

static MemoryListener xen_memory_listener = {
    .region_add = xen_region_add,
    .region_del = xen_region_del,
    .log_start = NULL,
    .log_stop = NULL,
    .log_sync = NULL,
    .log_global_start = NULL,
    .log_global_stop = NULL,
    .priority = 10,
};

struct XenArmState {
    /*< private >*/
    MachineState parent;

    XenIOState *state;
    const MemMapEntry *memmap;
    const int *irqmap;
};

static MemoryRegion ram_lo, ram_hi;

#define VIRTIO_MMIO_DEV_SIZE   0x200

#define VIRTIO_MMIO_IDX   0

#define NR_VIRTIO_MMIO_DEVICES   \
   (GUEST_VIRTIO_MMIO_SPI_LAST - GUEST_VIRTIO_MMIO_SPI_FIRST)

static const MemMapEntry xen_memmap[] = {
    [VIRTIO_MMIO_IDX] = { GUEST_VIRTIO_MMIO_BASE, VIRTIO_MMIO_DEV_SIZE },
};

static const int xen_irqmap[] = {
    [VIRTIO_MMIO_IDX] = GUEST_VIRTIO_MMIO_SPI_FIRST, /* ...to GUEST_VIRTIO_MMIO_SPI_LAST - 1 */
};

static void xen_set_irq(void *opaque, int irq, int level)
{
    xendevicemodel_set_irq_level(xen_dmod, xen_domid, irq, level);
}

static void xen_create_virtio_mmio_devices(XenArmState *xam)
{
    hwaddr size = xam->memmap[VIRTIO_MMIO_IDX].size;
    int i;

    for (i = 0; i < NR_VIRTIO_MMIO_DEVICES; i++) {
        hwaddr base = xam->memmap[VIRTIO_MMIO_IDX].base + i * size;
        qemu_irq irq = qemu_allocate_irq(xen_set_irq, NULL,
                                         xam->irqmap[VIRTIO_MMIO_IDX] + i);

        sysbus_create_simple("virtio-mmio", base, irq);

        DPRINTF("Created virtio-mmio device %d: irq %d base 0x%lx\n",
                i, xam->irqmap[VIRTIO_MMIO_IDX] + i, base);
    }
}

static void xen_init_ram(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    ram_addr_t block_len, ram_size[GUEST_RAM_BANKS];

    if (machine->ram_size <= GUEST_RAM0_SIZE) {
        ram_size[0] = machine->ram_size;
        ram_size[1] = 0;
        block_len = GUEST_RAM0_BASE + ram_size[0];
    } else {
        ram_size[0] = GUEST_RAM0_SIZE;
        ram_size[1] = machine->ram_size - GUEST_RAM0_SIZE;
        block_len = GUEST_RAM1_BASE + ram_size[1];
    }

    memory_region_init_ram(&ram_memory, NULL, "xen.ram", block_len, &error_fatal);

    memory_region_init_alias(&ram_lo, NULL, "xen.ram.lo", &ram_memory,
                             GUEST_RAM0_BASE, ram_size[0]);
    memory_region_add_subregion(sysmem, GUEST_RAM0_BASE, &ram_lo);
    DPRINTF("Initialized region xen.ram.lo: base 0x%llx size 0x%lx\n",
            GUEST_RAM0_BASE, ram_size[0]);

    if (ram_size[1] > 0) {
        memory_region_init_alias(&ram_hi, NULL, "xen.ram.hi", &ram_memory,
                                 GUEST_RAM1_BASE, ram_size[1]);
        memory_region_add_subregion(sysmem, GUEST_RAM1_BASE, &ram_hi);
        DPRINTF("Initialized region xen.ram.hi: base 0x%llx size 0x%lx\n",
                GUEST_RAM1_BASE, ram_size[1]);
    }
}

void arch_handle_ioreq(XenIOState *state, ioreq_t *req)
{
    hw_error("Invalid ioreq type 0x%x\n", req->type);

    return;
}

void arch_xen_set_memory(XenIOState *state,MemoryRegionSection *section,
                         bool add)
{
}

void xen_hvm_modified_memory(ram_addr_t start, ram_addr_t length)
{
}

void qmp_xen_set_global_dirty_log(bool enable, Error **errp)
{
}

static int xen_init_ioreq(XenIOState *state, unsigned int max_cpus)
{
    xen_dmod = xendevicemodel_open(0, 0);
    xen_xc = xc_interface_open(0, 0, 0);

    if (xen_xc == NULL) {
        perror("xen: can't open xen interface\n");
        return -1;
    }

    xen_fmem = xenforeignmemory_open(0, 0);
    if (xen_fmem == NULL) {
        perror("xen: can't open xen fmem interface\n");
        xc_interface_close(xen_xc);
        return -1;
    }

    xen_register_ioreq(state, max_cpus, xen_memory_listener);

    xenstore_record_dm_state(xenstore, "running");

    return 0;
}


static void xen_arm_init(MachineState *machine)
{
    DeviceState *dev;
    SysBusDevice *busdev;
    Error *errp = NULL;
    XenArmState *xam = XEN_ARM(machine);

    if (machine->ram_size == 0) {
        error_report("xenpv: ram_size must be specified");
        exit(1);
    }

    xam->state =  g_new0(XenIOState, 1);
    xam->memmap = xen_memmap;
    xam->irqmap = xen_irqmap;

    if (xen_init_ioreq(xam->state, machine->smp.cpus)) {
        return;
    }

    xen_init_ram(machine);
    xen_create_virtio_mmio_devices(xam);

    TPMBackend *be = qemu_find_tpm_be("tpm0");
    if (be == NULL) {
        DPRINTF("Couldn't fine the backend for tpm0\n");
        return;
    }

    dev = qdev_new(TYPE_TPM_TIS_SYSBUS);
    object_property_set_link(OBJECT(dev), "tpmdev", OBJECT(be), &errp);
    object_property_set_str(OBJECT(dev), "tpmdev", be->id, &errp);
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(busdev, &error_fatal);
    sysbus_mmio_map(busdev, 0, GUEST_TPM_BASE);

    DPRINTF("Connected tpmdev at address 0x%llx\n", GUEST_TPM_BASE);

    return;
}

static void xen_arm_machine_class_init(ObjectClass *oc, void *data)
{

    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Xen Para-virtualized PC";
    mc->init = xen_arm_init;
    mc->max_cpus = GUEST_MAX_VCPUS;
    /* Set explicitly here to make sure that real ram_size is passed */
    mc->default_ram_size = 0;
    machine_class_allow_dynamic_sysbus_dev(mc, TYPE_TPM_TIS_SYSBUS);
}

static const TypeInfo xen_arm_machine_type = {
    .name = TYPE_XEN_ARM,
    .parent = TYPE_MACHINE,
    .class_init = xen_arm_machine_class_init,
    .instance_size = sizeof(XenArmState),
};

static void xen_arm_machine_register_types(void)
{
    type_register_static(&xen_arm_machine_type);
}

type_init(xen_arm_machine_register_types)
