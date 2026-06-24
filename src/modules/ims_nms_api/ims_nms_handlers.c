/*
 * IMS NMS HTTP API - request handlers (native usrloc + ims_dialog)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../core/dprint.h"
#include "../../core/ut.h"
#include "../../core/counters.h"
#include "../../core/sr_module.h"
#include <dlfcn.h>
#include "../ims_usrloc_scscf/usrloc.h"
#include "../ims_usrloc_scscf/impurecord.h"
#include "../ims_usrloc_scscf/udomain.h"
#include "../ims_dialog/dlg_load.h"
#include "../ims_dialog/dlg_hash.h"

#include "ims_nms_api.h"
#include "ims_nms_pcscf.h"

static usrloc_api_t ul_scscf_api;
static int ul_scscf_loaded = 0;
typedef int (*nms_get_subscription_f)(str *impi_s, ims_subscription **s, int leave_slot_locked);
static nms_get_subscription_f nms_get_subscription = NULL;
static ims_dlg_api_t ims_dlg_api;
static int ims_dlg_loaded = 0;
static str nms_role_scscf = str_init("scscf");
static str nms_role_pcscf = str_init("pcscf");
static str nms_status_na = str_init("not_available");

#define NMS_MAX_PROFILE_KEYS 32
#define NMS_PROFILE_KEY_BUF 256
#define NMS_MAX_PUBIDS 32
#define NMS_MAX_CONTACTS 8
#define NMS_MAX_DLG_REFS 64
#define NMS_MAX_CALL_SNAPS 64
#define NMS_CALLID_BUF 128
#define NMS_TAG_BUF 64

typedef struct nms_dlg_ref
{
	unsigned int h_entry;
	unsigned int h_id;
} nms_dlg_ref_t;

typedef struct nms_dlg_collect_ctx
{
	nms_dlg_ref_t refs[NMS_MAX_DLG_REFS];
	int n;
} nms_dlg_collect_ctx_t;

typedef struct nms_call_snap
{
	char callid[NMS_CALLID_BUF];
	int callid_len;
	char from_tag[NMS_TAG_BUF];
	int from_tag_len;
	int state;
	unsigned int start_ts;
} nms_call_snap_t;

typedef struct nms_call_collect_ctx
{
	nms_call_snap_t snaps[NMS_MAX_CALL_SNAPS];
	int n;
	char seen_callids[32][NMS_CALLID_BUF];
	int seen_lens[32];
	int seen_n;
} nms_call_collect_ctx_t;

int ims_nms_build_impi(str *impi, char *imsi, int imsi_len)
{
	static char impi_buf[256];

	if(!impi || !imsi || imsi_len <= 0 || ims_nms_cfg.plmn_realm.len <= 0)
		return -1;
	if(imsi_len + 1 + ims_nms_cfg.plmn_realm.len >= (int)sizeof(impi_buf))
		return -1;
	memcpy(impi_buf, imsi, imsi_len);
	impi_buf[imsi_len] = '@';
	memcpy(impi_buf + imsi_len + 1, ims_nms_cfg.plmn_realm.s,
			ims_nms_cfg.plmn_realm.len);
	impi->s = impi_buf;
	impi->len = imsi_len + 1 + ims_nms_cfg.plmn_realm.len;
	return 0;
}

static int nms_uri_user_part(str *uri, char *buf, int buf_size, str *user)
{
	char *at;
	char *semi;
	int prefix = 0;

	if(!uri || !uri->s || uri->len <= 0 || !buf || buf_size <= 1 || !user)
		return -1;
	if(uri->len >= buf_size)
		return -1;
	memcpy(buf, uri->s, uri->len);
	buf[uri->len] = '\0';

	if(strncasecmp(buf, "sip:", 4) == 0)
		prefix = 4;
	else if(strncasecmp(buf, "tel:", 4) == 0)
		prefix = 4;

	at = strchr(buf + prefix, '@');
	if(at)
		*at = '\0';
	semi = strchr(buf + prefix, ';');
	if(semi)
		*semi = '\0';

	user->s = buf + prefix;
	user->len = strlen(user->s);
	return user->len > 0 ? 0 : -1;
}

static int nms_impu_user_is_imsi(str *impu, char *imsi, int imsi_len)
{
	char buf[128];
	str user;

	if(!impu || !imsi || imsi_len <= 0)
		return 0;
	if(nms_uri_user_part(impu, buf, sizeof(buf), &user) != 0)
		return 0;
	return user.len == imsi_len && memcmp(user.s, imsi, imsi_len) == 0;
}

static void nms_add_msisdn_from_impu(
		srjson_doc_t *doc, srjson_t *role, str *impu)
{
	char user_buf[128];
	str user;

	if(!impu || !role)
		return;
	if(nms_uri_user_part(impu, user_buf, sizeof(user_buf), &user) == 0)
		srjson_AddStrToObject(doc, role, "msisdn", user.s, user.len);
}

void ims_nms_json_add_msisdn_from_uri(
		srjson_doc_t *doc, srjson_t *obj, str *uri)
{
	nms_add_msisdn_from_impu(doc, obj, uri);
}

static void nms_copy_json_str_field(srjson_doc_t *doc, srjson_t *src,
		srjson_t *dst, const char *name)
{
	srjson_t *item;

	if(!doc || !src || !dst || !name)
		return;
	item = srjson_GetObjectItem(doc, src, name);
	if(item && item->type == srjson_String)
		srjson_AddStrToObject(
				doc, dst, name, item->valuestring, strlen(item->valuestring));
}

static srjson_t *nms_best_contact_object(srjson_doc_t *doc, srjson_t *contacts)
{
	srjson_t *best = NULL;
	const char *best_ts = "";
	int i, n;

	if(!doc || !contacts || contacts->type != srjson_Array)
		return NULL;
	n = srjson_GetArraySize(doc, contacts);
	for(i = 0; i < n; i++) {
		srjson_t *co = srjson_GetArrayItem(doc, contacts, i);
		srjson_t *ts;
		const char *tsv;

		if(!co)
			continue;
		ts = srjson_GetObjectItem(doc, co, "lastSeenAt");
		if(!ts || ts->type != srjson_String || !ts->valuestring[0])
			ts = srjson_GetObjectItem(doc, co, "expiresAt");
		if(!ts || ts->type != srjson_String || !ts->valuestring[0])
			continue;
		tsv = ts->valuestring;
		if(!best || strcmp(tsv, best_ts) > 0) {
			best = co;
			best_ts = tsv;
		}
	}
	return best;
}

static void nms_promote_contact_from_object(
		srjson_doc_t *doc, srjson_t *co, srjson_t *root)
{
	srjson_t *contact;

	if(!doc || !co || !root)
		return;
	contact = srjson_GetObjectItem(doc, co, "contact");
	if(contact && contact->type == srjson_String) {
		srjson_AddStrToObject(doc, root, "contact", contact->valuestring,
				strlen(contact->valuestring));
		srjson_AddStrToObject(doc, root, "sipUri", contact->valuestring,
				strlen(contact->valuestring));
	}
}

static void nms_add_primary_contact(
		srjson_doc_t *doc, srjson_t *role, srjson_t *root)
{
	srjson_t *contacts;
	srjson_t *co;

	if(!doc || !role || !root)
		return;
	contacts = srjson_GetObjectItem(doc, role, "contacts");
	co = nms_best_contact_object(doc, contacts);
	if(co)
		nms_promote_contact_from_object(doc, co, root);
}

static void nms_promote_registration_summary(
		srjson_doc_t *doc, srjson_t *root, srjson_t *cscf)
{
	srjson_t *scscf;
	srjson_t *pcscf;
	srjson_t *role;

	if(!doc || !root || !cscf)
		return;

	scscf = srjson_GetObjectItem(doc, cscf, "scscf");
	pcscf = srjson_GetObjectItem(doc, cscf, "pcscf");
	role = scscf;
	if(!role || !srjson_GetObjectItem(doc, role, "registered"))
		role = pcscf;
	if(!role)
		return;

	nms_copy_json_str_field(doc, role, root, "impu");
	nms_copy_json_str_field(doc, role, root, "msisdn");
	nms_copy_json_str_field(doc, role, root, "identity");
	if(!srjson_GetObjectItem(doc, root, "impu")) {
		srjson_t *id = srjson_GetObjectItem(doc, root, "identity");
		if(id && id->type == srjson_String)
			srjson_AddStrToObject(doc, root, "impu", id->valuestring,
					strlen(id->valuestring));
	}
	nms_add_primary_contact(doc, role, root);
	if(!srjson_GetObjectItem(doc, root, "contact")) {
		srjson_t *best = NULL;
		srjson_t *scscf_contacts;
		srjson_t *pcscf_contacts;

		if(scscf)
			scscf_contacts = srjson_GetObjectItem(doc, scscf, "contacts");
		else
			scscf_contacts = NULL;
		if(pcscf)
			pcscf_contacts = srjson_GetObjectItem(doc, pcscf, "contacts");
		else
			pcscf_contacts = NULL;
		best = nms_best_contact_object(doc, scscf_contacts);
		{
			srjson_t *pcscf_best = nms_best_contact_object(doc, pcscf_contacts);

			if(pcscf_best) {
				srjson_t *ts_a, *ts_b;
				const char *a = "", *b = "";

				if(best) {
					ts_a = srjson_GetObjectItem(doc, best, "lastSeenAt");
					if(!ts_a || ts_a->type != srjson_String)
						ts_a = srjson_GetObjectItem(doc, best, "expiresAt");
					if(ts_a && ts_a->type == srjson_String)
						a = ts_a->valuestring;
				}
				ts_b = srjson_GetObjectItem(doc, pcscf_best, "lastSeenAt");
				if(!ts_b || ts_b->type != srjson_String)
					ts_b = srjson_GetObjectItem(doc, pcscf_best, "expiresAt");
				if(ts_b && ts_b->type == srjson_String)
					b = ts_b->valuestring;
				if(!best || (b[0] && strcmp(b, a) > 0))
					best = pcscf_best;
			}
		}
		if(best)
			nms_promote_contact_from_object(doc, best, root);
	}
}

/* Copy a profile key into the caller-owned backing buffer so its bytes stay
 * valid for the whole dialog walk. The keys str array must NOT point into
 * function-local scratch buffers, because the walk happens after this
 * function returns. */
