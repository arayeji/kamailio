/*
 * IMS NMS HTTP API - P-CSCF usrloc bindings
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <string.h>
#include <time.h>

#include "../../core/dprint.h"
#include "../../core/sr_module.h"
#include "../ims_usrloc_pcscf/usrloc.h"

#include "ims_nms_api.h"
#include "ims_nms_pcscf.h"

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

static int nms_add_pcscf_contact(srjson_doc_t *doc, srjson_t *parent, pcontact_t *c)
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
	srjson_AddStrToObject(doc, co, "expiresAt", ebuf, strlen(ebuf));
	if(c->expires > now)
		srjson_AddNumberToObject(
				doc, co, "expiresInSec", (double)(c->expires - now));
	else
		srjson_AddNumberToObject(doc, co, "expiresInSec", 0);
	srjson_AddStrToObject(doc, co, "state", "registered", 10);
	return 0;
}

int nms_fill_pcscf_registration(
		srjson_doc_t *doc, srjson_t *role, char *imsi, int imsi_len)
{
	udomain_t *domain = NULL;
	str impu;
	str impi;
	pcontact_t *c = NULL;
	srjson_t *reg;
	srjson_t *contacts;
	int found = 0;

	if(!ul_pcscf_loaded || !ul_pcscf_api.get_udomain)
		return 0;
	if(!ul_pcscf_api.find_pcontact_by_identity
			&& !ul_pcscf_api.find_pcontact_by_impi)
		return 0;
	if(ul_pcscf_api.get_udomain((char *)"location", &domain) != 0)
		return 0;

	contacts = srjson_CreateArray(doc);
	reg = srjson_CreateObject(doc);

	if(ims_nms_build_impi(&impi, imsi, imsi_len) == 0
			&& ul_pcscf_api.find_pcontact_by_impi
			&& ul_pcscf_api.find_pcontact_by_impi(domain, &impi, &c) == 0 && c) {
		found = 1;
		if(c->head)
			srjson_AddStrToObject(doc, role, "identity", c->head->public_identity.s,
					c->head->public_identity.len);
	}

	if(!found && ims_nms_build_impu(&impu, imsi, imsi_len) == 0
			&& ul_pcscf_api.find_pcontact_by_identity
			&& ul_pcscf_api.find_pcontact_by_identity(domain, &impu, &c) == 0
			&& c) {
		found = 1;
		srjson_AddStrToObject(doc, role, "identity", impu.s, impu.len);
	}

	if(found && c) {
		char ebuf[32];
		time_t now = time(NULL);

		if(c->head) {
			srjson_AddStrToObject(doc, role, "impu", c->head->public_identity.s,
					c->head->public_identity.len);
			ims_nms_json_add_msisdn_from_uri(
					doc, role, &c->head->public_identity);
		}
		nms_add_pcscf_contact(doc, contacts, c);
		ims_nms_iso_utc(c->expires, ebuf, sizeof(ebuf));
		srjson_AddStrToObject(doc, reg, "expiresAt", ebuf, strlen(ebuf));
		if(c->expires > now)
			srjson_AddNumberToObject(doc, reg, "expiresInSec",
					(double)(c->expires - now));
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
