/*
 * IMS NMS HTTP API - Kamailio module (native C, xhttp)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../../core/sr_module.h"
#include "../../core/kemi.h"
#include "../../core/mod_fix.h"
#include "../../core/ut.h"
#include "../../core/ip_addr.h"
#include "../../core/resolve.h"
#include "../../core/nonsip_hooks.h"
#include "../../core/mem/pkg.h"
#include "../xhttp/api.h"

#include "ims_nms_api.h"

MODULE_VERSION

ims_nms_cfg_t ims_nms_cfg;

static str api_token = str_init("");
static str api_host = str_init("cscf-example");
static str plmn_realm = str_init("ims.mnc001.mcc001.3gppnetwork.org");
static str cscf_role = str_init("scscf");
static str api_listen_ip = str_init("");
static int api_listen_port = 0;

static xhttp_api_t xhttp_api;

extern int ims_nms_core_init(void);
extern int ims_nms_handle_registration(
		srjson_doc_t *doc, char *imsi, int imsi_len, srjson_t **out_root);
extern int ims_nms_handle_active_calls(
		srjson_doc_t *doc, char *imsi, int imsi_len, srjson_t **out_root);
extern int ims_nms_handle_live(
		srjson_doc_t *doc, char *imsis_csv, int csv_len, srjson_t **out_root);
extern int ims_nms_handle_stats(srjson_doc_t *doc, char *role, srjson_t **out_root);
extern int ims_nms_terminate_imsi_calls(char *imsi, int imsi_len);

static int mod_init(void);
static int child_init(int rank);
static void mod_destroy(void);
static int w_ims_nms_dispatch(sip_msg_t *msg);

static str ctype_json = str_init("application/json");
static str reason_ok = str_init("OK");
static str reason_unauth = str_init("Unauthorized");
static str reason_forbid = str_init("Forbidden");
static str reason_nf = str_init("Not Found");

static int param_set_token(modparam_t type, void *val)
{
	api_token.s = (char *)val;
	api_token.len = strlen((char *)val);
	return 0;
}

static int param_set_host(modparam_t type, void *val)
{
	api_host.s = (char *)val;
	api_host.len = strlen((char *)val);
	return 0;
}

static int param_set_realm(modparam_t type, void *val)
{
	plmn_realm.s = (char *)val;
	plmn_realm.len = strlen((char *)val);
	return 0;
}

static int param_set_role(modparam_t type, void *val)
{
	cscf_role.s = (char *)val;
	cscf_role.len = strlen((char *)val);
	return 0;
}

static int param_set_listen_ip(modparam_t type, void *val)
{
	api_listen_ip.s = (char *)val;
	api_listen_ip.len = strlen((char *)val);
	return 0;
}

static cmd_export_t cmds[] = {
	{"ims_nms_api_dispatch", (cmd_function)w_ims_nms_dispatch, 0, 0, 0,
			REQUEST_ROUTE | EVENT_ROUTE},
	{0, 0, 0, 0, 0, 0}};

static param_export_t params[] = {
	{"api_token", PARAM_STR | PARAM_USE_FUNC, param_set_token},
	{"api_host", PARAM_STR | PARAM_USE_FUNC, param_set_host},
	{"plmn_realm", PARAM_STR | PARAM_USE_FUNC, param_set_realm},
	{"cscf_role", PARAM_STR | PARAM_USE_FUNC, param_set_role},
	{"api_listen_ip", PARAM_STR | PARAM_USE_FUNC, param_set_listen_ip},
	{"api_listen_port", PARAM_INT, &api_listen_port},
	{0, 0, 0}};

static int mod_init(void)
{
	ims_nms_cfg.api_token = api_token;
	ims_nms_cfg.api_host = api_host;
	ims_nms_cfg.plmn_realm = plmn_realm;
	ims_nms_cfg.cscf_role = cscf_role;
	ims_nms_cfg.api_listen_ip = api_listen_ip;
	ims_nms_cfg.api_listen_port = api_listen_port;

	if(xhttp_load_api(&xhttp_api) < 0) {
		LM_ERR("xhttp module required\n");
		return -1;
	}
	if(ims_nms_core_init() < 0) {
		LM_ERR("failed to init nms core\n");
		return -1;
	}
	if(ims_nms_cfg.api_token.len <= 0)
		LM_WARN("api_token not set - NMS API authentication is disabled\n");
	if(ims_nms_cfg.api_listen_ip.len <= 0 && ims_nms_cfg.api_listen_port <= 0)
		LM_WARN("api_listen_ip/port not set - NMS API accepts any xhttp "
				"listener; bind to loopback or a management interface\n");
	return 0;
}

static int child_init(int rank)
{
	(void)rank;
	return 0;
}

static void mod_destroy(void)
{
}

static int nms_on_listen_socket(sip_msg_t *msg)
{
	struct ip_addr lip;

	if(ims_nms_cfg.api_listen_port > 0
			&& msg->rcv.dst_port != ims_nms_cfg.api_listen_port)
		return 0;

	if(ims_nms_cfg.api_listen_ip.len > 0) {
		if(msg->rcv.dst_ip.af == 0)
			return 0;
		if(str2ipxbuf(&ims_nms_cfg.api_listen_ip, &lip) < 0)
			return 0;
		if(ip_addr_cmp(&msg->rcv.dst_ip, &lip) == 0)
			return 0;
	}
	return 1;
}

/* Constant-time string compare (length + bytes). */
static int nms_ct_str_eq(str *a, str *b)
{
	unsigned char diff = 0;
	int i;
	int max;

	if(!a || !b || !a->s || !b->s)
		return 0;
	diff |= (unsigned char)(a->len != b->len);
	max = a->len > b->len ? a->len : b->len;
	for(i = 0; i < max; i++) {
		unsigned char av = i < a->len ? (unsigned char)a->s[i] : 0;
		unsigned char bv = i < b->len ? (unsigned char)b->s[i] : 0;
		diff |= av ^ bv;
	}
	return diff == 0;
}

