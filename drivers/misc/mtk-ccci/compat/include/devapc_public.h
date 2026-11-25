/* SPDX-License-Identifier: GPL-2.0 */
/* compat stub for the vendor <devapc_public.h>; see mtk_ccci_compat.h */
#ifndef __MTK_CCCI_DEVAPC_H
#define __MTK_CCCI_DEVAPC_H
#include <linux/list.h>
enum infra_subsys_id {
	INFRA_SUBSYS_MD = 0,
	INFRA_SUBSYS_CONN,
	INFRA_SUBSYS_ADSP,
	INFRA_SUBSYS_GCE,
};
enum devapc_cb_status {
	DEVAPC_OK = 0,
	DEVAPC_NOT_KE,
};
struct devapc_vio_callbacks {
	struct list_head list;
	enum infra_subsys_id id;
	void (*debug_dump)(void);
	enum devapc_cb_status (*debug_dump_adv)(uint32_t vio_addr);
};
/* No devapc driver here; md_sys1_platform.c has a weak fallback. */
void register_devapc_vio_callback(struct devapc_vio_callbacks *viocb);
#endif
