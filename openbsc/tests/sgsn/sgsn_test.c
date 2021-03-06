/* Test the SGSN */
/*
 * (C) 2014 by Holger Hans Peter Freyther
 * (C) 2014 by sysmocom s.f.m.c. GmbH
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <openbsc/gprs_llc.h>
#include <openbsc/sgsn.h>
#include <openbsc/gprs_gmm.h>
#include <openbsc/debug.h>
#include <openbsc/gsm_subscriber.h>
#include <openbsc/gprs_gsup_messages.h>
#include <openbsc/gprs_gsup_client.h>
#include <openbsc/gprs_utils.h>

#include <osmocom/gprs/gprs_bssgp.h>

#include <osmocom/gsm/gsm_utils.h>
#include <openbsc/gsm_04_08_gprs.h>

#include <osmocom/core/application.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/rate_ctr.h>

#include <stdio.h>

extern void *tall_msgb_ctx;

void *tall_bsc_ctx;
static struct sgsn_instance sgsn_inst = {
	.config_file = "osmo_sgsn.cfg",
	.cfg = {
		.gtp_statedir = "./",
		.auth_policy = SGSN_AUTH_POLICY_CLOSED,
	},
};
struct sgsn_instance *sgsn = &sgsn_inst;
unsigned sgsn_tx_counter = 0;

/* override */
int bssgp_tx_dl_ud(struct msgb *msg, uint16_t pdu_lifetime,
		   struct bssgp_dl_ud_par *dup)
{
	sgsn_tx_counter += 1;
	msgb_free(msg);
	return 0;
}

/* override, requires '-Wl,--wrap=sgsn_update_subscriber_data' */
void __real_sgsn_update_subscriber_data(struct sgsn_mm_ctx *);
void (*update_subscriber_data_cb)(struct sgsn_mm_ctx *) =
    &__real_sgsn_update_subscriber_data;

void __wrap_sgsn_update_subscriber_data(struct sgsn_mm_ctx *mmctx)
{
	(*update_subscriber_data_cb)(mmctx);
}

/* override, requires '-Wl,--wrap=gprs_subscr_request_update_location' */
int __real_gprs_subscr_request_update_location(struct sgsn_mm_ctx *mmctx);
int (*subscr_request_update_location_cb)(struct sgsn_mm_ctx *mmctx) =
	&__real_gprs_subscr_request_update_location;

int __wrap_gprs_subscr_request_update_location(struct sgsn_mm_ctx *mmctx) {
	return (*subscr_request_update_location_cb)(mmctx);
};

/* override, requires '-Wl,--wrap=gprs_subscr_request_auth_info' */
int __real_gprs_subscr_request_auth_info(struct sgsn_mm_ctx *mmctx);
int (*subscr_request_auth_info_cb)(struct sgsn_mm_ctx *mmctx) =
	&__real_gprs_subscr_request_auth_info;

int __wrap_gprs_subscr_request_auth_info(struct sgsn_mm_ctx *mmctx) {
	return (*subscr_request_auth_info_cb)(mmctx);
};

/* override, requires '-Wl,--wrap=gprs_gsup_client_send' */
int __real_gprs_gsup_client_send(struct gprs_gsup_client *gsupc, struct msgb *msg);
int (*gprs_gsup_client_send_cb)(struct gprs_gsup_client *gsupc, struct msgb *msg) =
	&__real_gprs_gsup_client_send;

int __wrap_gprs_gsup_client_send(struct gprs_gsup_client *gsupc, struct msgb *msg)
{
	return (*gprs_gsup_client_send_cb)(gsupc, msg);
};

static int count(struct llist_head *head)
{
	struct llist_head *cur;
	int count = 0;

	llist_for_each(cur, head)
		count += 1;

	return count;		
}

static struct msgb *create_msg(const uint8_t *data, size_t len)
{
	struct msgb *msg = msgb_alloc(len + 8, "test message");
	msg->l1h = msgb_put(msg, 8);
	msg->l2h = msgb_put(msg, len);
	memcpy(msg->l2h, data, len);

	msgb_bcid(msg) = msg->l1h;
	msgb_gmmh(msg) = msg->l2h;
	return msg;
}

/*
 * Create a context and search for it
 */
static struct sgsn_mm_ctx *alloc_mm_ctx(uint32_t tlli, struct gprs_ra_id *raid)
{
	struct sgsn_mm_ctx *ctx, *ictx;
	struct gprs_llc_lle *lle;
	int old_count = count(gprs_llme_list());

	lle = gprs_lle_get_or_create(tlli, 3);
	ctx = sgsn_mm_ctx_alloc(tlli, raid);
	ctx->mm_state = GMM_REGISTERED_NORMAL;
	ctx->llme = lle->llme;

	ictx = sgsn_mm_ctx_by_tlli(tlli, raid);
	OSMO_ASSERT(ictx == ctx);

	OSMO_ASSERT(count(gprs_llme_list()) == old_count + 1);

	return ctx;
}

static void send_0408_message(struct gprs_llc_llme *llme, uint32_t tlli,
			      const uint8_t *data, size_t data_len)
{
	struct msgb *msg;

	sgsn_tx_counter = 0;

	msg = create_msg(data, data_len);
	msgb_tlli(msg) = tlli;
	gsm0408_gprs_rcvmsg(msg, llme);
	msgb_free(msg);
}

static void test_llme(void)
{
	struct gprs_llc_lle *lle, *lle_copy;
	uint32_t local_tlli;
	uint32_t foreign_tlli;

	printf("Testing LLME allocations\n");
	local_tlli = gprs_tmsi2tlli(0x234, TLLI_LOCAL);
	foreign_tlli = gprs_tmsi2tlli(0x234, TLLI_FOREIGN);

	/* initial state */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);

	/* Create a new entry */
	lle = gprs_lle_get_or_create(local_tlli, 3);
	OSMO_ASSERT(lle);
	OSMO_ASSERT(count(gprs_llme_list()) == 1);

	/* No new entry is created */
	lle_copy = gprs_lle_get_or_create(local_tlli, 3);
	OSMO_ASSERT(lle == lle_copy);
	OSMO_ASSERT(count(gprs_llme_list()) == 1);
	lle_copy = gprs_lle_get_or_create(foreign_tlli, 3);
	OSMO_ASSERT(lle == lle_copy);
	OSMO_ASSERT(count(gprs_llme_list()) == 1);

	/* unassign which should delete it*/
	gprs_llgmm_assign(lle->llme, lle->llme->tlli, 0xffffffff, GPRS_ALGO_GEA0, NULL);

	/* Check that everything was cleaned up */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
}

struct gsm_subscriber *last_updated_subscr = NULL;
void my_dummy_sgsn_update_subscriber_data(struct sgsn_mm_ctx *mmctx)
{
	OSMO_ASSERT(mmctx);
	fprintf(stderr, "Called %s, mmctx = %p, subscr = %p\n",
		__func__, mmctx, mmctx->subscr);
	last_updated_subscr = mmctx->subscr;
}

static void assert_subscr(const struct gsm_subscriber *subscr, const char *imsi)
{
	struct gsm_subscriber *sfound;
	OSMO_ASSERT(subscr);
	OSMO_ASSERT(strcmp(subscr->imsi, imsi) == 0);

	sfound = gprs_subscr_get_by_imsi(imsi);
	OSMO_ASSERT(sfound == subscr);

	subscr_put(sfound);
}

static void show_subscrs(FILE *out)
{
	struct gsm_subscriber *subscr;

	llist_for_each_entry(subscr, &active_subscribers, entry) {
		fprintf(out, "  Subscriber: %s, "
			"use count: %d\n",
			subscr->imsi, subscr->use_count);
	}
}

static void assert_no_subscrs()
{
	show_subscrs(stdout);
	fflush(stdout);
	OSMO_ASSERT(llist_empty(&active_subscribers));
}

static void test_subscriber(void)
{
	struct gsm_subscriber *s1, *s2, *s3, *sfound;
	const char *imsi1 = "1234567890";
	const char *imsi2 = "9876543210";
	const char *imsi3 = "5656565656";

	update_subscriber_data_cb = my_dummy_sgsn_update_subscriber_data;

	printf("Testing core subscriber data API\n");

	/* Check for emptiness */
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi1) == NULL);
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi2) == NULL);
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi3) == NULL);

	/* Allocate entry 1 */
	s1 = gprs_subscr_get_or_create(imsi1);
	s1->flags |= GSM_SUBSCRIBER_FIRST_CONTACT;
	assert_subscr(s1, imsi1);
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi2) == NULL);

	/* Allocate entry 2 */
	s2 = gprs_subscr_get_or_create(imsi2);
	s2->flags |= GSM_SUBSCRIBER_FIRST_CONTACT;

	/* Allocate entry 3 */
	s3 = gprs_subscr_get_or_create(imsi3);

	/* Check entries */
	assert_subscr(s1, imsi1);
	assert_subscr(s2, imsi2);
	assert_subscr(s3, imsi3);

	/* Update entry 1 */
	last_updated_subscr = NULL;
	gprs_subscr_update(s1);
	OSMO_ASSERT(last_updated_subscr == NULL);
	OSMO_ASSERT(s1->sgsn_data->mm == NULL);
	OSMO_ASSERT((s1->flags & GSM_SUBSCRIBER_FIRST_CONTACT) == 0);

	/* There is no subscriber cache. Verify it */
	gprs_subscr_cleanup(s1);
	subscr_put(s1);
	s1 = NULL;
	sfound = gprs_subscr_get_by_imsi(imsi1);
	OSMO_ASSERT(sfound == NULL);

	assert_subscr(s2, imsi2);
	assert_subscr(s3, imsi3);

	/* Free entry 2 (GSM_SUBSCRIBER_FIRST_CONTACT is set) */
	gprs_subscr_cleanup(s2);
	subscr_put(s2);
	s2 = NULL;
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi1) == NULL);
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi2) == NULL);
	assert_subscr(s3, imsi3);

	/* Try to delete entry 3 */
	gprs_subscr_cleanup(s3);
	subscr_put(s3);
	s3 = NULL;
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi3) == NULL);

	OSMO_ASSERT(llist_empty(&active_subscribers));

	update_subscriber_data_cb = __real_sgsn_update_subscriber_data;
}