static void nms_kb_add(str *keys, char (*keybuf)[NMS_PROFILE_KEY_BUF],
		int *nkeys, const char *s, int len)
{
	int i;

	if(!s || len <= 0 || len >= NMS_PROFILE_KEY_BUF
			|| *nkeys >= NMS_MAX_PROFILE_KEYS)
		return;
	for(i = 0; i < *nkeys; i++) {
		if(keys[i].len == len && memcmp(keys[i].s, s, len) == 0)
			return;
	}
	memcpy(keybuf[*nkeys], s, len);
	keys[*nkeys].s = keybuf[*nkeys];
	keys[*nkeys].len = len;
	(*nkeys)++;
}

/* Add a phone-number-like key together with E.164 +/00 format variants, so a
 * dialog tagged from $fU/$rU (which may carry the number with or without a
 * leading '+' or '00' international prefix) still matches a subscription
 * identity stored in another format (e.g. tel:+98... vs 98...). */
static void nms_kb_add_number(str *keys, char (*keybuf)[NMS_PROFILE_KEY_BUF],
		int *nkeys, const char *s, int len)
{
	char tmp[NMS_PROFILE_KEY_BUF];

	nms_kb_add(keys, keybuf, nkeys, s, len);
	if(len > 1 && s[0] == '+') {
		nms_kb_add(keys, keybuf, nkeys, s + 1, len - 1);
		if(len + 1 < NMS_PROFILE_KEY_BUF) {
			tmp[0] = '0';
			tmp[1] = '0';
			memcpy(tmp + 2, s + 1, len - 1);
			nms_kb_add(keys, keybuf, nkeys, tmp, len + 1);
		}
	} else if(len > 2 && s[0] == '0' && s[1] == '0') {
		tmp[0] = '+';
		memcpy(tmp + 1, s + 2, len - 2);
		nms_kb_add(keys, keybuf, nkeys, tmp, len - 1);
		nms_kb_add(keys, keybuf, nkeys, s + 2, len - 2);
	} else {
		int alldig = 1, i;

		for(i = 0; i < len; i++) {
			if(s[i] < '0' || s[i] > '9') {
				alldig = 0;
				break;
			}
		}
		if(alldig && len + 1 < NMS_PROFILE_KEY_BUF) {
			tmp[0] = '+';
			memcpy(tmp + 1, s, len);
			nms_kb_add(keys, keybuf, nkeys, tmp, len + 1);
		}
	}
}

