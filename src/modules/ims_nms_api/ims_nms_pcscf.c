/*
 * IMS NMS HTTP API - P-CSCF usrloc bindings
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>
#include <time.h>

#include "../../core/dprint.h"
#include "../../core/sr_module.h"
#include "../ims_usrloc_pcscf/usrloc.h"
#include "../ims_usrloc_pcscf/udomain.h"
#include "../ims_usrloc_pcscf/hslot.h"

#include "ims_nms_api.h"
#include "ims_nms_pcscf.h"

#define NMS_PCSCF_MAX_CONTACTS 8

static usrloc_api_t ul_pcscf_api;
static int ul_pcscf_loaded = 0;

int nms_pcscf_init(void)
{
	if(!find_export("ul_bind_ims_usrloc_pcscf", 1, 0))
		return 0;
	memset(&ul_pcscf_api, 0, sizeof(ul_pcscf_api));
	if(((bind_usrloc_t)find_export("ul_bind_ims_usrloc_pcscf", 1, 0))(
			   &ul_pcscf_api)
			== 0) {
		ul_pcscf_loaded = 1;
		LM_DBG("bound ims_usrloc_pcscf\n");
	}
	return 0;
}

int nms_pcscf_is_loaded(void)
{
	return ul_pcscf_loaded;
}

static int nms_pcscf_contact_valid(pcontact_t *c, time_t now)
{
	if(!c)
		return 0;
	if(c->reg_state != PCONTACT_REGISTERED)
		return 0;
	if(c->expires > 0 && c->expires <= now)
		return 0;
	return 1;
}

static void nms_sort_pcscf_contacts(pcontact_t **list, int n)
{
	int i, j;

	for(i = 0; i < n - 1; i++) {
		for(j = i + 1; j < n; j++) {
			if(list[j]->expires > list[i]->expires) {
				pcontact_t *tmp = list[i];
				list[i] = list[j];
				list[j] = tmp;
			}
		}
	}
}

static int nms_pcscf_collect_by_impi(udomain_t *domain, str *impi,
		pcontact_t **out, int max_out, pcontact_t **best)
{
	unsigned int i;
	pcontact_t *c;
	int n = 0;
	time_t now = time(NULL);

	if(!domain || !impi || !out || max_out <= 0)
		return 0;

	for(i = 0; i < domain->size; i++) {
		lock_ulslot(domain, i);
		for(c = domain->table[i].first; c; c = c->next) {
			if(!nms_pcscf_contact_valid(c, now))
				continue;
			if(c->private_identity.len != impi->len
					|| memcmp(c->private_identity.s, impi->s, impi->len) != 0)
				continue;
			if(n < max_out)
				out[n++] = c;
			if(best && (!*best || c->expires > (*best)->expires))
				*best = c;
		}
		unlock_ulslot(domain, i);
	}
	return n;
}

static int nms_pcscf_collect_by_impu(udomain_t *domain, str *impu,
		pcontact_t **out, int max_out, pcontact_t **best)
{
	unsigned int i;
	pcontact_t *c;
	ppublic_t *p;
	int n = 0;
	time_t now = time(NULL);

	if(!domain || !impu || !out || max_out <= 0)
		return 0;

	for(i = 0; i < domain->size; i++) {
		lock_ulslot(domain, i);
		for(c = domain->table[i].first; c; c = c->next) {
			if(!nms_pcscf_contact_valid(c, now))
				continue;
			for(p = c->head; p; p = p->next) {
				if(p->public_identity.len == impu->len
						&& strncasecmp(p->public_identity.s, impu->s,
								   impu->len)
								== 0) {
					if(n < max_out)
						out[n++] = c;
					if(best && (!*best || c->expires > (*best)->expires))
						*best = c;
					break;
				}
			}
		}
		unlock_ulslot(domain, i);
	}
	return n;
}

static int nms_add_pcscf_contact(
		srjson_doc_t *doc, srjson_t *parent, pcontact_t *c, int primary)
{
	srjson_t *co;
	char ebuf[32];
	time_t now = time(NULL);

	co = srjson_CreateObject(doc);
	if(!co)
		return -1;
	srjson_AddItemToArray(doc, parent, co);
	srjson_AddStrToObject(doc, co, "contact", c->aor.s, c->aor.len);
	if(c->path.len > 0)
		srjson_AddStrToObject(doc, co, "path", c->path.s, c->path.len);
	ims_nms_iso_utc(c->expires, ebuf, sizeof(ebuf));
	srjson_AddStrToObject(doc, co, "lastSeenAt", ebuf, strlen(ebuf));
	srjson_AddStrToObject(doc, co, "expiresAt", ebuf, strlen(ebuf));
	if(c->expires > now)
		srjson_AddNumberToObject(
				doc, co, "expiresInSec", (double)(c->expires - now));
	else
		srjson_AddNumberToObject(doc, co, "expiresInSec", 0);
	srjson_AddStrToObject(doc, co, "state", "registered", 10);
	if(primary)
		srjson_AddTrueToObject(doc, co, "primary");
	else
		srjson_AddFalseToObject(doc, co, "primary");
	return 0;
}

int nms_fill_pcscf_registration(
		srjson_doc_t *doc, srjson_t *role, char *imsi, int imsi_len)
{
	udomain_t *domain = NULL;
	str impu;
	str impi;
	pcontact_t *matches[NMS_PCSCF_MAX_CONTACTS];
	pcontact_t *best = NULL;
	pcontact_t *unique[NMS_PCSCF_MAX_CONTACTS];
	srjson_t *reg;
	srjson_t *contacts;
	int found = 0;
	int nmatch = 0;
	int nunique = 0;
	int i, j;
	int dup;

	if(!ul_pcscf_loaded || !ul_pcscf_api.get_udomain)
		return 0;
	if(ul_pcscf_api.get_udomain((char *)"location", &domain) != 0)
		return 0;

	contacts = srjson_CreateArray(doc);
	reg = srjson_CreateObject(doc);

	if(ims_nms_build_impi(&impi, imsi, imsi_len) == 0)
		nmatch = nms_pcscf_collect_by_impi(
				domain, &impi, matches, NMS_PCSCF_MAX_CONTACTS, &best);

	if(nmatch <= 0 && ims_nms_build_impu(&impu, imsi, imsi_len) == 0)
		nmatch = nms_pcscf_collect_by_impu(
				domain, &impu, matches, NMS_PCSCF_MAX_CONTACTS, &best);

	for(i = 0; i < nmatch; i++) {
		dup = 0;
		for(j = 0; j < nunique; j++) {
			if(unique[j] == matches[i]) {
				dup = 1;
				break;
			}
		}
		if(!dup && nunique < NMS_PCSCF_MAX_CONTACTS)
			unique[nunique++] = matches[i];
	}

	if(best && nunique > 0) {
		found = 1;
		nms_sort_pcscf_contacts(unique, nunique);
		if(best != unique[0]) {
			for(i = 0; i < nunique; i++) {
				if(unique[i] == best) {
					pcontact_t *tmp = unique[0];
					unique[0] = unique[i];
					unique[i] = tmp;
					break;
				}
			}
		}
		if(best->head)
			srjson_AddStrToObject(doc, role, "identity",
					best->head->public_identity.s,
					best->head->public_identity.len);
		for(i = 0; i < nunique; i++)
			nms_add_pcscf_contact(doc, contacts, unique[i], i == 0);
	}

	if(found && best) {
		char ebuf[32];
		time_t now = time(NULL);

		if(best->head) {
			srjson_AddStrToObject(doc, role, "impu",
					best->head->public_identity.s,
					best->head->public_identity.len);
			ims_nms_json_add_msisdn_from_uri(
					doc, role, &best->head->public_identity);
		}
		ims_nms_iso_utc(best->expires, ebuf, sizeof(ebuf));
		srjson_AddStrToObject(doc, reg, "lastSeenAt", ebuf, strlen(ebuf));
		srjson_AddStrToObject(doc, reg, "expiresAt", ebuf, strlen(ebuf));
		if(best->expires > now)
			srjson_AddNumberToObject(doc, reg, "expiresInSec",
					(double)(best->expires - now));
		else
			srjson_AddNumberToObject(doc, reg, "expiresInSec", 0);
		srjson_AddStrToObject(doc, reg, "state", "registered", 10);
	}

	if(found)
		srjson_AddTrueToObject(doc, role, "registered");
	else
		srjson_AddFalseToObject(doc, role, "registered");
	srjson_AddItemToObject(
			doc, role, "registration", found ? reg : srjson_CreateNull(doc));
	srjson_AddItemToObject(doc, role, "contacts", contacts);
	return found;
}

int nms_pcscf_contacts_count(void)
{
	if(ul_pcscf_loaded && ul_pcscf_api.get_number_of_contacts)
		return (int)ul_pcscf_api.get_number_of_contacts();
	return 0;
}