static void test_auth_triplets(void)
{
	struct gsm_subscriber *s1, *s1found;
	const char *imsi1 = "1234567890";
	struct gsm_auth_tuple *at;
	struct sgsn_mm_ctx *ctx;
	struct gprs_ra_id raid = { 0, };
	uint32_t local_tlli = 0xffeeddcc;

	printf("Testing authentication triplet handling\n");

	/* Check for emptiness */
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi1) == NULL);

	/* Allocate entry 1 */
	s1 = gprs_subscr_get_or_create(imsi1);
	s1->flags |= GSM_SUBSCRIBER_FIRST_CONTACT;
	s1found = gprs_subscr_get_by_imsi(imsi1);
	OSMO_ASSERT(s1found == s1);
	subscr_put(s1found);

	/* Create a context */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ctx = alloc_mm_ctx(local_tlli, &raid);

	/* Attach s1 to ctx */
	ctx->subscr = subscr_get(s1);
	ctx->subscr->sgsn_data->mm = ctx;

	/* Try to get auth tuple */
	at = sgsn_auth_get_tuple(ctx, GSM_KEY_SEQ_INVAL);
	OSMO_ASSERT(at == NULL);

	/* Add triplets */
	s1->sgsn_data->auth_triplets[0].key_seq = 0;
	s1->sgsn_data->auth_triplets[1].key_seq = 1;
	s1->sgsn_data->auth_triplets[2].key_seq = 2;

	/* Try to get auth tuple */
	at = sgsn_auth_get_tuple(ctx, GSM_KEY_SEQ_INVAL);
	OSMO_ASSERT(at != NULL);
	OSMO_ASSERT(at->key_seq == 0);
	OSMO_ASSERT(at->use_count == 1);
	at = sgsn_auth_get_tuple(ctx, at->key_seq);
	OSMO_ASSERT(at != NULL);
	OSMO_ASSERT(at->key_seq == 1);
	OSMO_ASSERT(at->use_count == 1);
	at = sgsn_auth_get_tuple(ctx, at->key_seq);
	OSMO_ASSERT(at != NULL);
	OSMO_ASSERT(at->key_seq == 2);
	OSMO_ASSERT(at->use_count == 1);
	at = sgsn_auth_get_tuple(ctx, at->key_seq);
	OSMO_ASSERT(at == NULL);

	/* Free MM context and subscriber */
	subscr_put(s1);
	sgsn_mm_ctx_cleanup_free(ctx);
	s1found = gprs_subscr_get_by_imsi(imsi1);
	OSMO_ASSERT(s1found == NULL);
}

#define TEST_GSUP_IMSI1_IE 0x01, 0x05, 0x21, 0x43, 0x65, 0x87, 0x09

static int rx_gsup_message(const uint8_t *data, size_t data_len)
{
	struct msgb *msg;
	int rc;

	msg = msgb_alloc(1024, __func__);
	msg->l2h = msgb_put(msg, data_len);
	OSMO_ASSERT(msg->l2h != NULL);
	memcpy(msg->l2h, data, data_len);
	rc = gprs_subscr_rx_gsup_message(msg);
	msgb_free(msg);

	return rc;
}

static void test_subscriber_gsup(void)
{
	struct gsm_subscriber *s1, *s1found;
	const char *imsi1 = "1234567890";
	struct sgsn_mm_ctx *ctx;
	struct gprs_ra_id raid = { 0, };
	uint32_t local_tlli = 0xffeeddcc;
	struct sgsn_subscriber_pdp_data *pdpd;
	int rc;

	static const uint8_t send_auth_info_res[] = {
		0x0a,
		TEST_GSUP_IMSI1_IE,
		0x03, 0x22, /* Auth tuple */
			0x20, 0x10,
				0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
				0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
			0x21, 0x04,
				0x21, 0x22, 0x23, 0x24,
			0x22, 0x08,
				0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
		0x03, 0x22, /* Auth tuple */
			0x20, 0x10,
				0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
				0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90,
			0x21, 0x04,
				0xa1, 0xa2, 0xa3, 0xa4,
			0x22, 0x08,
				0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8,
	};

	static const uint8_t send_auth_info_err[] = {
		0x09,
		TEST_GSUP_IMSI1_IE,
		0x02, 0x01, 0x07 /* GPRS not allowed */
	};

	static const uint8_t update_location_res[] = {
		0x06,
		TEST_GSUP_IMSI1_IE,
		0x04, 0x00, /* PDP info complete */
		0x05, 0x12,
			0x10, 0x01, 0x01,
			0x11, 0x02, 0xf1, 0x21, /* IPv4 */
			0x12, 0x09, 0x04, 't', 'e', 's', 't', 0x03, 'a', 'p', 'n',
		0x05, 0x11,
			0x10, 0x01, 0x02,
			0x11, 0x02, 0xf1, 0x21, /* IPv4 */
			0x12, 0x08, 0x03, 'f', 'o', 'o', 0x03, 'a', 'p', 'n',
	};

	static const uint8_t update_location_err[] = {
		0x05,
		TEST_GSUP_IMSI1_IE,
		0x02, 0x01, 0x07 /* GPRS not allowed */
	};

	static const uint8_t location_cancellation_req[] = {
		0x1c,
		TEST_GSUP_IMSI1_IE,
		0x06, 0x01, 0x00,
	};

	static const uint8_t location_cancellation_req_withdraw[] = {
		0x1c,
		TEST_GSUP_IMSI1_IE,
		0x06, 0x01, 0x01,
	};

	static const uint8_t location_cancellation_req_other[] = {
		0x1c,
		0x01, 0x05, 0x11, 0x11, 0x11, 0x11, 0x01,
		0x06, 0x01, 0x00,
	};

	static const uint8_t purge_ms_err[] = {
		0x0d,
		TEST_GSUP_IMSI1_IE,
		0x02, 0x01, 0x02, /* IMSI unknown in HLR */
	};

	static const uint8_t purge_ms_err_no_cause[] = {
		0x0d,
		TEST_GSUP_IMSI1_IE,
	};

	static const uint8_t purge_ms_res[] = {
		0x0e,
		TEST_GSUP_IMSI1_IE,
		0x07, 0x00,
	};


	static const uint8_t insert_data_req[] = {
		0x10,
		TEST_GSUP_IMSI1_IE,
		0x05, 0x11,
			0x10, 0x01, 0x03,
			0x11, 0x02, 0xf1, 0x21, /* IPv4 */
			0x12, 0x08, 0x03, 'b', 'a', 'r', 0x03, 'a', 'p', 'n',
	};

	static const uint8_t delete_data_req[] = {
		0x14,
		TEST_GSUP_IMSI1_IE,
		0x10, 0x01, 0x03,
	};

	printf("Testing subcriber GSUP handling\n");

	update_subscriber_data_cb = my_dummy_sgsn_update_subscriber_data;

	/* Check for emptiness */
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi1) == NULL);

	/* Allocate entry 1 */
	s1 = gprs_subscr_get_or_create(imsi1);
	s1->flags |= GSM_SUBSCRIBER_FIRST_CONTACT;
	s1found = gprs_subscr_get_by_imsi(imsi1);
	OSMO_ASSERT(s1found == s1);
	subscr_put(s1found);

	/* Create a context */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ctx = alloc_mm_ctx(local_tlli, &raid);

	/* Attach s1 to ctx */
	ctx->subscr = subscr_get(s1);
	ctx->subscr->sgsn_data->mm = ctx;

	/* Inject SendAuthInfoReq GSUP message */
	rc = rx_gsup_message(send_auth_info_res, sizeof(send_auth_info_res));
	OSMO_ASSERT(rc >= 0);
	OSMO_ASSERT(last_updated_subscr == s1);

	/* Check triplets */
	OSMO_ASSERT(s1->sgsn_data->auth_triplets[0].key_seq == 0);
	OSMO_ASSERT(s1->sgsn_data->auth_triplets[1].key_seq == 1);
	OSMO_ASSERT(s1->sgsn_data->auth_triplets[2].key_seq == GSM_KEY_SEQ_INVAL);

	/* Inject SendAuthInfoErr GSUP message */
	rc = rx_gsup_message(send_auth_info_err, sizeof(send_auth_info_err));
	OSMO_ASSERT(rc == -GMM_CAUSE_GPRS_NOTALLOWED);
	OSMO_ASSERT(last_updated_subscr == s1);
	OSMO_ASSERT(s1->sgsn_data->error_cause == GMM_CAUSE_GPRS_NOTALLOWED);

	/* Check triplets */
	OSMO_ASSERT(s1->sgsn_data->auth_triplets[0].key_seq == GSM_KEY_SEQ_INVAL);
	OSMO_ASSERT(s1->sgsn_data->auth_triplets[1].key_seq == GSM_KEY_SEQ_INVAL);
	OSMO_ASSERT(s1->sgsn_data->auth_triplets[2].key_seq == GSM_KEY_SEQ_INVAL);

	/* Inject UpdateLocRes GSUP message */
	rc = rx_gsup_message(update_location_res, sizeof(update_location_res));
	OSMO_ASSERT(rc >= 0);
	OSMO_ASSERT(last_updated_subscr == s1);
	OSMO_ASSERT(s1->flags & GPRS_SUBSCRIBER_ENABLE_PURGE);
	OSMO_ASSERT(s1->sgsn_data->error_cause == SGSN_ERROR_CAUSE_NONE);
	OSMO_ASSERT(!llist_empty(&s1->sgsn_data->pdp_list));
	pdpd = llist_entry(s1->sgsn_data->pdp_list.next,
		struct sgsn_subscriber_pdp_data, list);
	OSMO_ASSERT(strcmp(pdpd->apn_str, "test.apn") == 0);
	pdpd = llist_entry(pdpd->list.next,
		struct sgsn_subscriber_pdp_data, list);
	OSMO_ASSERT(strcmp(pdpd->apn_str, "foo.apn") == 0);

	/* Check authorization */
	OSMO_ASSERT(s1->authorized == 1);

	/* Inject UpdateLocErr GSUP message */
	rc = rx_gsup_message(update_location_err, sizeof(update_location_err));
	OSMO_ASSERT(rc == -GMM_CAUSE_GPRS_NOTALLOWED);
	OSMO_ASSERT(last_updated_subscr == s1);
	OSMO_ASSERT(s1->sgsn_data->error_cause == GMM_CAUSE_GPRS_NOTALLOWED);

	/* Check authorization */
	OSMO_ASSERT(s1->authorized == 0);

	/* Inject InsertSubscrData GSUP message */
	last_updated_subscr = NULL;
	rc = rx_gsup_message(insert_data_req, sizeof(insert_data_req));
	OSMO_ASSERT(rc == -GMM_CAUSE_MSGT_NOTEXIST_NOTIMPL);
	OSMO_ASSERT(last_updated_subscr == NULL);

	/* Inject DeleteSubscrData GSUP message */
	last_updated_subscr = NULL;
	rc = rx_gsup_message(delete_data_req, sizeof(delete_data_req));
	OSMO_ASSERT(rc == -GMM_CAUSE_MSGT_NOTEXIST_NOTIMPL);
	OSMO_ASSERT(last_updated_subscr == NULL);

	/* Inject wrong LocCancelReq GSUP message */
	last_updated_subscr = NULL;
	rc = rx_gsup_message(location_cancellation_req_other,
			     sizeof(location_cancellation_req_other));
	OSMO_ASSERT(rc == -GMM_CAUSE_IMSI_UNKNOWN);
	OSMO_ASSERT(last_updated_subscr == NULL);

	/* Check cancellation result */
	OSMO_ASSERT(!(s1->flags & GPRS_SUBSCRIBER_CANCELLED));
	OSMO_ASSERT(s1->sgsn_data->mm != NULL);

	/* Inject LocCancelReq GSUP message */
	rc = rx_gsup_message(location_cancellation_req,
			     sizeof(location_cancellation_req));
	OSMO_ASSERT(rc >= 0);
	OSMO_ASSERT(last_updated_subscr == s1);
	OSMO_ASSERT(s1->sgsn_data->error_cause == SGSN_ERROR_CAUSE_NONE);

	/* Check cancellation result */
	OSMO_ASSERT(s1->flags & GPRS_SUBSCRIBER_CANCELLED);
	OSMO_ASSERT(s1->sgsn_data->mm == NULL);

	/* Inject LocCancelReq(withdraw) GSUP message */
	rc = rx_gsup_message(location_cancellation_req_withdraw,
			     sizeof(location_cancellation_req_withdraw));
	OSMO_ASSERT(rc >= 0);
	OSMO_ASSERT(s1->sgsn_data->error_cause == GMM_CAUSE_IMPL_DETACHED);

	/* Inject PurgeMsRes GSUP message */
	rc = rx_gsup_message(purge_ms_res,
			     sizeof(purge_ms_res));
	OSMO_ASSERT(rc >= 0);
	OSMO_ASSERT(!(s1->flags & GPRS_SUBSCRIBER_ENABLE_PURGE));

	/* Free MM context and subscriber */
	OSMO_ASSERT(ctx->subscr == NULL);
	sgsn_mm_ctx_cleanup_free(ctx);
	subscr_put(s1);
	s1found = gprs_subscr_get_by_imsi(imsi1);
	OSMO_ASSERT(s1found == NULL);

	/* Inject PurgeMsRes GSUP message */
	rc = rx_gsup_message(purge_ms_res,
			     sizeof(purge_ms_res));
	OSMO_ASSERT(rc >= 0);

	/* Inject PurgeMsErr(IMSI unknown in HLR) GSUP message */
	rc = rx_gsup_message(purge_ms_err,
			     sizeof(purge_ms_err));
	OSMO_ASSERT(rc == -GMM_CAUSE_IMSI_UNKNOWN);

	/* Inject PurgeMsErr() GSUP message */
	rc = rx_gsup_message(purge_ms_err_no_cause,
			     sizeof(purge_ms_err_no_cause));
	OSMO_ASSERT(rc == -GMM_CAUSE_NET_FAIL);

	/* Inject InsertSubscrData GSUP message (unknown IMSI) */
	last_updated_subscr = NULL;
	rc = rx_gsup_message(insert_data_req, sizeof(insert_data_req));
	OSMO_ASSERT(rc == -GMM_CAUSE_IMSI_UNKNOWN);
	OSMO_ASSERT(last_updated_subscr == NULL);

	/* Inject DeleteSubscrData GSUP message (unknown IMSI) */
	rc = rx_gsup_message(delete_data_req, sizeof(delete_data_req));
	OSMO_ASSERT(rc == -GMM_CAUSE_IMSI_UNKNOWN);
	OSMO_ASSERT(last_updated_subscr == NULL);

	/* Inject LocCancelReq GSUP message (unknown IMSI) */
	rc = rx_gsup_message(location_cancellation_req,
			     sizeof(location_cancellation_req));
	OSMO_ASSERT(rc == -GMM_CAUSE_IMSI_UNKNOWN);
	OSMO_ASSERT(last_updated_subscr == NULL);

	update_subscriber_data_cb = __real_sgsn_update_subscriber_data;
}