static int nms_collect_profile_keys(char *imsi, int imsi_len, str *keys,
		char (*keybuf)[NMS_PROFILE_KEY_BUF], int *nkeys)
{
	str impi;
	ims_subscription *sub = NULL;
	char pubid_buf[NMS_MAX_PUBIDS][256];
	char user_buf[NMS_MAX_PUBIDS][128];
	str pubids[NMS_MAX_PUBIDS];
	str users[NMS_MAX_PUBIDS];
	int npub = 0;
	int i, j;

	if(!imsi || imsi_len <= 0 || !keys || !keybuf || !nkeys)
		return -1;

	*nkeys = 0;
	nms_kb_add(keys, keybuf, nkeys, imsi, imsi_len);

	if(ims_nms_build_impi(&impi, imsi, imsi_len) != 0)
		return 0;
	nms_kb_add(keys, keybuf, nkeys, impi.s, impi.len);

	if(!ul_scscf_loaded || !nms_get_subscription
			|| nms_get_subscription(&impi, &sub, 0) != 0 || !sub)
		return 0;

	/* snapshot identities under the subscription lock; add keys after unlock
	 * so usrloc is not held during dialog profile work (avoids lock-order stalls) */
	ul_scscf_api.lock_subscription(sub);
	for(i = 0; i < sub->service_profiles_cnt && npub < NMS_MAX_PUBIDS; i++) {
		for(j = 0; j < sub->service_profiles[i].public_identities_cnt
				&& npub < NMS_MAX_PUBIDS;
				j++) {
			str *pub =
					&sub->service_profiles[i].public_identities[j].public_identity;
			str user;

			if(pub->len <= 0 || pub->len >= (int)sizeof(pubid_buf[0]))
				continue;
			memcpy(pubid_buf[npub], pub->s, pub->len);
			pubids[npub].s = pubid_buf[npub];
			pubids[npub].len = pub->len;
			if(nms_uri_user_part(pub, user_buf[npub], sizeof(user_buf[0]), &user)
					== 0) {
				users[npub].s = user_buf[npub];
				users[npub].len = user.len;
			} else {
				users[npub].s = NULL;
				users[npub].len = 0;
			}
			npub++;
		}
	}
	ul_scscf_api.unlock_subscription(sub);
	ul_scscf_api.unref_subscription(sub);

	for(i = 0; i < npub; i++) {
		nms_kb_add(keys, keybuf, nkeys, pubids[i].s, pubids[i].len);
		if(users[i].s && users[i].len > 0)
			nms_kb_add_number(keys, keybuf, nkeys, users[i].s, users[i].len);
	}
	return 0;
}

static void nms_json_add_bool(srjson_doc_t *doc, srjson_t *obj, const char *name,
		int v)
{
	if(v)
		srjson_AddTrueToObject(doc, obj, name);
	else
		srjson_AddFalseToObject(doc, obj, name);
}

static void nms_add_node_role(srjson_doc_t *doc, srjson_t *root)
{
	if(ims_nms_cfg.cscf_role.len > 0)
		srjson_AddStrToObject(doc, root, "nodeRole", ims_nms_cfg.cscf_role.s,
				ims_nms_cfg.cscf_role.len);
}

static srjson_t *nms_role_mark_unavailable(
		srjson_doc_t *doc, srjson_t *cscf, const char *name)
{
	srjson_t *r = srjson_CreateObject(doc);
	if(!r)
		return NULL;
	srjson_AddItemToObject(doc, cscf, name, r);
	nms_json_add_bool(doc, r, "available", 0);
	nms_json_add_bool(doc, r, "up", 0);
	srjson_AddStrToObject(doc, r, "status", nms_status_na.s, nms_status_na.len);
	return r;
}

static srjson_t *nms_role_mark_available(
		srjson_doc_t *doc, srjson_t *cscf, const char *name)
{
	srjson_t *r = srjson_CreateObject(doc);
	if(!r)
		return NULL;
	srjson_AddItemToObject(doc, cscf, name, r);
	nms_json_add_bool(doc, r, "available", 1);
	nms_json_add_bool(doc, r, "up", 1);
	return r;
}

int ims_nms_core_init(void)
{
	if(ims_nms_cfg.cscf_role.len == 5
			&& strncmp(ims_nms_cfg.cscf_role.s, "scscf", 5) == 0
			&& find_export("ul_bind_usrloc", 1, 0)) {
		memset(&ul_scscf_api, 0, sizeof(ul_scscf_api));
		if(((bind_usrloc_t)find_export("ul_bind_usrloc", 1, 0))(&ul_scscf_api)
				== 0) {
			ul_scscf_loaded = 1;
			LM_DBG("bound ims_usrloc_scscf\n");
			{
				struct sr_module *m = find_module_by_name("ims_usrloc_scscf");
				if(m && m->handle)
					nms_get_subscription = (nms_get_subscription_f)dlsym(m->handle, "get_subscription");
			}
		}
	}
	nms_pcscf_init();
	if(load_ims_dlg_api(&ims_dlg_api) == 0) {
		ims_dlg_loaded = 1;
		LM_DBG("bound ims_dialog\n");
	}
	return 0;
}

void ims_nms_iso_utc(time_t ts, char *buf, int len)
{
	struct tm tmb;
	if(ts <= 0) {
		buf[0] = '\0';
		return;
	}
	gmtime_r(&ts, &tmb);
	snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ", tmb.tm_year + 1900,
			tmb.tm_mon + 1, tmb.tm_mday, tmb.tm_hour, tmb.tm_min, tmb.tm_sec);
}

