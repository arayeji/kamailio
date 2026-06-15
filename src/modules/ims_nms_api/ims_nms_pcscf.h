/*
 * IMS NMS HTTP API - P-CSCF usrloc bindings
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _IMS_NMS_PCSCF_H_
#define _IMS_NMS_PCSCF_H_

#include "../../core/utils/srjson.h"

int nms_pcscf_init(void);
int nms_pcscf_is_loaded(void);
int nms_fill_pcscf_registration(
		srjson_doc_t *doc, srjson_t *role, char *imsi, int imsi_len);
int nms_pcscf_contacts_count(void);

#endif /* _IMS_NMS_PCSCF_H_ */