int ims_nms_check_auth(sip_msg_t *msg)
{
	struct hdr_field *hf;
	char *p;
	char *end;
	str token;

	if(ims_nms_cfg.api_token.len <= 0)
		return 0;

	for(hf = msg->headers; hf; hf = hf->next) {
		if(hf->name.len == 13
				&& strncasecmp(hf->name.s, "Authorization", 13) == 0) {
			if(hf->body.len < 7)
				return -1;
			p = hf->body.s;
			end = hf->body.s + hf->body.len;
			if(strncasecmp(p, "Bearer ", 7) != 0)
				return -1;
			p += 7;
			while(p < end && (*p == ' ' || *p == '\t'))
				p++;
			token.s = p;
			token.len = end - p;
			if(!nms_ct_str_eq(&token, &ims_nms_cfg.api_token))
				return -2;
			return 0;
		}
	}
	return -1;
}

static void nms_send_json(
		sip_msg_t *msg, int code, str *reason, char *body, int blen)
{
	str b;
	b.s = body;
	b.len = blen;
	xhttp_api.reply(msg, code, reason, &ctype_json, &b);
}

static int nms_copy_path(str *uri, char *path, int path_size, char **query)
{
	int len;
	char *q;

	if(uri->len >= path_size)
		return -1;
	memcpy(path, uri->s, uri->len);
	path[uri->len] = '\0';
	len = uri->len;
	q = strchr(path, '?');
	if(q) {
		*q = '\0';
		len = q - path;
		*query = q + 1;
	} else {
		*query = NULL;
	}
	return len;
}

static int nms_reply_json_msg(
		sip_msg_t *msg, srjson_doc_t *doc, srjson_t *root, int code)
{
	char *out;
	int ret = 0;

	out = srjson_PrintUnformatted(doc, root);
	if(!out) {
		ret = -1;
		goto done;
	}
	nms_send_json(msg, code, &reason_ok, out, strlen(out));
done:
	if(out)
		pkg_free(out);
	srjson_DeleteDoc(doc);
	return ret;
}

static int nms_extract_imsi_from_path(
		char *path, const char *suffix, char *imsi, int imsi_size)
{
	const char *prefix = "/api/subscribers/";
	int prefix_len = (int)strlen(prefix);
	int path_len = (int)strlen(path);
	int suffix_len = (int)strlen(suffix);
	int imsi_len;
	const char *imsi_s;

	if(strncmp(path, prefix, prefix_len) != 0)
		return -1;
	imsi_len = path_len - prefix_len - suffix_len;
	if(imsi_len <= 0 || imsi_len >= imsi_size)
		return -1;
	if(strcmp(path + path_len - suffix_len, suffix) != 0)
		return -1;
	imsi_s = path + prefix_len;
	if(memchr(imsi_s, '/', imsi_len))
		return -1;
	memcpy(imsi, imsi_s, imsi_len);
	imsi[imsi_len] = '\0';
	return imsi_len;
}