int my_gprs_gsup_client_send_dummy(struct gprs_gsup_client *gsupc, struct msgb *msg)
{
	msgb_free(msg);
	return 0;
};

/*
 * Test that a GMM Detach will remove the MMCTX and the
 * associated LLME.
 */
static void test_gmm_detach(void)
{
	struct gprs_ra_id raid = { 0, };
	struct sgsn_mm_ctx *ctx, *ictx;
	uint32_t local_tlli;

	printf("Testing GMM detach\n");

	/* DTAP - Detach Request (MO) */
	/* normal detach, power_off = 0 */
	static const unsigned char detach_req[] = {
		0x08, 0x05, 0x01, 0x18, 0x05, 0xf4, 0xef, 0xe2,
		0xb7, 0x00, 0x19, 0x03, 0xb9, 0x97, 0xcb
	};

	local_tlli = gprs_tmsi2tlli(0x23, TLLI_LOCAL);

	/* Create a context */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ctx = alloc_mm_ctx(local_tlli, &raid);

	/* inject the detach */
	send_0408_message(ctx->llme, local_tlli,
			  detach_req, ARRAY_SIZE(detach_req));

	/* verify that a single message (hopefully the Detach Accept) has been
	 * sent by the SGSN */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* verify that things are gone */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ictx = sgsn_mm_ctx_by_tlli(local_tlli, &raid);
	OSMO_ASSERT(!ictx);
}

/*
 * Test that a GMM Detach will remove the MMCTX and the associated LLME but
 * will not sent a Detach Accept message (power_off = 1)
 */
static void test_gmm_detach_power_off(void)
{
	struct gprs_ra_id raid = { 0, };
	struct sgsn_mm_ctx *ctx, *ictx;
	uint32_t local_tlli;

	printf("Testing GMM detach (power off)\n");

	/* DTAP - Detach Request (MO) */
	/* normal detach, power_off = 1 */
	static const unsigned char detach_req[] = {
		0x08, 0x05, 0x09, 0x18, 0x05, 0xf4, 0xef, 0xe2,
		0xb7, 0x00, 0x19, 0x03, 0xb9, 0x97, 0xcb
	};

	local_tlli = gprs_tmsi2tlli(0x23, TLLI_LOCAL);

	/* Create a context */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ctx = alloc_mm_ctx(local_tlli, &raid);

	/* inject the detach */
	send_0408_message(ctx->llme, local_tlli,
			  detach_req, ARRAY_SIZE(detach_req));

	/* verify that no message (and therefore no Detach Accept) has been
	 * sent by the SGSN */
	OSMO_ASSERT(sgsn_tx_counter == 0);

	/* verify that things are gone */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ictx = sgsn_mm_ctx_by_tlli(local_tlli, &raid);
	OSMO_ASSERT(!ictx);
}

/*
 * Test that a GMM Detach will remove the associated LLME if there is no MMCTX.
 */
static void test_gmm_detach_no_mmctx(void)
{
	struct gprs_llc_lle *lle;
	uint32_t local_tlli;

	printf("Testing GMM detach (no MMCTX)\n");

	/* DTAP - Detach Request (MO) */
	/* normal detach, power_off = 0 */
	static const unsigned char detach_req[] = {
		0x08, 0x05, 0x01, 0x18, 0x05, 0xf4, 0xef, 0xe2,
		0xb7, 0x00, 0x19, 0x03, 0xb9, 0x97, 0xcb
	};

	/* Create an LLME  */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	local_tlli = gprs_tmsi2tlli(0x23, TLLI_LOCAL);
	lle = gprs_lle_get_or_create(local_tlli, 3);

	OSMO_ASSERT(count(gprs_llme_list()) == 1);

	/* inject the detach */
	send_0408_message(lle->llme, local_tlli,
			  detach_req, ARRAY_SIZE(detach_req));

	/* verify that the LLME is gone */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
}

/*
 * Test that a single GMM Detach Accept message will not cause the SGSN to send
 * any message or leave an MM context at the SGSN.
 */
static void test_gmm_detach_accept_unexpected(void)
{
	struct gprs_llc_lle *lle;
	uint32_t local_tlli;

	printf("Testing GMM detach accept (unexpected)\n");

	/* DTAP - Detach Accept (MT) */
	/* normal detach */
	static const unsigned char detach_acc[] = {
		0x08, 0x06
	};

	/* Create an LLME  */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	local_tlli = gprs_tmsi2tlli(0x23, TLLI_LOCAL);
	lle = gprs_lle_get_or_create(local_tlli, 3);

	/* inject the detach */
	send_0408_message(lle->llme, local_tlli,
			  detach_acc, ARRAY_SIZE(detach_acc));

	/* verify that no message (and therefore no Status or XID reset) has been
	 * sent by the SGSN */
	OSMO_ASSERT(sgsn_tx_counter == 0);

	/* verify that things are gone */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
}

/*
 * Test that a GMM Status will remove the associated LLME if there is no MMCTX.
 */