int ims_nms_build_impu(str *impu, char *user, int user_len)
{
	static char impu_buf[256];
	if(user_len <= 0 || ims_nms_cfg.plmn_realm.len <= 0)
		return -1;
	if(user_len + 5 + ims_nms_cfg.plmn_realm.len >= (int)sizeof(impu_buf))
		return -1;
	memcpy(impu_buf, "sip:", 4);
	memcpy(impu_buf + 4, user, user_len);
	impu_buf[4 + user_len] = '@';
	memcpy(impu_buf + 5 + user_len, ims_nms_cfg.plmn_realm.s,
			ims_nms_cfg.plmn_realm.len);
	impu->s = impu_buf;
	impu->len = 5 + user_len + ims_nms_cfg.plmn_realm.len;
	return 0;
}

static int nms_scscf_contact_valid(ucontact_t *c, time_t now)
{
	if(!c)
		return 0;
	return VALID_CONTACT(c, now);
}

static int nms_impu_has_valid_contact(impurecord_t *impu_rec)
{
	impu_contact_t *ic;
	time_t now = time(NULL);

	if(!impu_rec)
		return 0;
	for(ic = impu_rec->linked_contacts.head; ic; ic = ic->next) {
		if(ic->contact && nms_scscf_contact_valid(ic->contact, now))
			return 1;
	}
	return 0;
}

static void nms_sort_contacts_by_last_modified(ucontact_t **list, int n)
{
	int i, j;

	for(i = 0; i < n - 1; i++) {
		for(j = i + 1; j < n; j++) {
			if(list[j]->last_modified > list[i]->last_modified) {
				ucontact_t *tmp = list[i];
				list[i] = list[j];
				list[j] = tmp;
			}
		}
	}
}

static int nms_add_reg_contact(srjson_doc_t *doc, srjson_t *parent, ucontact_t *c,
		time_t first_reg_ts, int primary)
{
	srjson_t *co;
	char tbuf[32];
	char ebuf[32];
	time_t now = time(NULL);

	co = srjson_CreateObject(doc);
	if(!co)
		return -1;
	srjson_AddItemToArray(doc, parent, co);
	srjson_AddStrToObject(doc, co, "contact", c->c.s, c->c.len);
	if(c->received.s && c->received.len > 0)
		srjson_AddStrToObject(
				doc, co, "received", c->received.s, c->received.len);
	ims_nms_iso_utc(first_reg_ts > 0 ? first_reg_ts : c->last_modified, tbuf,
			sizeof(tbuf));
	srjson_AddStrToObject(doc, co, "registeredAt", tbuf, strlen(tbuf));
	ims_nms_iso_utc(c->last_modified, tbuf, sizeof(tbuf));
	srjson_AddStrToObject(doc, co, "lastSeenAt", tbuf, strlen(tbuf));
	ims_nms_iso_utc(c->expires, ebuf, sizeof(ebuf));
	srjson_AddStrToObject(doc, co, "expiresAt", ebuf, strlen(ebuf));
	if(c->expires > now)
		srjson_AddNumberToObject(
				doc, co, "expiresInSec", (double)(c->expires - now));
	else
		srjson_AddNumberToObject(doc, co, "expiresInSec", 0);
	if(c->user_agent.s && c->user_agent.len > 0)
		srjson_AddStrToObject(doc, co, "userAgent", c->user_agent.s,
				c->user_agent.len);
	srjson_AddStrToObject(doc, co, "state", "registered", 10);
	if(primary)
		srjson_AddTrueToObject(doc, co, "primary");
	else
		srjson_AddFalseToObject(doc, co, "primary");
	return 0;
}

static int nms_scscf_add_impu_contacts(srjson_doc_t *doc, srjson_t *role,
		srjson_t *contacts, srjson_t *reg, impurecord_t *impu_rec, str *impu,
		int *found, time_t *first_reg)
{
	impu_contact_t *ic;
	ucontact_t *valid[NMS_MAX_CONTACTS];
	ucontact_t *best = NULL;
	time_t first = 0;
	time_t now = time(NULL);
	int nvalid = 0;
	int i;

	for(ic = impu_rec->linked_contacts.head; ic; ic = ic->next) {
		ucontact_t *c;

		if(!ic->contact)
			continue;
		c = ic->contact;
		if(!nms_scscf_contact_valid(c, now))
			continue;
		if(nvalid < NMS_MAX_CONTACTS)
			valid[nvalid++] = c;
		if(first == 0 || c->last_modified < first)
			first = c->last_modified;
		if(!best || c->last_modified > best->last_modified)
			best = c;
	}

	if(!best)
		return 0;

	nms_sort_contacts_by_last_modified(valid, nvalid);
	for(i = 0; i < nvalid; i++)
		nms_add_reg_contact(doc, contacts, valid[i], first, i == 0);

	{
		char tbuf[32], ebuf[32];

		if(impu && impu->len > 0)
			srjson_AddStrToObject(doc, role, "impu", impu->s, impu->len);
		nms_add_msisdn_from_impu(doc, role, impu);
		ims_nms_iso_utc(first, tbuf, sizeof(tbuf));
		srjson_AddStrToObject(doc, reg, "registeredAt", tbuf, strlen(tbuf));
		ims_nms_iso_utc(best->last_modified, tbuf, sizeof(tbuf));
		srjson_AddStrToObject(doc, reg, "lastSeenAt", tbuf, strlen(tbuf));
		ims_nms_iso_utc(best->expires, ebuf, sizeof(ebuf));
		srjson_AddStrToObject(doc, reg, "expiresAt", ebuf, strlen(ebuf));
		if(best->expires > now)
			srjson_AddNumberToObject(doc, reg, "expiresInSec",
					(double)(best->expires - now));
		else
			srjson_AddNumberToObject(doc, reg, "expiresInSec", 0);
		srjson_AddStrToObject(doc, reg, "state", "registered", 10);
		*first_reg = first;
		*found = 1;
	}
	return *found;
}

