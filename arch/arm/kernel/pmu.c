/*
 *  linux/arch/arm/kernel/pmu.c
 *
 *  Copyright (C) 2009 picoChip Designs Ltd, Jamie Iles
 *  Copyright (C) 2010 ARM Ltd, Will Deacon
 *  Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define pr_fmt(fmt) "PMU: " fmt

#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <asm/pmu.h>

static volatile long pmu_lock;

static struct platform_device *pmu_devices[ARM_NUM_PMU_DEVICES];

static int __devinit pmu_device_probe(struct platform_device *pdev)
{

	if (pdev->id < 0 || pdev->id >= ARM_NUM_PMU_DEVICES) {
		pr_warning("received registration request for unknown "
				"device %d\n", pdev->id);
		return -EINVAL;
	}

	if (pmu_devices[pdev->id])
		pr_warning("registering new PMU device type %d overwrites "
				"previous registration!\n", pdev->id);
	else
		pr_info("registered new PMU device of type %d\n",
				pdev->id);

	pmu_devices[pdev->id] = pdev;
	return 0;
}

static struct platform_driver cpu_pmu_driver = {
	.driver		= {
		.name	= "cpu-arm-pmu",
	},
	.probe		= pmu_device_probe,
};

static struct platform_driver l2_pmu_driver = {
	.driver		= {
		.name	= "l2-arm-pmu",
	},
	.probe		= pmu_device_probe,
};

static struct platform_driver *pmu_drivers[] __initdata = {
	&cpu_pmu_driver,
	&l2_pmu_driver,
};

static int __init register_pmu_driver(void)
{
	int i;
	int err;

	for (i = 0; i < ARM_NUM_PMU_DEVICES; i++) {
		err = platform_driver_register(pmu_drivers[i]);
		if (err) {
			pr_err("%s: failed to register id:%d\n", __func__, i);
			while (--i >= 0)
				platform_driver_unregister(pmu_drivers[i]);
			break;
		}
	}

	return err;
}
device_initcall(register_pmu_driver);

struct platform_device *
reserve_pmu(enum arm_pmu_type device)
{
	struct platform_device *pdev;

	if (test_and_set_bit_lock(device, &pmu_lock)) {
		pdev = ERR_PTR(-EBUSY);
	} else if (pmu_devices[device] == NULL) {
		clear_bit_unlock(device, &pmu_lock);
		pdev = ERR_PTR(-ENODEV);
	} else {
		pdev = pmu_devices[device];
	}

	return pdev;
}
EXPORT_SYMBOL_GPL(reserve_pmu);

int
release_pmu(struct platform_device *pdev)
{
	if (WARN_ON(pdev != pmu_devices[pdev->id]))
		return -EINVAL;
	clear_bit_unlock(pdev->id, &pmu_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(release_pmu);

static int
set_irq_affinity(int irq,
		 unsigned int cpu)
{
#ifdef CONFIG_SMP
	int err = irq_set_affinity(irq, cpumask_of(cpu));
	if (err)
		pr_warning("unable to set irq affinity (irq=%d, cpu=%u)\n",
			   irq, cpu);
	return err;
#else
	return 0;
#endif
}

static int
init_cpu_pmu(void)
{
	int i, err = 0;
	struct platform_device *pdev = pmu_devices[ARM_PMU_DEVICE_CPU];

	if (!pdev) {
		err = -ENODEV;
		goto out;
	}

	for (i = 0; i < pdev->num_resources; ++i) {
		err = set_irq_affinity(platform_get_irq(pdev, i), i);
		if (err)
			break;
	}

out:
	return err;
}

int
init_pmu(enum arm_pmu_type device)
{
	int err = 0;

	switch (device) {
	case ARM_PMU_DEVICE_CPU:
		err = init_cpu_pmu();
		break;
	default:
		pr_warning("attempt to initialise unknown device %d\n",
				device);
		err = -EINVAL;
	}

	return err;
}
EXPORT_SYMBOL_GPL(init_pmu);