static void test_gmm_status_no_mmctx(void)
{
	struct gprs_llc_lle *lle;
	uint32_t local_tlli;

	printf("Testing GMM Status (no MMCTX)\n");

	/* DTAP - GMM Status, protocol error */
	static const unsigned char gmm_status[] = {
		0x08, 0x20, 0x6f
	};

	/* Create an LLME  */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	local_tlli = gprs_tmsi2tlli(0x23, TLLI_LOCAL);
	lle = gprs_lle_get_or_create(local_tlli, 3);

	OSMO_ASSERT(count(gprs_llme_list()) == 1);

	/* inject the detach */
	send_0408_message(lle->llme, local_tlli,
			  gmm_status, ARRAY_SIZE(gmm_status));

	/* verify that no message has been sent by the SGSN */
	OSMO_ASSERT(sgsn_tx_counter == 0);

	/* verify that the LLME is gone */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
}

/*
 * Test the GMM Attach procedure
 */
static void test_gmm_attach(int retry)
{
	struct gprs_ra_id raid = { 0, };
	struct sgsn_mm_ctx *ctx = NULL;
	struct sgsn_mm_ctx *ictx;
	uint32_t ptmsi1;
	uint32_t foreign_tlli;
	uint32_t local_tlli = 0;
	struct gprs_llc_lle *lle;

	/* DTAP - Attach Request */
	/* The P-TMSI is not known by the SGSN */
	static const unsigned char attach_req[] = {
		0x08, 0x01, 0x02, 0xf5, 0xe0, 0x21, 0x08, 0x02, 0x05, 0xf4,
		0xfb, 0xc5, 0x46, 0x79, 0x11, 0x22, 0x33, 0x40, 0x50, 0x60,
		0x19, 0x18, 0xb3, 0x43, 0x2b, 0x25, 0x96, 0x62, 0x00, 0x60,
		0x80, 0x9a, 0xc2, 0xc6, 0x62, 0x00, 0x60, 0x80, 0xba, 0xc8,
		0xc6, 0x62, 0x00, 0x60, 0x80, 0x00
	};

	/* DTAP - Identity Response IMEI */
	static const unsigned char ident_resp_imei[] = {
		0x08, 0x16, 0x08, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x90, 0x78,
		0x56
	};

	/* DTAP - Identity Response IMSI */
	static const unsigned char ident_resp_imsi[] = {
		0x08, 0x16, 0x08, 0x19, 0x32, 0x54, 0x76, 0x98, 0x10, 0x32,
		0x54
	};

	/* DTAP - Authentication and Ciphering Resp */
	static const unsigned char auth_ciph_resp[] = {
		0x08, 0x13, 0x00, 0x22, 0x51, 0xe5, 0x51, 0xe5, 0x23, 0x09,
		0x9a, 0x78, 0x56, 0x34, 0x12, 0x90, 0x78, 0x56, 0x01
	};

	/* DTAP - Attach Complete */
	static const unsigned char attach_compl[] = {
		0x08, 0x03
	};

	/* DTAP - Detach Request (MO) */
	/* normal detach, power_off = 0 */
	static const unsigned char detach_req[] = {
		0x08, 0x05, 0x01, 0x18, 0x05, 0xf4, 0xeb, 0x8b,
		0x45, 0x67, 0x19, 0x03, 0xb9, 0x97, 0xcb
	};

	printf("Testing GMM attach%s\n", retry ? " with retry" : "");

	/* reset the PRNG used by sgsn_alloc_ptmsi */
	srand(1);

	ptmsi1 = sgsn_alloc_ptmsi();
	OSMO_ASSERT(ptmsi1 != GSM_RESERVED_TMSI);

	/* reset the PRNG, so that the same P-TMSI sequence will be generated
	 * again */
	srand(1);

	foreign_tlli = gprs_tmsi2tlli(0xc0000023, TLLI_FOREIGN);

	/* Create a LLE/LLME */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	lle = gprs_lle_get_or_create(foreign_tlli, 3);
	OSMO_ASSERT(count(gprs_llme_list()) == 1);

	/* inject the attach request */
	send_0408_message(lle->llme, foreign_tlli,
			  attach_req, ARRAY_SIZE(attach_req));

	ctx = sgsn_mm_ctx_by_tlli(foreign_tlli, &raid);
	OSMO_ASSERT(ctx != NULL);
	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);

	/* we expect an identity request (IMEI) */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* inject the identity response (IMEI) */
	send_0408_message(ctx->llme, foreign_tlli,
			  ident_resp_imei, ARRAY_SIZE(ident_resp_imei));

	/* we expect an identity request (IMSI) */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* inject the identity response (IMSI) */
	send_0408_message(ctx->llme, foreign_tlli,
			  ident_resp_imsi, ARRAY_SIZE(ident_resp_imsi));

	/* check that the MM context has not been removed due to a failed
	 * authorization */
	OSMO_ASSERT(ctx == sgsn_mm_ctx_by_tlli(foreign_tlli, &raid));

	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);

retry_attach_req:

	if (retry && sgsn_tx_counter == 0) {
		fprintf(stderr, "Retrying attach request\n");
		/* re-inject the attach request */
		send_0408_message(lle->llme, foreign_tlli,
				  attach_req, ARRAY_SIZE(attach_req));
	}

	if (ctx->auth_state == SGSN_AUTH_AUTHENTICATE && sgsn_tx_counter == 1) {
		/* we got an auth & ciph request */

		/* inject the auth & ciph response */
		send_0408_message(ctx->llme, foreign_tlli,
				  auth_ciph_resp, ARRAY_SIZE(auth_ciph_resp));

		/* check that the MM context has not been removed due to a
		 * failed authorization */
		OSMO_ASSERT(ctx == sgsn_mm_ctx_by_tlli(foreign_tlli, &raid));
	}

	if (retry && sgsn_tx_counter == 0)
		goto retry_attach_req;

	/* we expect an attach accept/reject */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* this has been randomly assigned by the SGSN */
	local_tlli = gprs_tmsi2tlli(ptmsi1, TLLI_LOCAL);

	/* inject the attach complete */
	send_0408_message(ctx->llme, local_tlli,
			  attach_compl, ARRAY_SIZE(attach_compl));

	OSMO_ASSERT(ctx->mm_state == GMM_REGISTERED_NORMAL);

	/* we don't expect a response */
	OSMO_ASSERT(sgsn_tx_counter == 0);

	/* inject the detach */
	send_0408_message(ctx->llme, local_tlli,
			  detach_req, ARRAY_SIZE(detach_req));

	/* verify that things are gone */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ictx = sgsn_mm_ctx_by_tlli(local_tlli, &raid);
	OSMO_ASSERT(!ictx);
}

static void test_gmm_attach_acl(void)
{
	const enum sgsn_auth_policy saved_auth_policy = sgsn->cfg.auth_policy;

	sgsn_inst.cfg.auth_policy = SGSN_AUTH_POLICY_CLOSED;
	sgsn_acl_add("123456789012345", &sgsn->cfg);
	printf("Auth policy 'closed': ");
	test_gmm_attach(0);
	sgsn_acl_del("123456789012345", &sgsn->cfg);

	sgsn->cfg.auth_policy = saved_auth_policy;
}

int my_subscr_request_update_location(struct sgsn_mm_ctx *mmctx) {
	int rc;
	rc = __real_gprs_subscr_request_update_location(mmctx);
	if (rc == -ENOTSUP) {
		OSMO_ASSERT(mmctx->subscr);
		gprs_subscr_update(mmctx->subscr);
	}
	return rc;
};

int my_subscr_request_auth_info(struct sgsn_mm_ctx *mmctx) {
	gprs_subscr_update(mmctx->subscr);
	return 0;
};

static void test_gmm_attach_subscr(void)
{
	const enum sgsn_auth_policy saved_auth_policy = sgsn->cfg.auth_policy;
	struct gsm_subscriber *subscr;

	sgsn_inst.cfg.auth_policy = SGSN_AUTH_POLICY_REMOTE;
	subscr_request_update_location_cb = my_subscr_request_update_location;
	subscr_request_auth_info_cb = my_subscr_request_auth_info;

	subscr = gprs_subscr_get_or_create("123456789012345");
	subscr->authorized = 1;

	printf("Auth policy 'remote': ");
	test_gmm_attach(0);
	subscr_put(subscr);
	assert_no_subscrs();

	sgsn->cfg.auth_policy = saved_auth_policy;
	subscr_request_update_location_cb = __real_gprs_subscr_request_update_location;
	subscr_request_auth_info_cb = __real_gprs_subscr_request_auth_info;
}

int my_subscr_request_auth_info_fake_auth(struct sgsn_mm_ctx *mmctx)
{
	/* Fake an authentication */
	OSMO_ASSERT(mmctx->subscr);
	mmctx->is_authenticated = 1;
	gprs_subscr_update_auth_info(mmctx->subscr);

	return 0;
};

static void test_gmm_attach_subscr_fake_auth(void)
{
	const enum sgsn_auth_policy saved_auth_policy = sgsn->cfg.auth_policy;
	struct gsm_subscriber *subscr;

	sgsn_inst.cfg.auth_policy = SGSN_AUTH_POLICY_REMOTE;
	subscr_request_update_location_cb = my_subscr_request_update_location;
	subscr_request_auth_info_cb = my_subscr_request_auth_info_fake_auth;

	subscr = gprs_subscr_get_or_create("123456789012345");
	subscr->authorized = 1;
	sgsn->cfg.require_authentication = 1;
	sgsn->cfg.require_update_location = 1;

	printf("Auth policy 'remote', auth faked: ");
	test_gmm_attach(0);
	subscr_put(subscr);
	assert_no_subscrs();

	sgsn->cfg.auth_policy = saved_auth_policy;
	subscr_request_update_location_cb = __real_gprs_subscr_request_update_location;
	subscr_request_auth_info_cb = __real_gprs_subscr_request_auth_info;
}

int my_subscr_request_auth_info_real_auth(struct sgsn_mm_ctx *mmctx)
{
	struct gsm_auth_tuple at = {
		.sres = {0x51, 0xe5, 0x51, 0xe5},
		.key_seq = 0
	};

	/* Fake an authentication */
	OSMO_ASSERT(mmctx->subscr);
	mmctx->subscr->sgsn_data->auth_triplets[0] = at;

	gprs_subscr_update_auth_info(mmctx->subscr);

	return 0;
};