static int nms_fill_scscf_registration(srjson_doc_t *doc, srjson_t *role,
		char *imsi, int imsi_len)
{
	udomain_t *domain = NULL;
	impurecord_t *impu_rec = NULL;
	str impu;
	str impu_try;
	str impi;
	ims_subscription *sub = NULL;
	srjson_t *reg = NULL;
	srjson_t *contacts;
	int found = 0;
	time_t first_reg = 0;
	int i, j;
	str best_pub = {0, 0};
	static char best_pub_buf[256];
	/* snapshot of public identities, taken under the subscription lock so the
	 * udomain lookups below run without holding it (avoids lock-order deadlock
	 * with the registrar and the lock leak that blocked S-CSCF registration) */
	char pubid_buf[NMS_MAX_PUBIDS][256];
	str pubids[NMS_MAX_PUBIDS];
	int npub = 0;

	if(!ul_scscf_loaded || !ul_scscf_api.get_udomain)
		return 0;
	if(ul_scscf_api.get_udomain((char *)"location", &domain) != 0)
		return 0;

	contacts = srjson_CreateArray(doc);
	reg = srjson_CreateObject(doc);

	if(ims_nms_build_impi(&impi, imsi, imsi_len) == 0
			&& nms_get_subscription && nms_get_subscription(&impi, &sub, 0) == 0
			&& sub) {
		ul_scscf_api.lock_subscription(sub);
		for(i = 0; i < sub->service_profiles_cnt && npub < NMS_MAX_PUBIDS; i++) {
			for(j = 0; j < sub->service_profiles[i].public_identities_cnt
					&& npub < NMS_MAX_PUBIDS;
					j++) {
				str *pub = &sub->service_profiles[i]
									  .public_identities[j]
									  .public_identity;

				if(pub->len <= 0 || pub->len >= (int)sizeof(pubid_buf[0]))
					continue;
				memcpy(pubid_buf[npub], pub->s, pub->len);
				pubids[npub].s = pubid_buf[npub];
				pubids[npub].len = pub->len;
				npub++;
			}
		}
		ul_scscf_api.unlock_subscription(sub);
		ul_scscf_api.unref_subscription(sub);
	}

	/* udomain lookups run without the subscription lock held */
	for(i = 0; i < npub; i++) {
		impu_try = pubids[i];
		ul_scscf_api.lock_udomain(domain, &impu_try);
		if(ul_scscf_api.get_impurecord(domain, &impu_try, &impu_rec) == 0
				&& impu_rec->reg_state == IMPU_REGISTERED
				&& nms_impu_has_valid_contact(impu_rec)) {
			if(best_pub.len <= 0
					|| (nms_impu_user_is_imsi(&best_pub, imsi, imsi_len)
							&& !nms_impu_user_is_imsi(&pubids[i], imsi, imsi_len))) {
				if(pubids[i].len < (int)sizeof(best_pub_buf)) {
					memcpy(best_pub_buf, pubids[i].s, pubids[i].len);
					best_pub.s = best_pub_buf;
					best_pub.len = pubids[i].len;
				}
			}
		}
		ul_scscf_api.unlock_udomain(domain, &impu_try);
	}

	if(best_pub.len > 0) {
		impu_try = best_pub;
		ul_scscf_api.lock_udomain(domain, &impu_try);
		if(ul_scscf_api.get_impurecord(domain, &impu_try, &impu_rec) == 0
				&& impu_rec->reg_state == IMPU_REGISTERED) {
			nms_scscf_add_impu_contacts(doc, role, contacts, reg, impu_rec,
					&best_pub, &found, &first_reg);
		}
		ul_scscf_api.unlock_udomain(domain, &impu_try);
	}

	if(!found && ims_nms_build_impu(&impu, imsi, imsi_len) == 0) {
		impu_try = impu;
		ul_scscf_api.lock_udomain(domain, &impu_try);
		if(ul_scscf_api.get_impurecord(domain, &impu_try, &impu_rec) == 0
				&& impu_rec->reg_state == IMPU_REGISTERED) {
			nms_scscf_add_impu_contacts(doc, role, contacts, reg, impu_rec,
					&impu, &found, &first_reg);
		}
		ul_scscf_api.unlock_udomain(domain, &impu_try);
	}

	if(!found) {
		nms_json_add_bool(doc, role, "registered", 0);
		srjson_AddNullToObject(doc, role, "registration");
		srjson_AddItemToObject(doc, role, "contacts", contacts);
		return 0;
	}

	nms_json_add_bool(doc, role, "registered", 1);
	srjson_AddItemToObject(doc, role, "registration", reg);
	srjson_AddItemToObject(doc, role, "contacts", contacts);
	return found;
}

int ims_nms_handle_registration(
		srjson_doc_t *doc, char *imsi, int imsi_len, srjson_t **out_root)
{
	srjson_t *root;
	srjson_t *cscf;
	srjson_t *role_obj;
	int scscf_reg = 0;
	int pcscf_reg = 0;

	root = srjson_CreateObject(doc);
	if(!root)
		return -1;
	srjson_AddStrToObject(doc, root, "imsi", imsi, imsi_len);
	nms_add_node_role(doc, root);

	cscf = srjson_CreateObject(doc);
	if(!cscf)
		return -1;
	srjson_AddItemToObject(doc, root, "cscf", cscf);

	if(ul_scscf_loaded) {
		role_obj = nms_role_mark_available(doc, cscf, "scscf");
		if(role_obj)
			scscf_reg = nms_fill_scscf_registration(doc, role_obj, imsi, imsi_len);
	} else {
		nms_role_mark_unavailable(doc, cscf, "scscf");
	}

	if(nms_pcscf_is_loaded()) {
		role_obj = nms_role_mark_available(doc, cscf, "pcscf");
		if(role_obj)
			pcscf_reg = nms_fill_pcscf_registration(doc, role_obj, imsi, imsi_len);
	} else {
		nms_role_mark_unavailable(doc, cscf, "pcscf");
	}

	nms_role_mark_unavailable(doc, cscf, "icscf");

	nms_json_add_bool(doc, root, "registered", (scscf_reg || pcscf_reg) ? 1 : 0);
	if(scscf_reg || pcscf_reg)
		nms_promote_registration_summary(doc, root, cscf);
	*out_root = root;
	return 0;
}

typedef struct nms_call_ctx
{
	srjson_doc_t *doc;
	srjson_t *arr;
	int count;
	str *cscf_name;
} nms_call_ctx_t;

static int nms_callid_already_seen(nms_call_collect_ctx_t *ctx, str *callid)
{
	int i;

	for(i = 0; i < ctx->seen_n; i++) {
		if(ctx->seen_lens[i] == callid->len
				&& memcmp(ctx->seen_callids[i], callid->s, callid->len) == 0)
			return 1;
	}
	return 0;
}

