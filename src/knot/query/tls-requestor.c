/*  Copyright (C) 2024 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#include "knot/query/tls-requestor.h"

#include "libknot/error.h"
#include "libknot/quic/tls_common.h"
#include "libknot/quic/tls.h"

#include <string.h>

int knot_tls_req_ctx_init(knot_tls_req_ctx_t *ctx, int fd,
                          const struct knot_quic_creds *local_creds,
                          const uint8_t *peer_pin, uint8_t peer_pin_len,
                          int io_timeout_ms)
{
	struct knot_quic_creds *creds = knot_quic_init_creds_peer(local_creds,
	                                                          peer_pin, peer_pin_len);
	if (creds == NULL) {
		return KNOT_ENOMEM;
	}

	ctx->ctx = knot_tls_ctx_new(creds, false, io_timeout_ms);
	if (ctx->ctx == NULL) {
		knot_quic_free_creds(creds);
		return KNOT_ENOMEM;
	}

	ctx->conn = knot_tls_conn_new(ctx->ctx, fd);
	if (ctx->conn == NULL) {
		knot_tls_req_ctx_deinit(ctx);
		return KNOT_ERROR;
	}

	return KNOT_EOK;
}

void knot_tls_req_ctx_deinit(knot_tls_req_ctx_t *ctx)
{
	if (ctx != NULL && ctx->ctx != NULL) {
		knot_quic_free_creds(ctx->ctx->creds);
		knot_tls_conn_del(ctx->conn);
		knot_tls_ctx_free(ctx->ctx);
		memset(ctx, 0, sizeof(*ctx));
	}
}