static void test_gmm_attach_subscr_real_auth(void)
{
	const enum sgsn_auth_policy saved_auth_policy = sgsn->cfg.auth_policy;
	struct gsm_subscriber *subscr;

	sgsn_inst.cfg.auth_policy = SGSN_AUTH_POLICY_REMOTE;
	subscr_request_update_location_cb = my_subscr_request_update_location;
	subscr_request_auth_info_cb = my_subscr_request_auth_info_real_auth;

	subscr = gprs_subscr_get_or_create("123456789012345");
	subscr->authorized = 1;
	sgsn->cfg.require_authentication = 1;
	sgsn->cfg.require_update_location = 1;

	printf("Auth policy 'remote', triplet based auth: ");

	test_gmm_attach(0);
	subscr_put(subscr);
	assert_no_subscrs();

	sgsn->cfg.auth_policy = saved_auth_policy;
	subscr_request_update_location_cb = __real_gprs_subscr_request_update_location;
	subscr_request_auth_info_cb = __real_gprs_subscr_request_auth_info;
}

#define TEST_GSUP_IMSI_LONG_IE 0x01, 0x08, \
			       0x21, 0x43, 0x65, 0x87, 0x09, 0x21, 0x43, 0xf5

static int auth_info_skip = 0;
static int upd_loc_skip = 0;

int my_subscr_request_auth_info_gsup_auth(struct sgsn_mm_ctx *mmctx)
{
	static const uint8_t send_auth_info_res[] = {
		0x0a,
		TEST_GSUP_IMSI_LONG_IE,
		0x03, 0x22, /* Auth tuple */
			0x20, 0x10,
				0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
				0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
			0x21, 0x04,
				0x51, 0xe5, 0x51, 0xe5,
			0x22, 0x08,
				0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
	};

	OSMO_ASSERT(!mmctx || mmctx->subscr);

	if (auth_info_skip > 0) {
		auth_info_skip -= 1;
		return -EAGAIN;
	}

	/* Fake an SendAuthInfoRes */
	rx_gsup_message(send_auth_info_res, sizeof(send_auth_info_res));

	return 0;
};

int my_subscr_request_update_gsup_auth(struct sgsn_mm_ctx *mmctx) {
	static const uint8_t update_location_res[] = {
		0x06,
		TEST_GSUP_IMSI_LONG_IE,
		0x04, 0x00, /* PDP info complete */
		0x05, 0x12,
			0x10, 0x01, 0x01,
			0x11, 0x02, 0xf1, 0x21, /* IPv4 */
			0x12, 0x09, 0x04, 't', 'e', 's', 't', 0x03, 'a', 'p', 'n',
	};

	OSMO_ASSERT(!mmctx || mmctx->subscr);

	if (upd_loc_skip > 0) {
		upd_loc_skip -= 1;
		return -EAGAIN;
	}

	/* Fake an UpdateLocRes */
	return rx_gsup_message(update_location_res, sizeof(update_location_res));
};


static void test_gmm_attach_subscr_gsup_auth(int retry)
{
	const enum sgsn_auth_policy saved_auth_policy = sgsn->cfg.auth_policy;
	struct gsm_subscriber *subscr;

	sgsn_inst.cfg.auth_policy = SGSN_AUTH_POLICY_REMOTE;
	subscr_request_update_location_cb = my_subscr_request_update_gsup_auth;
	subscr_request_auth_info_cb = my_subscr_request_auth_info_gsup_auth;
	if (retry) {
		upd_loc_skip = 3;
		auth_info_skip = 3;
	}

	subscr = gprs_subscr_get_or_create("123456789012345");
	subscr->authorized = 1;
	sgsn->cfg.require_authentication = 1;
	sgsn->cfg.require_update_location = 1;
	subscr_put(subscr);

	printf("Auth policy 'remote', GSUP based auth: ");
	test_gmm_attach(retry);
	assert_no_subscrs();

	sgsn->cfg.auth_policy = saved_auth_policy;
	subscr_request_update_location_cb = __real_gprs_subscr_request_update_location;
	subscr_request_auth_info_cb = __real_gprs_subscr_request_auth_info;
	upd_loc_skip = 0;
	auth_info_skip = 0;
}

int my_gprs_gsup_client_send(struct gprs_gsup_client *gsupc, struct msgb *msg)
{
	struct gprs_gsup_message to_peer = {0};
	struct gprs_gsup_message from_peer = {0};
	struct msgb *reply_msg;
	int rc;

	/* Simulate the GSUP peer */
	rc = gprs_gsup_decode(msgb_data(msg), msgb_length(msg), &to_peer);
	OSMO_ASSERT(rc >= 0);
	OSMO_ASSERT(to_peer.imsi[0] != 0);
	strncpy(from_peer.imsi, to_peer.imsi, sizeof(from_peer.imsi));

	/* This invalidates the pointers in to_peer */
	msgb_free(msg);

	switch (to_peer.message_type) {
	case GPRS_GSUP_MSGT_UPDATE_LOCATION_REQUEST:
		/* Send UPDATE_LOCATION_RESULT */
		return my_subscr_request_update_gsup_auth(NULL);

	case GPRS_GSUP_MSGT_SEND_AUTH_INFO_REQUEST:
		/* Send SEND_AUTH_INFO_RESULT */
		return my_subscr_request_auth_info_gsup_auth(NULL);

	case GPRS_GSUP_MSGT_PURGE_MS_REQUEST:
		from_peer.message_type = GPRS_GSUP_MSGT_PURGE_MS_RESULT;
		break;

	default:
		if ((to_peer.message_type & 0b00000011) == 0) {
			/* Unhandled request */
			/* Send error(NOT_IMPL) */
			from_peer.message_type = to_peer.message_type + 1;
			from_peer.cause = GMM_CAUSE_MSGT_NOTEXIST_NOTIMPL;
			break;
		}

		/* Ignore it */
		return 0;
	}

	reply_msg = gprs_gsup_msgb_alloc();
	reply_msg->l2h = reply_msg->data;
	gprs_gsup_encode(reply_msg, &from_peer);
	gprs_subscr_rx_gsup_message(reply_msg);
	msgb_free(reply_msg);

	return 0;
};

static void test_gmm_attach_subscr_real_gsup_auth(int retry)
{
	const enum sgsn_auth_policy saved_auth_policy = sgsn->cfg.auth_policy;
	struct gsm_subscriber *subscr;

	sgsn_inst.cfg.auth_policy = SGSN_AUTH_POLICY_REMOTE;
	gprs_gsup_client_send_cb = my_gprs_gsup_client_send;

	sgsn->gsup_client = talloc_zero(tall_bsc_ctx, struct gprs_gsup_client);

	if (retry) {
		upd_loc_skip = 3;
		auth_info_skip = 3;
	}

	printf("Auth policy 'remote', real GSUP based auth: ");
	test_gmm_attach(retry);

	subscr = gprs_subscr_get_by_imsi("123456789012345");
	OSMO_ASSERT(subscr == NULL);
	assert_no_subscrs();

	sgsn->cfg.auth_policy = saved_auth_policy;
	gprs_gsup_client_send_cb = __real_gprs_gsup_client_send;
	upd_loc_skip = 0;
	auth_info_skip = 0;
	talloc_free(sgsn->gsup_client);
	sgsn->gsup_client = NULL;
}

/*
 * Test the GMM Rejects
 */