static void nms_callid_remember(nms_call_collect_ctx_t *ctx, str *callid)
{
	if(ctx->seen_n >= 32 || !callid || !callid->s || callid->len <= 0
			|| callid->len >= NMS_CALLID_BUF)
		return;
	memcpy(ctx->seen_callids[ctx->seen_n], callid->s, callid->len);
	ctx->seen_lens[ctx->seen_n] = callid->len;
	ctx->seen_n++;
}

static int nms_dlg_ref_already_seen(nms_dlg_collect_ctx_t *ctx,
		unsigned int h_entry, unsigned int h_id)
{
	int i;

	for(i = 0; i < ctx->n; i++) {
		if(ctx->refs[i].h_entry == h_entry && ctx->refs[i].h_id == h_id)
			return 1;
	}
	return 0;
}

static int nms_collect_dlg_cb(struct dlg_cell *dlg, void *param)
{
	nms_dlg_collect_ctx_t *ctx = (nms_dlg_collect_ctx_t *)param;

	if(!dlg || !dlg->h_id)
		return 0;
	if(nms_dlg_ref_already_seen(ctx, dlg->h_entry, dlg->h_id))
		return 0;
	if(ctx->n >= NMS_MAX_DLG_REFS)
		return -1;
	ctx->refs[ctx->n].h_entry = dlg->h_entry;
	ctx->refs[ctx->n].h_id = dlg->h_id;
	ctx->n++;
	return 0;
}

static int nms_call_collect_cb(struct dlg_cell *dlg, void *param)
{
	nms_call_collect_ctx_t *ctx = (nms_call_collect_ctx_t *)param;
	nms_call_snap_t *snap;

	if(!dlg || !dlg->callid.s || dlg->callid.len <= 0)
		return 0;
	if(nms_callid_already_seen(ctx, &dlg->callid))
		return 0;
	if(ctx->n >= NMS_MAX_CALL_SNAPS)
		return -1;

	snap = &ctx->snaps[ctx->n];
	if(dlg->callid.len >= NMS_CALLID_BUF)
		return 0;
	memcpy(snap->callid, dlg->callid.s, dlg->callid.len);
	snap->callid_len = dlg->callid.len;
	if(dlg->from_tag.s && dlg->from_tag.len > 0
			&& dlg->from_tag.len < NMS_TAG_BUF) {
		memcpy(snap->from_tag, dlg->from_tag.s, dlg->from_tag.len);
		snap->from_tag_len = dlg->from_tag.len;
	} else {
		snap->from_tag_len = 0;
	}
	snap->state = dlg->state;
	snap->start_ts = dlg->start_ts ? dlg->start_ts : dlg->init_ts;
	nms_callid_remember(ctx, &dlg->callid);
	ctx->n++;
	return 0;
}

static int nms_foreach_profile_dialogs(str *profile, str *keys, int nkeys,
		ims_dlg_profile_cb_f cb, void *param)
{
	int i, r;

	for(i = 0; i < nkeys; i++) {
		r = ims_dlg_api.foreach_in_profile(profile, &keys[i], cb, param);
		/* nmsimsi profile may be undeclared on P-CSCF – treat as no matches */
		if(r < 0)
			continue;
	}
	return 0;
}

static int nms_emit_call_snapshots(
		nms_call_collect_ctx_t *collect, nms_call_ctx_t *ctx)
{
	srjson_t *co;
	char tbuf[32];
	time_t now = time(NULL);
	int i;

	for(i = 0; i < collect->n; i++) {
		nms_call_snap_t *snap = &collect->snaps[i];
		unsigned int start = snap->start_ts;

		co = srjson_CreateObject(ctx->doc);
		if(!co)
			return -1;
		srjson_AddItemToArray(ctx->doc, ctx->arr, co);
		srjson_AddStrToObject(
				ctx->doc, co, "callid", snap->callid, snap->callid_len);
		if(snap->from_tag_len > 0)
			srjson_AddStrToObject(ctx->doc, co, "fromTag", snap->from_tag,
					snap->from_tag_len);
		if(ctx->cscf_name)
			srjson_AddStrToObject(ctx->doc, co, "cscf", ctx->cscf_name->s,
					ctx->cscf_name->len);
		if(snap->state == DLG_STATE_CONFIRMED
				|| snap->state == DLG_STATE_CONFIRMED_NA)
			srjson_AddStrToObject(ctx->doc, co, "state", "confirmed", 9);
		else
			srjson_AddStrToObject(ctx->doc, co, "state", "early", 5);
		ims_nms_iso_utc((time_t)start, tbuf, sizeof(tbuf));
		srjson_AddStrToObject(ctx->doc, co, "startedAt", tbuf, strlen(tbuf));
		if(start > 0 && start <= (unsigned int)now)
			srjson_AddNumberToObject(
					ctx->doc, co, "durationSec", (double)(now - start));
		ctx->count++;
	}
	return 0;
}

static int nms_collect_calls(str *imsi, str *cscf_name, srjson_t *arr, srjson_doc_t *doc)
{
	nms_call_ctx_t ctx;
	nms_call_collect_ctx_t collect;
	str profile;
	str keys[NMS_MAX_PROFILE_KEYS];
	char keybuf[NMS_MAX_PROFILE_KEYS][NMS_PROFILE_KEY_BUF];
	int nkeys = 0;

	if(!ims_dlg_loaded || !ims_dlg_api.foreach_in_profile || !imsi || !imsi->s)
		return 0;

	memset(&ctx, 0, sizeof(ctx));
	memset(&collect, 0, sizeof(collect));
	ctx.doc = doc;
	ctx.arr = arr;
	ctx.cscf_name = cscf_name;
	profile.s = IMS_NMS_PROFILE;
	profile.len = strlen(IMS_NMS_PROFILE);

	nms_collect_profile_keys(imsi->s, imsi->len, keys, keybuf, &nkeys);
	nms_foreach_profile_dialogs(
			&profile, keys, nkeys, nms_call_collect_cb, &collect);
	if(nms_emit_call_snapshots(&collect, &ctx) < 0)
		return ctx.count;
	return ctx.count;
}

