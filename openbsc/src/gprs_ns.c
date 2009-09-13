/* GPRS Networks Service (NS) messages on the Gb interface
 * 3GPP TS 08.16 version 8.0.1 Release 1999 / ETSI TS 101 299 V8.0.1 (2002-05) */

/* (C) 2009 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <openbsc/msgb.h>
#include <openbsc/talloc.h>
#include <openbsc/gprs_ns.h>

#define NS_ALLOC_SIZE	1024

/* a layer 1 entity transporting NS frames */
struct gprs_ns_link {
	union {
		struct {
			int fd;
		} ip;
	};
};

static int gprs_ns_tx_simple(struct gprs_ns_link *link, u_int8_t pdu_type)
{
	struct msgb *msg = msgb_alloc(NS_ALLOC_SIZE);
	struct gprs_ns_hdr *nsh;

	if (!msgb)
		return -ENOMEM;

	nsh = msgb_put(msg, sizeof(*nsh));

	nsh->pdu_type = pdu_type;

	/* FIXME: actually transmit */
}

static int gprs_ns_rx_unitdata(struct msgb *msg)
{
	struct gprs_ns_hdr *nsh = msgb->l2h;
	u_int16_t bvci;

	bvci = nsh->data[1] << 8 | nsh->data[2];
	msgb->l3h = &nsh->data[3];

	/* call upper layer (BSSGP) */
	return bssgp_rcvmsg(msg, bvc);
}

/* main entry point, here incoming NS frames enter */
int gprs_ns_rcvmsg(struct msgb *msg)
{
	struct gprs_ns_hdr *nsh = msgb->l2h;
	int rc = -EINVAL;

	switch (nsh->pdu_type) {
	case NS_PDUT_ALIVE:
		/* remote end inquires whether we're still alive,
		 * we need to respond with ALIVE_ACK */
		rc = gprs_ns_tx_simple(link, NS_PDUT_ALIVE_ACK);
		break;
	case NS_PDUT_ALIVE_ACK:
		/* simply ignore it for now */
		break;
	case NS_PDUT_UNITDATA:
		rc = gprs_ns_rx_unitdata(msg);
		/* actual user data */
		break;
	case NS_PDUT_RESET:
	case NS_PDUT_RESET_ACK:
	case NS_PDUT_BLOCK:
	case NS_PDUT_BLOCK_ACK:
	case NS_PDUT_UNBLOCK:
	case NS_PDUT_UNBLOCK_ACK:
	case NS_PDUT_STATUS:
		rc = 0;
		break;
	default:
		break;
	}
	return rc;
}