static int nms_handle_get(sip_msg_t *msg, str *uri)
{
	char path[512];
	char *query = NULL;
	char imsi[32];
	srjson_doc_t doc;
	srjson_t *root = NULL;
	char *imsis_val;
	int imsis_len;

	if(nms_copy_path(uri, path, sizeof(path), &query) < 0)
		return -1;

	if(strcmp(path, "/health") == 0) {
		nms_send_json(msg, 200, &reason_ok, "{\"status\":\"ok\"}", 15);
		return 0;
	}

	if(strncmp(path, "/api/", 5) != 0)
		return -1;

	srjson_InitDoc(&doc, NULL);

	if(strcmp(path, "/api/stats") == 0) {
		if(ims_nms_handle_stats(&doc, NULL, &root) != 0)
			goto error;
		return nms_reply_json_msg(msg, &doc, root, 200);
	}

	if(strncmp(path, "/api/stats/", 11) == 0) {
		if(ims_nms_handle_stats(&doc, path + 11, &root) != 0)
			goto error;
		return nms_reply_json_msg(msg, &doc, root, 200);
	}

	if(strcmp(path, "/api/subscribers/live") == 0) {
		if(!query) {
			nms_send_json(msg, 400, &reason_nf,
					"{\"error\":\"imsis query parameter required\"}", 44);
			return 0;
		}
		imsis_val = strstr(query, "imsis=");
		if(!imsis_val) {
			nms_send_json(msg, 400, &reason_nf,
					"{\"error\":\"imsis query parameter required\"}", 44);
			return 0;
		}
		imsis_val += 6;
		imsis_len = strlen(imsis_val);
		if(ims_nms_handle_live(&doc, imsis_val, imsis_len, &root) != 0)
			goto error;
		return nms_reply_json_msg(msg, &doc, root, 200);
	}

	if(nms_extract_imsi_from_path(path, "/registration", imsi, sizeof(imsi)) >= 0) {
		if(ims_nms_handle_registration(
				   &doc, imsi, strlen(imsi), &root)
				!= 0)
			goto error;
		return nms_reply_json_msg(msg, &doc, root, 200);
	}

	if(nms_extract_imsi_from_path(path, "/calls/active", imsi, sizeof(imsi)) >= 0) {
		if(ims_nms_handle_active_calls(
				   &doc, imsi, strlen(imsi), &root)
				!= 0)
			goto error;
		return nms_reply_json_msg(msg, &doc, root, 200);
	}

notfound:
	nms_send_json(msg, 404, &reason_nf, "{\"error\":\"not found\"}", 21);
	srjson_DeleteDoc(&doc);
	return 0;

error:
	srjson_DeleteDoc(&doc);
	nms_send_json(msg, 500, &reason_nf, "{\"error\":\"internal\"}", 20);
	return -1;
}

static int nms_disconnect_imsi(sip_msg_t *msg, char *imsi, int imsi_len)
{
	int n;
	char out[128];

	n = ims_nms_terminate_imsi_calls(imsi, imsi_len);
	if(n < 0) {
		nms_send_json(msg, 500, &reason_nf, "{\"error\":\"internal\"}", 20);
		return -1;
	}
	snprintf(out, sizeof(out),
			"{\"success\":true,\"imsi\":\"%.*s\",\"terminated\":%d}", imsi_len,
			imsi, n);
	nms_send_json(msg, 200, &reason_ok, out, strlen(out));
	return 0;
}

static int nms_handle_delete(sip_msg_t *msg, str *uri)
{
	char path[512];
	char *query = NULL;
	char imsi[32];

	(void)query;
	if(nms_copy_path(uri, path, sizeof(path), &query) < 0)
		return -1;

	if(nms_extract_imsi_from_path(path, "/calls/active", imsi, sizeof(imsi)) >= 0)
		return nms_disconnect_imsi(msg, imsi, strlen(imsi));
	nms_send_json(msg, 404, &reason_nf, "{\"error\":\"not found\"}", 21);
	return 0;
}

static int nms_handle_post(sip_msg_t *msg, str *uri)
{
	char path[512];
	char *query = NULL;
	char imsi[32];

	(void)query;
	if(nms_copy_path(uri, path, sizeof(path), &query) < 0)
		return -1;

	if(nms_extract_imsi_from_path(path, "/calls/disconnect", imsi, sizeof(imsi))
			>= 0)
		return nms_disconnect_imsi(msg, imsi, strlen(imsi));

	nms_send_json(msg, 404, &reason_nf, "{\"error\":\"not found\"}", 21);
	return 0;
}

int ims_nms_dispatch(sip_msg_t *msg)
{
	str *uri;
	str *method;
	int auth;

	if(!nms_on_listen_socket(msg))
		return NONSIP_MSG_PASS;

	uri = &msg->first_line.u.request.uri;
	method = &msg->first_line.u.request.method;

	if(!(uri->len == 7 && memcmp(uri->s, "/health", 7) == 0)) {
		auth = ims_nms_check_auth(msg);
		if(auth == -1) {
			nms_send_json(msg, 401, &reason_unauth,
					"{\"error\":\"unauthorized\"}", 24);
			return 0;
		}
		if(auth == -2) {
			nms_send_json(msg, 403, &reason_forbid,
					"{\"error\":\"forbidden\"}", 21);
			return 0;
		}
	}

	if(method->len == 3 && strncasecmp(method->s, "GET", 3) == 0)
		return nms_handle_get(msg, uri);
	if(method->len == 6 && strncasecmp(method->s, "DELETE", 6) == 0)
		return nms_handle_delete(msg, uri);
	if(method->len == 4 && strncasecmp(method->s, "POST", 4) == 0)
		return nms_handle_post(msg, uri);

	nms_send_json(msg, 405, &reason_nf, "{\"error\":\"method not allowed\"}", 30);
	return 0;
}

static int w_ims_nms_dispatch(sip_msg_t *msg)
{
	return ims_nms_dispatch(msg);
}

struct module_exports exports = {
	"ims_nms_api",
	DEFAULT_DLFLAGS,
	cmds,
	params,
	0,
	0,
	0,
	mod_init,
	child_init,
	mod_destroy,
};