static void nms_fill_role_calls_empty(srjson_doc_t *doc, srjson_t *role)
{
	srjson_AddNumberToObject(doc, role, "activeCalls", 0);
	srjson_AddItemToObject(doc, role, "calls", srjson_CreateArray(doc));
}

static int nms_fill_role_calls(
		srjson_doc_t *doc, srjson_t *role, str *imsi, str *role_name)
{
	srjson_t *calls;
	int n;

	calls = srjson_CreateArray(doc);
	if(!calls)
		return 0;
	n = nms_collect_calls(imsi, role_name, calls, doc);
	srjson_AddNumberToObject(doc, role, "activeCalls", n);
	srjson_AddItemToObject(doc, role, "calls", calls);
	return n;
}

int ims_nms_handle_active_calls(
		srjson_doc_t *doc, char *imsi, int imsi_len, srjson_t **out_root)
{
	str imsi_str;
	srjson_t *root;
	srjson_t *cscf;
	srjson_t *role_obj;
	int total = 0;

	imsi_str.s = imsi;
	imsi_str.len = imsi_len;

	root = srjson_CreateObject(doc);
	cscf = srjson_CreateObject(doc);
	if(!root || !cscf)
		return -1;

	srjson_AddStrToObject(doc, root, "imsi", imsi, imsi_len);
	nms_add_node_role(doc, root);
	srjson_AddItemToObject(doc, root, "cscf", cscf);

	if(ul_scscf_loaded && ims_dlg_loaded) {
		role_obj = nms_role_mark_available(doc, cscf, "scscf");
		if(role_obj)
			total += nms_fill_role_calls(doc, role_obj, &imsi_str, &nms_role_scscf);
	} else {
		role_obj = nms_role_mark_unavailable(doc, cscf, "scscf");
		if(role_obj)
			nms_fill_role_calls_empty(doc, role_obj);
	}

	if(nms_pcscf_is_loaded() && ims_dlg_loaded) {
		role_obj = nms_role_mark_available(doc, cscf, "pcscf");
		if(role_obj)
			total += nms_fill_role_calls(doc, role_obj, &imsi_str, &nms_role_pcscf);
	} else {
		role_obj = nms_role_mark_unavailable(doc, cscf, "pcscf");
		if(role_obj)
			nms_fill_role_calls_empty(doc, role_obj);
	}

	nms_role_mark_unavailable(doc, cscf, "icscf");

	srjson_AddNumberToObject(doc, root, "activeCalls", total);
	*out_root = root;
	return 0;
}

static int nms_stat_val(const char *group, const char *name)
{
	counter_handle_t h;
	str g;
	str n;

	g.s = (char *)group;
	g.len = strlen(group);
	n.s = (char *)name;
	n.len = strlen(name);
	if(counter_lookup_str(&h, &g, &n) < 0)
		return 0;
	return (int)counter_get_val(h);
}

static int nms_scscf_registered_count(void)
{
	return nms_stat_val("ims_usrloc_scscf", "active_impus");
}

static int nms_active_dialog_count(void)
{
	if(!ims_dlg_loaded)
		return 0;
	return nms_stat_val("dialog_ng", "active");
}

int ims_nms_handle_stats(srjson_doc_t *doc, char *role, srjson_t **out_root)
{
	srjson_t *root;
	srjson_t *cscf;
	srjson_t *role_obj;
	char tbuf[32];
	time_t now = time(NULL);

	(void)role;

	root = srjson_CreateObject(doc);
	cscf = srjson_CreateObject(doc);
	if(!root || !cscf)
		return -1;

	ims_nms_iso_utc(now, tbuf, sizeof(tbuf));
	srjson_AddStrToObject(doc, root, "fetchedAt", tbuf, strlen(tbuf));
	srjson_AddStrToObject(doc, root, "host", ims_nms_cfg.api_host.s,
			ims_nms_cfg.api_host.len);
	nms_add_node_role(doc, root);
	srjson_AddItemToObject(doc, root, "cscf", cscf);

	if(ul_scscf_loaded) {
		role_obj = nms_role_mark_available(doc, cscf, "scscf");
		if(role_obj) {
			srjson_AddNumberToObject(
					doc, role_obj, "registered", nms_scscf_registered_count());
			srjson_AddNumberToObject(
					doc, role_obj, "activeCalls", nms_active_dialog_count());
		}
	} else {
		nms_role_mark_unavailable(doc, cscf, "scscf");
	}

	if(nms_pcscf_is_loaded()) {
		role_obj = nms_role_mark_available(doc, cscf, "pcscf");
		if(role_obj) {
			srjson_AddNumberToObject(
					doc, role_obj, "contacts", nms_pcscf_contacts_count());
			srjson_AddNumberToObject(
					doc, role_obj, "activeCalls", nms_active_dialog_count());
		}
	} else {
		nms_role_mark_unavailable(doc, cscf, "pcscf");
	}

	nms_role_mark_unavailable(doc, cscf, "icscf");

	*out_root = root;
	return 0;
}

static void nms_live_copy_call(srjson_doc_t *doc, srjson_t *calls_out, srjson_t *src)
{
	srjson_t *dst;
	srjson_t *f;

	if(!src || src->type != srjson_Object)
		return;
	dst = srjson_CreateObject(doc);
	if(!dst)
		return;
	srjson_AddItemToArray(doc, calls_out, dst);
	f = srjson_GetObjectItem(doc, src, "callid");
	if(f && f->type == srjson_String)
		srjson_AddStrToObject(doc, dst, "callid", f->valuestring,
				strlen(f->valuestring));
	f = srjson_GetObjectItem(doc, src, "state");
	if(f && f->type == srjson_String)
		srjson_AddStrToObject(doc, dst, "state", f->valuestring,
				strlen(f->valuestring));
	f = srjson_GetObjectItem(doc, src, "startedAt");
	if(f && f->type == srjson_String)
		srjson_AddStrToObject(doc, dst, "startedAt", f->valuestring,
				strlen(f->valuestring));
	f = srjson_GetObjectItem(doc, src, "durationSec");
	if(f && f->type == srjson_Number)
		srjson_AddNumberToObject(doc, dst, "durationSec", f->valuedouble);
	f = srjson_GetObjectItem(doc, src, "cscf");
	if(f && f->type == srjson_String)
		srjson_AddStrToObject(doc, dst, "cscf", f->valuestring,
				strlen(f->valuestring));
}

