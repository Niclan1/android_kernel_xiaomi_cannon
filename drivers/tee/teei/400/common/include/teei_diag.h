/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Boot diagnostics for the TEEI driver: waits that keep the original
 * blocking semantics but report periodically where they are stuck.
 */

#ifndef TEEI_DIAG_H
#define TEEI_DIAG_H

#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/printk.h>

#define TEEI_DIAG_PERIOD	(10 * HZ)

static inline void teei_diag_wait(struct completion *c, const char *what,
				  unsigned long long arg)
{
	unsigned long start = jiffies;

	while (!wait_for_completion_timeout(c, TEEI_DIAG_PERIOD))
		pr_debug("teei-diag: still waiting for %s (arg=0x%llx) after %u ms\n",
		       what, arg, jiffies_to_msecs(jiffies - start));
}

static inline int teei_diag_wait_interruptible(struct completion *c,
					       const char *what)
{
	unsigned long start = jiffies;
	long ret;

	for (;;) {
		ret = wait_for_completion_interruptible_timeout(c,
							TEEI_DIAG_PERIOD);
		if (ret > 0)
			return 0;
		if (ret < 0)
			return ret;
		pr_debug("teei-diag: still waiting for %s after %u ms\n",
		       what, jiffies_to_msecs(jiffies - start));
	}
}

#endif
