// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Yixun Lan <dlan@kernel.org>
 */

#include <dm.h>
#include <event.h>
#include <log.h>

static int k3_init_cluster_clocks(void)
{
	struct udevice *dev;
	int ret;

	/*
	 * Ensure APMU (CPU cluster C0..C3 core clocks) and DCIU
	 * (A100 cluster C2/C3 TCM pipeline clocks) are probed and enabled
	 * before handing off to S-mode payloads.
	 */
	ret = uclass_get_device_by_name(UCLASS_CLK,
					"system-controller@d4282800", &dev);
	if (ret)
		log_debug("K3 APMU clk probe returned %d\n", ret);

	ret = uclass_get_device_by_name(UCLASS_CLK,
					"system-controller@d8440000", &dev);
	if (ret)
		log_debug("K3 DCIU clk probe returned %d\n", ret);

	return 0;
}
EVENT_SPY_SIMPLE(EVT_DM_POST_INIT_R, k3_init_cluster_clocks);
