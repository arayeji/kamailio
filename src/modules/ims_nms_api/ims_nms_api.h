/*
 * IMS NMS HTTP API - shared definitions
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _IMS_NMS_API_H_
#define _IMS_NMS_API_H_

#include "../../core/str.h"
#include "../../core/parser/msg_parser.h"
#include "../../core/utils/srjson.h"

#define IMS_NMS_PROFILE "nms_imsi"
#define IMS_NMS_BUF_SIZE 65536
#define IMS_NMS_LIVE_MAX 50

typedef struct ims_nms_cfg
{
	str api_token;
	str api_host;
	str plmn_realm;
	str cscf_role;
	str api_listen_ip;
	int api_listen_port;
} ims_nms_cfg_t;

extern ims_nms_cfg_t ims_nms_cfg;

int ims_nms_check_auth(sip_msg_t *msg);
int ims_nms_dispatch(sip_msg_t *msg);

int ims_nms_core_init(void);
int ims_nms_handle_registration(
		srjson_doc_t *doc, char *imsi, int imsi_len, srjson_t **out_root);
int ims_nms_handle_active_calls(
		srjson_doc_t *doc, char *imsi, int imsi_len, srjson_t **out_root);
int ims_nms_handle_live(
		srjson_doc_t *doc, char *imsis_csv, int csv_len, srjson_t **out_root);
int ims_nms_handle_stats(srjson_doc_t *doc, char *role, srjson_t **out_root);
int ims_nms_terminate_imsi_calls(char *imsi, int imsi_len);

void ims_nms_iso_utc(time_t ts, char *buf, int len);
int ims_nms_build_impu(str *impu, char *user, int user_len);

#endif /* _IMS_NMS_API_H_ */
