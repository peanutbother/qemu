/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 *
 * Rasperry Pi 2 emulation and refactoring Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/arm/bcm2836.h"
#include "hw/arm/raspi_platform.h"
#include "hw/arm/raspi4_platform.h"
#include "hw/sysbus.h"
#include "trace.h"

struct BCM283XClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
    const char *name;
    const char *cpu_type;
    unsigned core_count;
    hwaddr peri_base; /* Peripheral base address seen by the CPU */
    hwaddr ctrl_base; /* Interrupt controller and mailboxes etc. */
    hwaddr gic_base;
    int clusterid;
};

static Property bcm2836_enabled_cores_property =
    DEFINE_PROP_UINT32("enabled-cpus", BCM283XState, enabled_cpus, 0);

static void bcm2836_init(Object *obj)
{
    BCM283XState *s = BCM283X(obj);
    BCM283XClass *bc = BCM283X_GET_CLASS(obj);
    int n;

    for (n = 0; n < bc->core_count; n++) {
        object_initialize_child(obj, "cpu[*]", &s->cpu[n].core,
                                bc->cpu_type);
    }
    if (bc->core_count > 1) {
        qdev_property_add_static(DEVICE(obj), &bcm2836_enabled_cores_property);
        qdev_prop_set_uint32(DEVICE(obj), "enabled-cpus", bc->core_count);
    }
    if (bc->gic_base) {
        object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    }

    if (bc->ctrl_base) {
        object_initialize_child(obj, "control", &s->control,
                                TYPE_BCM2836_CONTROL);
    }

    object_initialize_child(obj, "peripherals", &s->peripherals,
                            TYPE_BCM2835_PERIPHERALS);
    object_property_add_alias(obj, "board-rev", OBJECT(&s->peripherals),
                              "board-rev");
    object_property_add_alias(obj, "vcram-size", OBJECT(&s->peripherals),
                              "vcram-size");
}

static bool bcm283x_common_realize(DeviceState *dev, Error **errp)
{
    BCM283XState *s = BCM283X(dev);
    BCM283XClass *bc = BCM283X_GET_CLASS(dev);
    Object *obj;

    /* common peripherals from bcm2835 */

    obj = object_property_get_link(OBJECT(dev), "ram", &error_abort);

    object_property_add_const_link(OBJECT(&s->peripherals), "ram", obj);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->peripherals), errp)) {
        return false;
    }

    object_property_add_alias(OBJECT(s), "sd-bus", OBJECT(&s->peripherals),
                              "sd-bus");

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->peripherals), 0,
                            bc->peri_base, 1);
    return true;
}

static void bcm2835_realize(DeviceState *dev, Error **errp)
{
    BCM283XState *s = BCM283X(dev);

    if (!bcm283x_common_realize(dev, errp)) {
        return;
    }

    if (!qdev_realize(DEVICE(&s->cpu[0].core), NULL, errp)) {
        return;
    }

    /* Connect irq/fiq outputs from the interrupt controller. */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 0,
            qdev_get_gpio_in(DEVICE(&s->cpu[0].core), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 1,
            qdev_get_gpio_in(DEVICE(&s->cpu[0].core), ARM_CPU_FIQ));
}

static void bcm2836_realize(DeviceState *dev, Error **errp)
{
    BCM283XState *s = BCM283X(dev);
    BCM283XClass *bc = BCM283X_GET_CLASS(dev);
    int n;

    if (!bcm283x_common_realize(dev, errp)) {
        return;
    }

    /* bcm2836 interrupt controller (and mailboxes, etc.) */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->control), errp)) {
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->control), 0, bc->ctrl_base);

    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 0,
        qdev_get_gpio_in_named(DEVICE(&s->control), "gpu-irq", 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 1,
        qdev_get_gpio_in_named(DEVICE(&s->control), "gpu-fiq", 0));

    for (n = 0; n < BCM283X_NCPUS; n++) {
        /* TODO: this should be converted to a property of ARM_CPU */
        s->cpu[n].core.mp_affinity = (bc->clusterid << 8) | n;

        /* set periphbase/CBAR value for CPU-local registers */
        if (!object_property_set_int(OBJECT(&s->cpu[n].core), "reset-cbar",
                                     bc->peri_base, errp)) {
            return;
        }

        /* start powered off if not enabled */
        if (!object_property_set_bool(OBJECT(&s->cpu[n].core),
                                      "start-powered-off",
                                      n >= s->enabled_cpus,
                                      errp)) {
            return;
        }

        if (!qdev_realize(DEVICE(&s->cpu[n].core), NULL, errp)) {
            return;
        }

        /* Connect irq/fiq outputs from the interrupt controller. */
        qdev_connect_gpio_out_named(DEVICE(&s->control), "irq", n,
                qdev_get_gpio_in(DEVICE(&s->cpu[n].core), ARM_CPU_IRQ));
        qdev_connect_gpio_out_named(DEVICE(&s->control), "fiq", n,
                qdev_get_gpio_in(DEVICE(&s->cpu[n].core), ARM_CPU_FIQ));

        /* Connect timers from the CPU to the interrupt controller */
        qdev_connect_gpio_out(DEVICE(&s->cpu[n].core), GTIMER_PHYS,
                qdev_get_gpio_in_named(DEVICE(&s->control), "cntpnsirq", n));
        qdev_connect_gpio_out(DEVICE(&s->cpu[n].core), GTIMER_VIRT,
                qdev_get_gpio_in_named(DEVICE(&s->control), "cntvirq", n));
        qdev_connect_gpio_out(DEVICE(&s->cpu[n].core), GTIMER_HYP,
                qdev_get_gpio_in_named(DEVICE(&s->control), "cnthpirq", n));
        qdev_connect_gpio_out(DEVICE(&s->cpu[n].core), GTIMER_SEC,
                qdev_get_gpio_in_named(DEVICE(&s->control), "cntpsirq", n));
    }
}