static void nms_live_merge_role_calls(srjson_doc_t *doc, srjson_t *calls_root,
		const char *role_name, srjson_t *calls_out)
{
	srjson_t *cscf;
	srjson_t *role;
	srjson_t *calls;
	int i;

	cscf = srjson_GetObjectItem(doc, calls_root, "cscf");
	if(!cscf)
		return;
	role = srjson_GetObjectItem(doc, cscf, role_name);
	if(!role)
		return;
	calls = srjson_GetObjectItem(doc, role, "calls");
	if(!calls || calls->type != srjson_Array)
		return;
	for(i = 0; i < srjson_GetArraySize(doc, calls); i++)
		nms_live_copy_call(doc, calls_out, srjson_GetArrayItem(doc, calls, i));
}

int ims_nms_handle_live(
		srjson_doc_t *doc, char *imsis_csv, int csv_len, srjson_t **out_root)
{
	srjson_t *root;
	srjson_t *subs;
	char imsi[32];
	char tbuf[32];
	int count = 0;
	int i, start;
	time_t now = time(NULL);

	if(csv_len <= 0)
		return -1;

	root = srjson_CreateObject(doc);
	subs = srjson_CreateArray(doc);
	if(!root || !subs)
		return -1;

	ims_nms_iso_utc(now, tbuf, sizeof(tbuf));
	srjson_AddStrToObject(doc, root, "fetchedAt", tbuf, strlen(tbuf));
	nms_add_node_role(doc, root);
	srjson_AddItemToObject(doc, root, "subscribers", subs);

	start = 0;
	for(i = 0; i <= csv_len; i++) {
		srjson_t *reg_root;
		srjson_t *calls_root;
		srjson_t *entry;
		srjson_t *reg_item;
		srjson_t *calls_item;
		srjson_t *calls_out;
		int len, active;

		if(i < csv_len && imsis_csv[i] != ',')
			continue;
		if(i <= start)
			goto next_csv;
		len = i - start;
		if(len >= (int)sizeof(imsi))
			len = sizeof(imsi) - 1;
		memcpy(imsi, imsis_csv + start, len);
		imsi[len] = '\0';
		if(count >= IMS_NMS_LIVE_MAX)
			break;

		reg_root = NULL;
		calls_root = NULL;
		if(ims_nms_handle_registration(doc, imsi, len, &reg_root) != 0)
			goto next_csv;
		reg_item = srjson_GetObjectItem(doc, reg_root, "registered");
		if(!reg_item || reg_item->type != srjson_True)
			goto next_csv;
		if(ims_nms_handle_active_calls(doc, imsi, len, &calls_root) != 0)
			goto next_csv;

		entry = srjson_CreateObject(doc);
		if(!entry)
			goto next_csv;
		srjson_AddItemToArray(doc, subs, entry);
		srjson_t *cscf_item;
		srjson_t *scscf_role;

		srjson_AddStrToObject(doc, entry, "imsi", imsi, len);
		nms_json_add_bool(doc, entry, "registered", 1);

		cscf_item = srjson_GetObjectItem(doc, reg_root, "cscf");
		if(cscf_item) {
			scscf_role = srjson_GetObjectItem(doc, cscf_item, "scscf");
			if(scscf_role) {
				reg_item = srjson_GetObjectItem(doc, scscf_role, "impu");
				if(reg_item && reg_item->type == srjson_String)
					srjson_AddStrToObject(doc, entry, "impu",
							reg_item->valuestring,
							strlen(reg_item->valuestring));
				reg_item = srjson_GetObjectItem(doc, scscf_role, "msisdn");
				if(reg_item && reg_item->type == srjson_String)
					srjson_AddStrToObject(doc, entry, "msisdn",
							reg_item->valuestring,
							strlen(reg_item->valuestring));
			}
			srjson_AddItemToObject(doc, entry, "cscf", cscf_item);
		}

		active = 0;
		calls_item = srjson_GetObjectItem(doc, calls_root, "activeCalls");
		if(calls_item && calls_item->type == srjson_Number)
			active = (int)calls_item->valuedouble;
		srjson_AddNumberToObject(doc, entry, "activeCalls", active);

		calls_out = srjson_CreateArray(doc);
		if(calls_out) {
			nms_live_merge_role_calls(doc, calls_root, "scscf", calls_out);
			nms_live_merge_role_calls(doc, calls_root, "pcscf", calls_out);
			srjson_AddItemToObject(doc, entry, "calls", calls_out);
		}
		count++;

	next_csv:
		if(reg_root)
			srjson_Delete(doc, reg_root);
		if(calls_root)
			srjson_Delete(doc, calls_root);
		start = i + 1;
	}

	*out_root = root;
	return 0;
}

typedef struct nms_term_ctx
{
	int count;
} nms_term_ctx_t;

int ims_nms_terminate_imsi_calls(char *imsi, int imsi_len)
{
	str profile;
	str keys[NMS_MAX_PROFILE_KEYS];
	char keybuf[NMS_MAX_PROFILE_KEYS][NMS_PROFILE_KEY_BUF];
	str hdrs = str_init("Reason: NMS disconnect\r\n");
	nms_dlg_collect_ctx_t collect;
	nms_term_ctx_t ctx;
	int nkeys = 0;
	int i;

	if(!ims_dlg_loaded || !ims_dlg_api.foreach_in_profile
			|| !ims_dlg_api.lookup_terminate_dlg)
		return -1;

	profile.s = IMS_NMS_PROFILE;
	profile.len = strlen(IMS_NMS_PROFILE);
	memset(&collect, 0, sizeof(collect));
	memset(&ctx, 0, sizeof(ctx));
	nms_collect_profile_keys(imsi, imsi_len, keys, keybuf, &nkeys);
	nms_foreach_profile_dialogs(
			&profile, keys, nkeys, nms_collect_dlg_cb, &collect);

	/* terminate after profile lock is released (foreach_in_profile unlocks) */
	for(i = 0; i < collect.n; i++) {
		if(ims_dlg_api.lookup_terminate_dlg(
				   collect.refs[i].h_entry, collect.refs[i].h_id, &hdrs)
				== 0)
			ctx.count++;
	}
	return ctx.count;
}