static void test_gmm_reject(void)
{
	struct gprs_ra_id raid = { 0, };
	struct sgsn_mm_ctx *ctx = NULL;
	uint32_t foreign_tlli;
	struct gprs_llc_lle *lle;
	int idx;

	/* DTAP - Attach Request */
	/* Invalid MI length */
	static const unsigned char attach_req_inv_mi_len[] = {
		0x08, 0x01, 0x02, 0xf5, 0xe0, 0x21, 0x08, 0x02, 0x09, 0xf4,
		0xfb, 0xc5, 0x46, 0x79, 0xff, 0xff, 0xff, 0xff, 0x11, 0x22,
		0x33, 0x40, 0x50, 0x60, 0x19, 0x18, 0xb3, 0x43, 0x2b, 0x25,
		0x96, 0x62, 0x00, 0x60, 0x80, 0x9a, 0xc2, 0xc6, 0x62, 0x00,
		0x60, 0x80, 0xba, 0xc8, 0xc6, 0x62, 0x00, 0x60, 0x80, 0x00
	};

	/* DTAP - Attach Request */
	/* Invalid MI type (IMEI) */
	static const unsigned char attach_req_inv_mi_type[] = {
		0x08, 0x01, 0x02, 0xf5, 0xe0, 0x21, 0x08, 0x02, 0x05, 0xf2,
		0xfb, 0xc5, 0x46, 0x79, 0x11, 0x22, 0x33, 0x40, 0x50, 0x60,
		0x19, 0x18, 0xb3, 0x43, 0x2b, 0x25, 0x96, 0x62, 0x00, 0x60,
		0x80, 0x9a, 0xc2, 0xc6, 0x62, 0x00, 0x60, 0x80, 0xba, 0xc8,
		0xc6, 0x62, 0x00, 0x60, 0x80, 0x00
	};

	/* DTAP - Routing Area Update Request */
	static const unsigned char dtap_ra_upd_req[] = {
		0x08, 0x08, 0x10, 0x11, 0x22, 0x33, 0x40, 0x50,
		0x60, 0x1d, 0x19, 0x13, 0x42, 0x33, 0x57, 0x2b,
		0xf7, 0xc8, 0x48, 0x02, 0x13, 0x48, 0x50, 0xc8,
		0x48, 0x02, 0x14, 0x48, 0x50, 0xc8, 0x48, 0x02,
		0x17, 0x49, 0x10, 0xc8, 0x48, 0x02, 0x00, 0x19,
		0x8b, 0xb2, 0x92, 0x17, 0x16, 0x27, 0x07, 0x04,
		0x31, 0x02, 0xe5, 0xe0, 0x32, 0x02, 0x20, 0x00
	};

	/* DTAP - Routing Area Update Request */
	/* Invalid type: GPRS_UPD_T_RA_LA_IMSI_ATT */
	static const unsigned char dtap_ra_upd_req_inv_type[] = {
		0x08, 0x08, 0x12, 0x11, 0x22, 0x33, 0x40, 0x50,
		0x60, 0x1d, 0x19, 0x13, 0x42, 0x33, 0x57, 0x2b,
		0xf7, 0xc8, 0x48, 0x02, 0x13, 0x48, 0x50, 0xc8,
		0x48, 0x02, 0x14, 0x48, 0x50, 0xc8, 0x48, 0x02,
		0x17, 0x49, 0x10, 0xc8, 0x48, 0x02, 0x00, 0x19,
		0x8b, 0xb2, 0x92, 0x17, 0x16, 0x27, 0x07, 0x04,
		0x31, 0x02, 0xe5, 0xe0, 0x32, 0x02, 0x20, 0x00
	};

	/* DTAP - Routing Area Update Request */
	/* Invalid cap length */
	static const unsigned char dtap_ra_upd_req_inv_cap_len[] = {
		0x08, 0x08, 0x10, 0x11, 0x22, 0x33, 0x40, 0x50,
		0x60, 0x3d, 0x19, 0x13, 0x42, 0x33, 0x57, 0x2b,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xf7, 0xc8, 0x48, 0x02, 0x13, 0x48, 0x50, 0xc8,
		0x48, 0x02, 0x14, 0x48, 0x50, 0xc8, 0x48, 0x02,
		0x17, 0x49, 0x10, 0xc8, 0x48, 0x02, 0x00, 0x19,
		0x8b, 0xb2, 0x92, 0x17, 0x16, 0x27, 0x07, 0x04,
		0x31, 0x02, 0xe5, 0xe0, 0x32, 0x02, 0x20, 0x00
	};

	struct test {
		const char *title;
		const unsigned char *msg;
		unsigned msg_len;
		unsigned num_resp;

	};
	static struct test tests[] = {
		{
			.title    = "Attach Request (invalid MI length)",
			.msg      = attach_req_inv_mi_len,
			.msg_len  = sizeof(attach_req_inv_mi_len),
			.num_resp = 1 /* Reject */

		},
		{
			.title   = "Attach Request (invalid MI type)",
			.msg     = attach_req_inv_mi_type,
			.msg_len = sizeof(attach_req_inv_mi_type),
			.num_resp = 1 /* Reject */
		},
		{
			.title   = "Routing Area Update Request (valid)",
			.msg     = dtap_ra_upd_req,
			.msg_len = sizeof(dtap_ra_upd_req),
			.num_resp = 2 /* XID Reset + Reject */
		},
		{
			.title   = "Routing Area Update Request (invalid type)",
			.msg     = dtap_ra_upd_req_inv_type,
			.msg_len = sizeof(dtap_ra_upd_req_inv_type),
			.num_resp = 1 /* Reject */
		},
		{
			.title   = "Routing Area Update Request (invalid CAP length)",
			.msg     = dtap_ra_upd_req_inv_cap_len,
			.msg_len = sizeof(dtap_ra_upd_req_inv_cap_len),
			.num_resp = 1 /* Reject */
		},
	};

	printf("Testing GMM reject\n");

	/* reset the PRNG used by sgsn_alloc_ptmsi */
	srand(1);

	foreign_tlli = gprs_tmsi2tlli(0xc0000023, TLLI_FOREIGN);

	OSMO_ASSERT(count(gprs_llme_list()) == 0);

	for (idx = 0; idx < ARRAY_SIZE(tests); idx++) {
		const struct test *test = &tests[idx];
		printf("  - %s\n", test->title);

		/* Create a LLE/LLME */
		lle = gprs_lle_get_or_create(foreign_tlli, 3);
		OSMO_ASSERT(count(gprs_llme_list()) == 1);

		/* Inject the Request message */
		send_0408_message(lle->llme, foreign_tlli,
				  test->msg, test->msg_len);

		/* We expect a Reject message */
		fprintf(stderr, "sgsn_tx_counter = %d (expected %d)\n",
			sgsn_tx_counter, test->num_resp);
		OSMO_ASSERT(sgsn_tx_counter == test->num_resp);

		/* verify that LLME/MM are removed */
		ctx = sgsn_mm_ctx_by_tlli(foreign_tlli, &raid);
		OSMO_ASSERT(ctx == NULL);
		OSMO_ASSERT(count(gprs_llme_list()) == 0);
	}
}

/*
 * Test cancellation of attached MM contexts
 */
static void test_gmm_cancel(void)
{
	struct gprs_ra_id raid = { 0, };
	struct sgsn_mm_ctx *ctx = NULL;
	struct sgsn_mm_ctx *ictx;
	uint32_t ptmsi1;
	uint32_t foreign_tlli;
	uint32_t local_tlli = 0;
	struct gprs_llc_lle *lle;
	const enum sgsn_auth_policy saved_auth_policy = sgsn->cfg.auth_policy;

	/* DTAP - Attach Request */
	/* The P-TMSI is not known by the SGSN */
	static const unsigned char attach_req[] = {
		0x08, 0x01, 0x02, 0xf5, 0xe0, 0x21, 0x08, 0x02, 0x05, 0xf4,
		0xfb, 0xc5, 0x46, 0x79, 0x11, 0x22, 0x33, 0x40, 0x50, 0x60,
		0x19, 0x18, 0xb3, 0x43, 0x2b, 0x25, 0x96, 0x62, 0x00, 0x60,
		0x80, 0x9a, 0xc2, 0xc6, 0x62, 0x00, 0x60, 0x80, 0xba, 0xc8,
		0xc6, 0x62, 0x00, 0x60, 0x80, 0x00
	};

	/* DTAP - Identity Response IMEI */
	static const unsigned char ident_resp_imei[] = {
		0x08, 0x16, 0x08, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x90, 0x78,
		0x56
	};

	/* DTAP - Identity Response IMSI */
	static const unsigned char ident_resp_imsi[] = {
		0x08, 0x16, 0x08, 0x19, 0x32, 0x54, 0x76, 0x98, 0x10, 0x32,
		0x54
	};

	/* DTAP - Attach Complete */
	static const unsigned char attach_compl[] = {
		0x08, 0x03
	};

	printf("Testing cancellation\n");

	sgsn_inst.cfg.auth_policy = SGSN_AUTH_POLICY_OPEN;

	/* reset the PRNG used by sgsn_alloc_ptmsi */
	srand(1);

	ptmsi1 = sgsn_alloc_ptmsi();
	OSMO_ASSERT(ptmsi1 != GSM_RESERVED_TMSI);

	/* reset the PRNG, so that the same P-TMSI sequence will be generated
	 * again */
	srand(1);

	foreign_tlli = gprs_tmsi2tlli(0xc0000023, TLLI_FOREIGN);

	/* Create a LLE/LLME */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	lle = gprs_lle_get_or_create(foreign_tlli, 3);
	OSMO_ASSERT(count(gprs_llme_list()) == 1);

	/* inject the attach request */
	send_0408_message(lle->llme, foreign_tlli,
			  attach_req, ARRAY_SIZE(attach_req));

	ctx = sgsn_mm_ctx_by_tlli(foreign_tlli, &raid);
	OSMO_ASSERT(ctx != NULL);
	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);

	/* we expect an identity request (IMEI) */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* inject the identity response (IMEI) */
	send_0408_message(ctx->llme, foreign_tlli,
			  ident_resp_imei, ARRAY_SIZE(ident_resp_imei));

	/* we expect an identity request (IMSI) */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* inject the identity response (IMSI) */
	send_0408_message(ctx->llme, foreign_tlli,
			  ident_resp_imsi, ARRAY_SIZE(ident_resp_imsi));

	/* check that the MM context has not been removed due to a failed
	 * authorization */
	OSMO_ASSERT(ctx == sgsn_mm_ctx_by_tlli(foreign_tlli, &raid));

	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);

	/* we expect an attach accept/reject */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* this has been randomly assigned by the SGSN */
	local_tlli = gprs_tmsi2tlli(ptmsi1, TLLI_LOCAL);

	/* inject the attach complete */
	send_0408_message(ctx->llme, local_tlli,
			  attach_compl, ARRAY_SIZE(attach_compl));

	OSMO_ASSERT(ctx->mm_state == GMM_REGISTERED_NORMAL);

	/* we don't expect a response */
	OSMO_ASSERT(sgsn_tx_counter == 0);

	/* cancel */
	gsm0408_gprs_access_cancelled(ctx, 0);

	/* verify that things are gone */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ictx = sgsn_mm_ctx_by_tlli(local_tlli, &raid);
	OSMO_ASSERT(!ictx);

	sgsn->cfg.auth_policy = saved_auth_policy;
}

/*
 * Test the dynamic allocation of P-TMSIs
 */
