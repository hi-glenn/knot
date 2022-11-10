/*  Copyright (C) 2022 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "contrib/net.h"
#include "contrib/time.h"
#include "knot/query/quic-requestor.h"
#include "libknot/endian.h"
#include "libknot/error.h"
#include "libknot/xdp/eth.h"
#include "libknot/xdp/quic.h"

#define QUIC_MAX_SEND_PER_RECV	4 // NOTE: also in xdp-handler.c
#define QUIC_BUF_SIZE 4096

static int quic_exchange(knot_xquic_conn_t *conn, knot_quic_reply_t *r, int timeout_ms)
{
	int fd = (int)(size_t)r->in_ctx;

	int ret = knot_quic_send(conn->xquic_table, conn, r, QUIC_MAX_SEND_PER_RECV, false);
	if (ret != KNOT_EOK) {
		return ret;
	}

	ret = net_dgram_recv(fd, r->in_payload->iov_base, QUIC_BUF_SIZE, timeout_ms);
	if (ret == 0) {
		return KNOT_ECONN;
	} else if (ret < 0) {
		return ret;
	}
	r->in_payload->iov_len = ret;

	knot_xquic_conn_t *hconn = NULL;
	r->in_ret = knot_quic_handle(conn->xquic_table, r, timeout_ms * 1000L, &hconn);
	if (hconn == NULL) {
		return KNOT_EOK;
	} else if (hconn != conn) {
		return KNOT_ESEMCHECK;
	} else {
		return KNOT_EOK;
	}
}

int qr_alloc_reply(struct knot_quic_reply *r)
{
	r->out_payload->iov_len = QUIC_BUF_SIZE;
	return KNOT_EOK;
}

int qr_send_reply(struct knot_quic_reply *r)
{
	int fd = (int)(size_t)r->in_ctx;
	int ret = net_dgram_send(fd, r->out_payload->iov_base, r->out_payload->iov_len, r->ip_rem);
	if (ret < 0) {
		return ret;
	} else if (ret == r->out_payload->iov_len) {
		return KNOT_EOK;
	} else {
		return KNOT_EAGAIN;
	}
}

void qr_free_reply(struct knot_quic_reply *r)
{
	(void)r;
}

struct knot_quic_reply *knot_qreq_connect(int fd, struct sockaddr_storage *rem_addr,
                                          const char *quic_cert, int timeout_ms)
{
	knot_quic_reply_t *r = calloc(1, sizeof(*r) + 2 * sizeof(struct iovec) + 2 * QUIC_BUF_SIZE + sizeof(*r->ip_loc));
	if (r == NULL) {
		return NULL;
	}

	r->in_payload = ((void *)r) + sizeof(*r);
	r->out_payload = r->in_payload + 1;
	r->in_payload->iov_base = ((void *)r->out_payload) + sizeof(*r->out_payload);
	r->out_payload->iov_base = r->in_payload->iov_base + QUIC_BUF_SIZE;
	r->in_ctx = (void *)(size_t)fd;
	r->ip_loc = r->out_payload->iov_base + QUIC_BUF_SIZE;
	r->ip_rem = rem_addr;
	r->alloc_reply = qr_alloc_reply;
	r->send_reply = qr_send_reply;
	r->free_reply = qr_free_reply;
	int ret = knot_eth_addr_from_fd(fd, NULL, (struct sockaddr_storage *)r->ip_loc);
	if (ret != KNOT_EOK) {
		free(r);
		return NULL;
	}

	struct knot_quic_creds *creds = knot_xquic_init_creds(false, quic_cert, NULL);
	if (creds == NULL) {
		free(r);
		return NULL;
	}

	knot_xquic_table_t *table = knot_xquic_table_new(1, QUIC_BUF_SIZE, QUIC_BUF_SIZE, 0, creds); // NOTE the limits on conns and buffers do not do anything since we do not sweep
	if (table == NULL) {
		knot_xquic_free_creds(creds);
		free(r);
		return NULL;
	}

	knot_xquic_conn_t *conn = NULL;
	ret = knot_xquic_client(table, (struct sockaddr_in6 *)r->ip_rem, (struct sockaddr_in6 *)r->ip_loc, &conn);
	r->ctx = conn;
	if (ret != KNOT_EOK) {
		knot_qreq_close(r);
		return NULL;
	}

	struct timespec t_start = time_now(), t_cur;
	while (!conn->handshake_done) {
		t_cur = time_now();
		if (time_diff_ms(&t_start, &t_cur) > timeout_ms ||
		    quic_exchange(conn, r, timeout_ms) != KNOT_EOK) {
			knot_qreq_close(r);
			return NULL;
		}
	}

	r->ctx = conn;
	return r;
}

int knot_qreq_send(struct knot_quic_reply *r, const struct iovec *data)
{
	knot_xquic_conn_t *conn = r->ctx;
	return knot_xquic_stream_add_data(conn, conn->streams_count * 4, data->iov_base, data->iov_len) == NULL ? KNOT_NET_ESEND : KNOT_EOK;
}

int knot_qreq_recv(struct knot_quic_reply *r, struct iovec *out, int timeout_ms)
{
	knot_xquic_conn_t *conn = r->ctx;
	knot_xquic_stream_t *stream = &conn->streams[conn->streams_count - 1];

	assert(conn->streams_count != 0);

	assert(stream->inbuf_fin_count <= 2);
	if (stream->inbuf_fin_count == 2) { // first inbuf has been processed last time
		if (stream->inbuf_fin[1].iov_len > out->iov_len) {
			return KNOT_ESPACE;
		}
		out->iov_len = stream->inbuf_fin[1].iov_len;
		memcpy(out->iov_base, stream->inbuf_fin[1].iov_base, out->iov_len);
		free(stream->inbuf_fin);
		stream->inbuf_fin = NULL;
		return KNOT_EOK;
	}

	struct timespec t_start = time_now(), t_cur;
	while (stream->inbuf_fin == NULL) {
		int ret = quic_exchange(conn, r, timeout_ms);
		if (ret != KNOT_EOK) {
			return ret;
		}
		t_cur = time_now();
		if (time_diff_ms(&t_start, &t_cur) > timeout_ms) {
			return KNOT_NET_ETIMEOUT;
		}
		if (conn->streams_count == 0) {
			return KNOT_ECONN;
		}
	}

	assert(stream->inbuf_fin_count > 0);
	if (stream->inbuf_fin_count > 2) {
		return KNOT_ESEMCHECK; // this would be difficult to handle and in practice it hardly happens
	}

	if (stream->inbuf_fin->iov_len <= out->iov_len) {
		out->iov_len = stream->inbuf_fin->iov_len;
		memcpy(out->iov_base, stream->inbuf_fin->iov_base, out->iov_len);
		if (stream->inbuf_fin_count < 2) {
			free(stream->inbuf_fin);
			stream->inbuf_fin = NULL;
		}
	} else {
		return KNOT_ESPACE;
	}

	return KNOT_EOK;
}

void knot_qreq_close(struct knot_quic_reply *r)
{
	knot_xquic_conn_t *conn = r->ctx;
	knot_xquic_table_t *table = conn->xquic_table;
	knot_xquic_table_rem(conn, table);
	knot_xquic_cleanup(&conn, 1);
	knot_xquic_free_creds(table->creds);
	knot_xquic_table_free(table);
	free(r);
}