static void bcm283x_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    /* Reason: Must be wired up in code (see raspi_init() function) */
    dc->user_creatable = false;
}

static void bcm2835_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    BCM283XClass *bc = BCM283X_CLASS(oc);

    bc->cpu_type = ARM_CPU_TYPE_NAME("arm1176");
    bc->core_count = 1;
    bc->peri_base = 0x20000000;
    dc->realize = bcm2835_realize;
};

static void bcm2836_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    BCM283XClass *bc = BCM283X_CLASS(oc);

    bc->cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    bc->core_count = BCM283X_NCPUS;
    bc->peri_base = 0x3f000000;
    bc->ctrl_base = 0x40000000;
    bc->clusterid = 0xf;
    dc->realize = bcm2836_realize;
};



#ifdef TARGET_AARCH64


#define GIC400_MAINTAINANCE_IRQ      9
#define GIC400_TIMER_NS_EL2_IRQ     10
#define GIC400_TIMER_VIRT_IRQ       11
#define GIC400_LEGACY_FIQ           12
#define GIC400_TIMER_S_EL1_IRQ      13
#define GIC400_TIMER_NS_EL1_IRQ     14
#define GIC400_LEGACY_IRQ           15

/* Number of external interrupt lines to configure the GIC with */
#define GIC_NUM_IRQS                128

#define PPI(cpu, irq) (GIC_NUM_IRQS + (cpu) * GIC_INTERNAL + GIC_NR_SGIS + irq)

#define GIC_BASE_OFS                0x0000
#define GIC_DIST_OFS                0x1000
#define GIC_CPU_OFS                 0x2000
#define GIC_VIFACE_THIS_OFS         0x4000
#define GIC_VIFACE_OTHER_OFS(cpu)  (0x5000 + (cpu) * 0x200)
#define GIC_VCPU_OFS                0x6000

#define VIRTUAL_PMU_IRQ 7

static void bcm2838_gic_set_irq(void *opaque, int irq, int level)
{
    BCM283XState *s = (BCM283XState *)opaque;

    trace_bcm2838_gic_set_irq(irq, level);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->gic), irq), level);
}