static void test_gmm_ptmsi_allocation(void)
{
	struct gprs_ra_id raid = { 0, };
	struct sgsn_mm_ctx *ctx = NULL;
	struct sgsn_mm_ctx *ictx;
	uint32_t foreign_tlli;
	uint32_t ptmsi1;
	uint32_t ptmsi2;
	uint32_t old_ptmsi;
	uint32_t local_tlli = 0;
	struct gprs_llc_lle *lle;
	const enum sgsn_auth_policy saved_auth_policy = sgsn->cfg.auth_policy;

	/* DTAP - Attach Request (IMSI 12131415161718) */
	static const unsigned char attach_req[] = {
		0x08, 0x01, 0x02, 0xf5, 0xe0, 0x21, 0x08, 0x02,
		0x08, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
		0x18, 0x11, 0x22, 0x33, 0x40, 0x50, 0x60, 0x19,
		0x18, 0xb3, 0x43, 0x2b, 0x25, 0x96, 0x62, 0x00,
		0x60, 0x80, 0x9a, 0xc2, 0xc6, 0x62, 0x00, 0x60,
		0x80, 0xba, 0xc8, 0xc6, 0x62, 0x00, 0x60, 0x80,
		0x00,
	};

	/* DTAP - Identity Response IMEI */
	static const unsigned char ident_resp_imei[] = {
		0x08, 0x16, 0x08, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x90, 0x78,
		0x56
	};

	/* DTAP - Attach Complete */
	static const unsigned char attach_compl[] = {
		0x08, 0x03
	};

	/* DTAP - Routing Area Update Request */
	static const unsigned char ra_upd_req[] = {
		0x08, 0x08, 0x10, 0x11, 0x22, 0x33, 0x40, 0x50,
		0x60, 0x1d, 0x19, 0x13, 0x42, 0x33, 0x57, 0x2b,
		0xf7, 0xc8, 0x48, 0x02, 0x13, 0x48, 0x50, 0xc8,
		0x48, 0x02, 0x14, 0x48, 0x50, 0xc8, 0x48, 0x02,
		0x17, 0x49, 0x10, 0xc8, 0x48, 0x02, 0x00, 0x19,
		0x8b, 0xb2, 0x92, 0x17, 0x16, 0x27, 0x07, 0x04,
		0x31, 0x02, 0xe5, 0xe0, 0x32, 0x02, 0x20, 0x00
	};

	/* DTAP - Routing Area Update Complete */
	static const unsigned char ra_upd_complete[] = {
		0x08, 0x0a
	};

	/* DTAP - Detach Request (MO) */
	/* normal detach, power_off = 1 */
	static const unsigned char detach_req[] = {
		0x08, 0x05, 0x09, 0x18, 0x05, 0xf4, 0xef, 0xe2,
		0xb7, 0x00, 0x19, 0x03, 0xb9, 0x97, 0xcb
	};

	sgsn->cfg.auth_policy = SGSN_AUTH_POLICY_OPEN;

	printf("Testing P-TMSI allocation\n");

	printf("  - sgsn_alloc_ptmsi\n");

	/* reset the PRNG used by sgsn_alloc_ptmsi */
	srand(1);

	ptmsi1 = sgsn_alloc_ptmsi();
	OSMO_ASSERT(ptmsi1 != GSM_RESERVED_TMSI);

	ptmsi2 = sgsn_alloc_ptmsi();
	OSMO_ASSERT(ptmsi2 != GSM_RESERVED_TMSI);

	OSMO_ASSERT(ptmsi1 != ptmsi2);

	printf("  - Repeated Attach Request\n");

	/* reset the PRNG, so that the same P-TMSI will be generated
	 * again */
	srand(1);

	foreign_tlli = gprs_tmsi2tlli(0xc0000023, TLLI_FOREIGN);

	/* Create a LLE/LLME */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	lle = gprs_lle_get_or_create(foreign_tlli, 3);
	OSMO_ASSERT(count(gprs_llme_list()) == 1);

	/* inject the attach request */
	send_0408_message(lle->llme, foreign_tlli,
			  attach_req, ARRAY_SIZE(attach_req));

	ctx = sgsn_mm_ctx_by_tlli(foreign_tlli, &raid);
	OSMO_ASSERT(ctx != NULL);
	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);
	OSMO_ASSERT(ctx->p_tmsi == ptmsi1);

	old_ptmsi = ctx->p_tmsi_old;

	/* we expect an identity request (IMEI) */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* inject the identity response (IMEI) */
	send_0408_message(ctx->llme, foreign_tlli,
			  ident_resp_imei, ARRAY_SIZE(ident_resp_imei));

	/* check that the MM context has not been removed due to a failed
	 * authorization */
	OSMO_ASSERT(ctx == sgsn_mm_ctx_by_tlli(foreign_tlli, &raid));

	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);
	OSMO_ASSERT(ctx->p_tmsi == ptmsi1);

	/* we expect an attach accept */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	/* we ignore this and send the attach again */
	send_0408_message(lle->llme, foreign_tlli,
			  attach_req, ARRAY_SIZE(attach_req));

	/* the allocated P-TMSI should be the same */
	ctx = sgsn_mm_ctx_by_tlli(foreign_tlli, &raid);
	OSMO_ASSERT(ctx != NULL);
	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);
	OSMO_ASSERT(ctx->p_tmsi_old == old_ptmsi);
	OSMO_ASSERT(ctx->p_tmsi == ptmsi1);

	/* inject the attach complete */
	local_tlli = gprs_tmsi2tlli(ptmsi1, TLLI_LOCAL);
	send_0408_message(ctx->llme, local_tlli,
			  attach_compl, ARRAY_SIZE(attach_compl));

	/* we don't expect a response */
	OSMO_ASSERT(sgsn_tx_counter == 0);

	OSMO_ASSERT(ctx->mm_state == GMM_REGISTERED_NORMAL);
	OSMO_ASSERT(ctx->p_tmsi_old == 0);
	OSMO_ASSERT(ctx->p_tmsi == ptmsi1);

	printf("  - Repeated RA Update Request\n");

	/* inject the RA update request */
	send_0408_message(ctx->llme, local_tlli,
			  ra_upd_req, ARRAY_SIZE(ra_upd_req));

	/* we expect an RA update accept */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);
	OSMO_ASSERT(ctx->p_tmsi_old == ptmsi1);
	OSMO_ASSERT(ctx->p_tmsi == ptmsi2);

	/* repeat the RA update request */
	send_0408_message(ctx->llme, local_tlli,
			  ra_upd_req, ARRAY_SIZE(ra_upd_req));

	/* we expect an RA update accept */
	OSMO_ASSERT(sgsn_tx_counter == 1);

	OSMO_ASSERT(ctx->mm_state == GMM_COMMON_PROC_INIT);
	OSMO_ASSERT(ctx->p_tmsi_old == ptmsi1);
	OSMO_ASSERT(ctx->p_tmsi == ptmsi2);

	/* inject the RA update complete */
	local_tlli = gprs_tmsi2tlli(ptmsi2, TLLI_LOCAL);
	send_0408_message(ctx->llme, local_tlli,
			  ra_upd_complete, ARRAY_SIZE(ra_upd_complete));

	/* we don't expect a response */
	OSMO_ASSERT(sgsn_tx_counter == 0);

	OSMO_ASSERT(ctx->mm_state == GMM_REGISTERED_NORMAL);
	OSMO_ASSERT(ctx->p_tmsi_old == 0);
	OSMO_ASSERT(ctx->p_tmsi == ptmsi2);

	/* inject the detach */
	send_0408_message(ctx->llme, local_tlli,
			  detach_req, ARRAY_SIZE(detach_req));

	/* verify that things are gone */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ictx = sgsn_mm_ctx_by_tlli(local_tlli, &raid);
	OSMO_ASSERT(!ictx);

	sgsn->cfg.auth_policy = saved_auth_policy;
}

static void test_apn_matching(void)
{
	struct apn_ctx *actx, *actxs[9];

	printf("Testing APN matching\n");

	actxs[0] = sgsn_apn_ctx_find_alloc("*.test", "");
	actxs[1] = sgsn_apn_ctx_find_alloc("*.def.test", "");
	actxs[2] = sgsn_apn_ctx_find_alloc("abc.def.test", "");
	actxs[3] = NULL;

	actxs[4] = sgsn_apn_ctx_find_alloc("abc.def.test", "456");
	actxs[5] = sgsn_apn_ctx_find_alloc("abc.def.test", "456123");
	actxs[6] = sgsn_apn_ctx_find_alloc("*.def.test", "456");
	actxs[7] = sgsn_apn_ctx_find_alloc("*.def.test", "456123");

	actxs[8] = sgsn_apn_ctx_find_alloc("ghi.def.test", "456");

	actx = sgsn_apn_ctx_match("abc.def.test", "12345678");
	OSMO_ASSERT(actx == actxs[2]);
	actx = sgsn_apn_ctx_match("aBc.dEf.test", "12345678");
	OSMO_ASSERT(actx == actxs[2]);
	actx = sgsn_apn_ctx_match("xyz.def.test", "12345678");
	OSMO_ASSERT(actx == actxs[1]);
	actx = sgsn_apn_ctx_match("xyz.dEf.test", "12345678");
	OSMO_ASSERT(actx == actxs[1]);
	actx = sgsn_apn_ctx_match("xyz.uvw.test", "12345678");
	OSMO_ASSERT(actx == actxs[0]);
	actx = sgsn_apn_ctx_match("xyz.uvw.foo", "12345678");
	OSMO_ASSERT(actx == NULL);

	actxs[3] = sgsn_apn_ctx_find_alloc("*", "");
	actx = sgsn_apn_ctx_match("xyz.uvw.foo", "12345678");
	OSMO_ASSERT(actx == actxs[3]);

	actx = sgsn_apn_ctx_match("abc.def.test", "45699900");
	OSMO_ASSERT(actx == actxs[4]);

	actx = sgsn_apn_ctx_match("xyz.def.test", "45699900");
	OSMO_ASSERT(actx == actxs[6]);

	actx = sgsn_apn_ctx_match("abc.def.test", "45612300");
	OSMO_ASSERT(actx == actxs[5]);

	actx = sgsn_apn_ctx_match("xyz.def.test", "45612300");
	OSMO_ASSERT(actx == actxs[7]);

	actx = sgsn_apn_ctx_match("ghi.def.test", "45699900");
	OSMO_ASSERT(actx == actxs[8]);

	actx = sgsn_apn_ctx_match("ghi.def.test", "45612300");
	OSMO_ASSERT(actx == actxs[7]);

	/* Free APN contexts and check how the matching changes */

	sgsn_apn_ctx_free(actxs[7]);
	actx = sgsn_apn_ctx_match("ghi.def.test", "45612300");
	OSMO_ASSERT(actx == actxs[8]);

	sgsn_apn_ctx_free(actxs[8]);
	actx = sgsn_apn_ctx_match("ghi.def.test", "45612300");
	OSMO_ASSERT(actx == actxs[6]);

	sgsn_apn_ctx_free(actxs[6]);
	actx = sgsn_apn_ctx_match("ghi.def.test", "45612300");
	OSMO_ASSERT(actx == actxs[1]);

	sgsn_apn_ctx_free(actxs[5]);
	actx = sgsn_apn_ctx_match("abc.def.test", "45612300");
	OSMO_ASSERT(actx == actxs[4]);

	sgsn_apn_ctx_free(actxs[4]);
	actx = sgsn_apn_ctx_match("abc.def.test", "45612300");
	OSMO_ASSERT(actx == actxs[2]);

	sgsn_apn_ctx_free(actxs[2]);
	actx = sgsn_apn_ctx_match("abc.def.test", "12345678");
	OSMO_ASSERT(actx == actxs[1]);

	sgsn_apn_ctx_free(actxs[1]);
	actx = sgsn_apn_ctx_match("abc.def.test", "12345678");
	OSMO_ASSERT(actx == actxs[0]);

	sgsn_apn_ctx_free(actxs[0]);
	actx = sgsn_apn_ctx_match("abc.def.test", "12345678");
	OSMO_ASSERT(actx == actxs[3]);

	sgsn_apn_ctx_free(actxs[3]);
	actx = sgsn_apn_ctx_match("abc.def.test", "12345678");
	OSMO_ASSERT(actx == NULL);
}

