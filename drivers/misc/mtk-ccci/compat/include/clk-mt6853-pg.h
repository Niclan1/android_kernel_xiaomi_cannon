/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <clk-mt6853-pg.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_CLK_PG_H
#define __MTK_CCCI_CLK_PG_H
#include <linux/list.h>
/* MTCMOS callbacks of the vendor clock driver; MD1 is a genpd here. */
enum subsys_id {
	SYS_MD1 = 0,
	NR_SYSS
};
struct pg_callbacks {
	struct list_head list;
	void (*before_off)(enum subsys_id sys);
	void (*after_on)(enum subsys_id sys);
	void (*debug_dump)(enum subsys_id sys);
};
static inline struct pg_callbacks *register_pg_callback(struct pg_callbacks *pgcb)
{
	return NULL;
}
#endif