static void bcm2838_realize(DeviceState *dev, Error **errp)
{
    BCM283XState *s = BCM283X(dev);
    BCM283XClass *bc = BCM283X_GET_CLASS(dev);
    int n;

    if (!bcm283x_common_realize(dev, errp)) {
        return;
    }

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->peripherals), 0,
                            bc->peri_base, 1);

    /* bcm2836 interrupt controller (and mailboxes, etc.) */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->control), errp)) {
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->control), 0, bc->ctrl_base);

    /* Create cores */
    for (n = 0; n < bc->core_count; n++) {
        /* TODO: this should be converted to a property of ARM_CPU */
        s->cpu[n].core.mp_affinity = (bc->clusterid << 8) | n;

        /* set periphbase/CBAR value for CPU-local registers */
        if (!object_property_set_int(OBJECT(&s->cpu[n].core), "reset-cbar",
                                     bc->peri_base, errp)) {
            return;
        }

        /* start powered off if not enabled */
        if (!object_property_set_bool(OBJECT(&s->cpu[n].core),
                                      "start-powered-off",
                                      n >= s->enabled_cpus,
                                      errp)) {
            return;
        }

        if (!qdev_realize(DEVICE(&s->cpu[n].core), NULL, errp)) {
            return;
        }
    }

    if (!object_property_set_uint(OBJECT(&s->gic), "revision", 2, errp)) {
        return;
    }

    if (!object_property_set_uint(OBJECT(&s->gic), "num-cpu", BCM283X_NCPUS,
                                  errp)) {
        return;
    }

    if (!object_property_set_uint(OBJECT(&s->gic),
                                  "num-irq", GIC_NUM_IRQS + GIC_INTERNAL,
                                  errp)) {
        return;
    }

    if (!object_property_set_bool(OBJECT(&s->gic),
                                  "has-virtualization-extensions",
                                  true, errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gic), errp)) {
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 0,
                    bc->ctrl_base + bc->gic_base + GIC_DIST_OFS);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 1,
                    bc->ctrl_base + bc->gic_base + GIC_CPU_OFS);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 2,
                    bc->ctrl_base + bc->gic_base + GIC_VIFACE_THIS_OFS);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 3,
                    bc->ctrl_base + bc->gic_base + GIC_VCPU_OFS);

    for (n = 0; n < BCM283X_NCPUS; n++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->gic), 4 + n,
                        bc->ctrl_base + bc->gic_base + GIC_VIFACE_OTHER_OFS(n));
    }

    for (n = 0; n < BCM283X_NCPUS; n++) {
        DeviceState *cpudev = DEVICE(&s->cpu[n]);
        DeviceState *gicdev = DEVICE(&s->gic);

        /* Connect the GICv2 outputs to the CPU */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), n,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), n + BCM283X_NCPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), n + 2 * BCM283X_NCPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), n + 3 * BCM283X_NCPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), n + 4 * BCM283X_NCPUS,
                           qdev_get_gpio_in(gicdev,
                                           PPI(n, GIC400_MAINTAINANCE_IRQ)));

        /* Connect UART0 to the interrupt controller */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals.uart0), 0, 
                           qdev_get_gpio_in(gicdev, RPI4_INTERRUPT_UART0));

        /* Connect timers from the CPU to the interrupt controller */
        qdev_connect_gpio_out(cpudev, GTIMER_PHYS,
                    qdev_get_gpio_in(gicdev, PPI(n, GIC400_TIMER_NS_EL1_IRQ)));
        qdev_connect_gpio_out(cpudev, GTIMER_VIRT,
                    qdev_get_gpio_in(gicdev, PPI(n, GIC400_TIMER_VIRT_IRQ)));
        qdev_connect_gpio_out(cpudev, GTIMER_HYP,
                    qdev_get_gpio_in(gicdev, PPI(n, GIC400_TIMER_NS_EL2_IRQ)));
        qdev_connect_gpio_out(cpudev, GTIMER_SEC,
                    qdev_get_gpio_in(gicdev, PPI(n, GIC400_TIMER_S_EL1_IRQ)));
        /* PMU interrupt */
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                    qdev_get_gpio_in(gicdev, PPI(n, VIRTUAL_PMU_IRQ)));
    }

    /* Pass through inbound GPIO lines to the GIC */
    qdev_init_gpio_in(dev, bcm2838_gic_set_irq, GIC_NUM_IRQS);

    /* Pass through outbound IRQ lines from the GIC */
    qdev_pass_gpios(DEVICE(&s->gic), DEVICE(&s->peripherals), NULL);
}

static void bcm2837_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    BCM283XClass *bc = BCM283X_CLASS(oc);

    bc->cpu_type = ARM_CPU_TYPE_NAME("cortex-a53");
    bc->core_count = BCM283X_NCPUS;
    bc->peri_base = 0x3f000000;
    bc->ctrl_base = 0x40000000;
    bc->clusterid = 0x0;
    dc->realize = bcm2836_realize;
};

static void bcm2838_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    BCM283XClass *bc = BCM283X_CLASS(oc);

    bc->cpu_type = ARM_CPU_TYPE_NAME("cortex-a72");
    bc->core_count = BCM283X_NCPUS;
    bc->peri_base = 0xfe000000;
    bc->ctrl_base = 0xff800000;
    bc->gic_base = 0x40000;
    bc->clusterid = 0x0;
    dc->realize = bcm2838_realize;
};
#endif

static const TypeInfo bcm283x_types[] = {
    {
        .name           = TYPE_BCM2835,
        .parent         = TYPE_BCM283X,
        .class_init     = bcm2835_class_init,
    }, {
        .name           = TYPE_BCM2836,
        .parent         = TYPE_BCM283X,
        .class_init     = bcm2836_class_init,
#ifdef TARGET_AARCH64
    }, {
        .name           = TYPE_BCM2837,
        .parent         = TYPE_BCM283X,
        .class_init     = bcm2837_class_init,
    }, {
        .name           = TYPE_BCM2838,
        .parent         = TYPE_BCM283X,
        .class_init     = bcm2838_class_init,
#endif
    }, {
        .name           = TYPE_BCM283X,
        .parent         = TYPE_DEVICE,
        .instance_size  = sizeof(BCM283XState),
        .instance_init  = bcm2836_init,
        .class_size     = sizeof(BCM283XClass),
        .class_init     = bcm283x_class_init,
        .abstract       = true,
    }
};

DEFINE_TYPES(bcm283x_types)