struct sgsn_subscriber_pdp_data* sgsn_subscriber_pdp_data_alloc(
	struct sgsn_subscriber_data *sdata);

static void test_ggsn_selection(void)
{
	struct apn_ctx *actxs[4];
	struct sgsn_ggsn_ctx *ggc, *ggcs[3];
	struct gsm_subscriber *s1;
	const char *imsi1 = "1234567890";
	struct sgsn_mm_ctx *ctx;
	struct gprs_ra_id raid = { 0, };
	uint32_t local_tlli = 0xffeeddcc;
	enum gsm48_gsm_cause gsm_cause;
	struct tlv_parsed tp;
	uint8_t apn_enc[GSM_APN_LENGTH + 10];
	struct sgsn_subscriber_pdp_data *pdp_data;

	printf("Testing GGSN selection\n");

	gprs_gsup_client_send_cb = my_gprs_gsup_client_send_dummy;

	/* Check for emptiness */
	OSMO_ASSERT(gprs_subscr_get_by_imsi(imsi1) == NULL);

	/* Create a context */
	OSMO_ASSERT(count(gprs_llme_list()) == 0);
	ctx = alloc_mm_ctx(local_tlli, &raid);
	strncpy(ctx->imsi, imsi1, sizeof(ctx->imsi) - 1);

	/* Allocate and attach a subscriber */
	s1 = gprs_subscr_get_or_create_by_mmctx(ctx);
	assert_subscr(s1, imsi1);

	tp.lv[GSM48_IE_GSM_APN].len = 0;
	tp.lv[GSM48_IE_GSM_APN].val = apn_enc;

	/* TODO: Add PDP info entries to s1 */

	ggcs[0] = sgsn_ggsn_ctx_find_alloc(0);
	ggcs[1] = sgsn_ggsn_ctx_find_alloc(1);
	ggcs[2] = sgsn_ggsn_ctx_find_alloc(2);

	actxs[0] = sgsn_apn_ctx_find_alloc("test.apn", "123456");
	actxs[0]->ggsn = ggcs[0];
	actxs[1] = sgsn_apn_ctx_find_alloc("*.apn", "123456");
	actxs[1]->ggsn = ggcs[1];
	actxs[2] = sgsn_apn_ctx_find_alloc("*", "456789");
	actxs[2]->ggsn = ggcs[2];

	/* Resolve GGSNs */

	tp.lv[GSM48_IE_GSM_APN].len =
		gprs_str_to_apn(apn_enc, sizeof(apn_enc), "Test.Apn");

	ggc = sgsn_mm_ctx_find_ggsn_ctx(ctx, &tp, &gsm_cause);
	OSMO_ASSERT(ggc != NULL);
	OSMO_ASSERT(ggc->id == 0);

	tp.lv[GSM48_IE_GSM_APN].len =
		gprs_str_to_apn(apn_enc, sizeof(apn_enc), "Other.Apn");

	ggc = sgsn_mm_ctx_find_ggsn_ctx(ctx, &tp, &gsm_cause);
	OSMO_ASSERT(ggc != NULL);
	OSMO_ASSERT(ggc->id == 1);

	tp.lv[GSM48_IE_GSM_APN].len = 0;
	tp.lv[GSM48_IE_GSM_APN].val = NULL;

	ggc = sgsn_mm_ctx_find_ggsn_ctx(ctx, &tp, &gsm_cause);
	OSMO_ASSERT(ggc != NULL);
	OSMO_ASSERT(ggc->id == 0);

	actxs[3] = sgsn_apn_ctx_find_alloc("*", "123456");
	actxs[3]->ggsn = ggcs[2];
	ggc = sgsn_mm_ctx_find_ggsn_ctx(ctx, &tp, &gsm_cause);
	OSMO_ASSERT(ggc != NULL);
	OSMO_ASSERT(ggc->id == 2);

	sgsn_apn_ctx_free(actxs[3]);
	tp.lv[GSM48_IE_GSM_APN].val = apn_enc;

	tp.lv[GSM48_IE_GSM_APN].len =
		gprs_str_to_apn(apn_enc, sizeof(apn_enc), "Foo.Bar");

	ggc = sgsn_mm_ctx_find_ggsn_ctx(ctx, &tp, &gsm_cause);
	OSMO_ASSERT(ggc == NULL);
	OSMO_ASSERT(gsm_cause == GSM_CAUSE_MISSING_APN);

	tp.lv[GSM48_IE_GSM_APN].len = sizeof(apn_enc);
	ggc = sgsn_mm_ctx_find_ggsn_ctx(ctx, &tp, &gsm_cause);
	OSMO_ASSERT(ggc == NULL);
	OSMO_ASSERT(gsm_cause == GSM_CAUSE_INV_MAND_INFO);

	/* Add PDP data entry to subscriber */

	pdp_data = sgsn_subscriber_pdp_data_alloc(s1->sgsn_data);
	pdp_data->context_id = 1;

	pdp_data->pdp_type = 0x0121;
	strncpy(pdp_data->apn_str, "Test.Apn", sizeof(pdp_data->apn_str)-1);

	tp.lv[GSM48_IE_GSM_APN].len =
		gprs_str_to_apn(apn_enc, sizeof(apn_enc), "Test.Apn");

	ggc = sgsn_mm_ctx_find_ggsn_ctx(ctx, &tp, &gsm_cause);
	OSMO_ASSERT(ggc != NULL);
	OSMO_ASSERT(ggc->id == 0);

	tp.lv[GSM48_IE_GSM_APN].len =
		gprs_str_to_apn(apn_enc, sizeof(apn_enc), "Other.Apn");

	ggc = sgsn_mm_ctx_find_ggsn_ctx(ctx, &tp, &gsm_cause);
	OSMO_ASSERT(ggc == NULL);
	OSMO_ASSERT(gsm_cause == GSM_CAUSE_REQ_SERV_OPT_NOTSUB);

	/* Cleanup */

	subscr_put(s1);
	sgsn_mm_ctx_cleanup_free(ctx);

	assert_no_subscrs();

	sgsn_apn_ctx_free(actxs[0]);
	sgsn_apn_ctx_free(actxs[1]);
	sgsn_apn_ctx_free(actxs[2]);

	sgsn_ggsn_ctx_free(ggcs[0]);
	sgsn_ggsn_ctx_free(ggcs[1]);
	sgsn_ggsn_ctx_free(ggcs[2]);

	gprs_gsup_client_send_cb = __real_gprs_gsup_client_send;
}

static struct log_info_cat gprs_categories[] = {
	[DMM] = {
		.name = "DMM",
		.description = "Layer3 Mobility Management (MM)",
		.color = "\033[1;33m",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DPAG]	= {
		.name = "DPAG",
		.description = "Paging Subsystem",
		.color = "\033[1;38m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DMEAS] = {
		.name = "DMEAS",
		.description = "Radio Measurement Processing",
		.enabled = 0, .loglevel = LOGL_NOTICE,
	},
	[DREF] = {
		.name = "DREF",
		.description = "Reference Counting",
		.enabled = 0, .loglevel = LOGL_NOTICE,
	},
	[DGPRS] = {
		.name = "DGPRS",
		.description = "GPRS Packet Service",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DNS] = {
		.name = "DNS",
		.description = "GPRS Network Service (NS)",
		.enabled = 1, .loglevel = LOGL_INFO,
	},
	[DBSSGP] = {
		.name = "DBSSGP",
		.description = "GPRS BSS Gateway Protocol (BSSGP)",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DLLC] = {
		.name = "DLLC",
		.description = "GPRS Logical Link Control Protocol (LLC)",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
	[DSNDCP] = {
		.name = "DSNDCP",
		.description = "GPRS Sub-Network Dependent Control Protocol (SNDCP)",
		.enabled = 1, .loglevel = LOGL_DEBUG,
	},
};

static struct log_info info = {
	.cat = gprs_categories,
	.num_cat = ARRAY_SIZE(gprs_categories),
};

int main(int argc, char **argv)
{
	void *osmo_sgsn_ctx;

	osmo_init_logging(&info);
	osmo_sgsn_ctx = talloc_named_const(NULL, 0, "osmo_sgsn");
	tall_bsc_ctx = talloc_named_const(osmo_sgsn_ctx, 0, "bsc");
	tall_msgb_ctx = talloc_named_const(osmo_sgsn_ctx, 0, "msgb");

	sgsn_auth_init();
	gprs_subscr_init(sgsn);

	test_llme();
	test_subscriber();
	test_auth_triplets();
	test_subscriber_gsup();
	test_gmm_detach();
	test_gmm_detach_power_off();
	test_gmm_detach_no_mmctx();
	test_gmm_detach_accept_unexpected();
	test_gmm_status_no_mmctx();
	test_gmm_attach_acl();
	test_gmm_attach_subscr();
	test_gmm_attach_subscr_fake_auth();
	test_gmm_attach_subscr_real_auth();
	test_gmm_attach_subscr_gsup_auth(0);
	test_gmm_attach_subscr_gsup_auth(1);
	test_gmm_attach_subscr_real_gsup_auth(0);
	test_gmm_reject();
	test_gmm_cancel();
	test_gmm_ptmsi_allocation();
	test_apn_matching();
	test_ggsn_selection();
	printf("Done\n");

	talloc_report_full(osmo_sgsn_ctx, stderr);
	OSMO_ASSERT(talloc_total_blocks(tall_msgb_ctx) == 1);
	OSMO_ASSERT(talloc_total_blocks(tall_bsc_ctx) == 1);
	return 0;
}


/* stubs */
struct osmo_prim_hdr;
int bssgp_prim_cb(struct osmo_prim_hdr *oph, void *ctx)
{
	abort();
}
