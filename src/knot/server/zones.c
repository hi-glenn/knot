/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>
#include <unistd.h>

#include "common/lists.h"
#include "common/prng.h"
#include "libknot/dname.h"
#include "libknot/util/wire.h"
#include "knot/zone/zone-dump-text.h"
#include "knot/zone/zone-load.h"
#include "libknot/zone/zone.h"
#include "libknot/zone/zonedb.h"
#include "knot/conf/conf.h"
#include "knot/other/debug.h"
#include "knot/other/error.h"
#include "common/log.h"
#include "knot/server/notify.h"
#include "knot/server/server.h"
#include "libknot/updates/xfr-in.h"
#include "knot/server/zones.h"
#include "libknot/util/error.h"
#include "knot/zone/zone-dump.h"
#include "libknot/nameserver/name-server.h"
#include "libknot/updates/changesets.h"
#include "libknot/tsig-op.h"
#include "libknot/packet/response.h"
#include "libknot/zone/zone-diff.h"

static const size_t XFRIN_CHANGESET_BINARY_SIZE = 100;
static const size_t XFRIN_CHANGESET_BINARY_STEP = 100;
static const size_t XFRIN_BOOTSTRAP_DELAY = 60; /*!< AXFR bootstrap avg. delay */

/* Forward declarations. */
static int zones_dump_zone_text(knot_zone_contents_t *zone,  const char *zf);
static int zones_dump_zone_binary(knot_zone_contents_t *zone, 
                                   const char *zonedb,
                                   const char *zonefile);
/*----------------------------------------------------------------------------*/

/*!
 * \brief Wrapper for TCP send.
 */
#include "knot/server/tcp-handler.h"
static int zones_send_cb(int fd, sockaddr_t *addr, uint8_t *msg, size_t msglen)
{
	return tcp_send(fd, msg, msglen);
}

/*----------------------------------------------------------------------------*/

/*! \brief Zone data destructor function. */
static int zonedata_destroy(knot_zone_t *zone)
{
	if (zone == NULL) {
		return KNOTD_EINVAL;
	}
	
	dbg_zones_verb("zones: zonedata_destroy(%p) called\n", zone);
	
	zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
	if (!zd) {
		return KNOTD_EINVAL;
	}

	/* Cancel REFRESH timer. */
	if (zd->xfr_in.timer) {
		evsched_t *sch = zd->xfr_in.timer->parent;
		evsched_cancel(sch, zd->xfr_in.timer);
		evsched_event_free(sch, zd->xfr_in.timer);
		zd->xfr_in.timer = 0;
	}

	/* Cancel EXPIRE timer. */
	if (zd->xfr_in.expire) {
		evsched_t *sch = zd->xfr_in.expire->parent;
		evsched_cancel(sch, zd->xfr_in.expire);
		evsched_event_free(sch, zd->xfr_in.expire);
		zd->xfr_in.expire = 0;
	}

	/* Remove list of pending NOTIFYs. */
	pthread_mutex_lock(&zd->lock);
	notify_ev_t *ev = 0, *evn = 0;
	WALK_LIST_DELSAFE(ev, evn, zd->notify_pending) {
		zones_cancel_notify(zd, ev);
	}
	pthread_mutex_unlock(&zd->lock);

	/* Cancel IXFR DB sync timer. */
	if (zd->ixfr_dbsync) {
		evsched_t *sch = zd->ixfr_dbsync->parent;
		evsched_cancel(sch, zd->ixfr_dbsync);
		evsched_event_free(sch, zd->ixfr_dbsync);
		zd->ixfr_dbsync = 0;
	}

	/* Destroy mutex. */
	pthread_mutex_destroy(&zd->lock);
	pthread_mutex_destroy(&zd->xfr_in.lock);

	acl_delete(&zd->xfr_in.acl);
	acl_delete(&zd->xfr_out);
	acl_delete(&zd->notify_in);
	acl_delete(&zd->notify_out);

	/* Close IXFR db. */
	journal_release(zd->ixfr_db);
	
	/* Free assigned config. */
	conf_free_zone(zd->conf);

	free(zd);
	
	/* Invalidate. */
	zone->data = 0;

	return KNOTD_EOK;
}

/*! \brief Zone data constructor function. */
static int zonedata_init(conf_zone_t *cfg, knot_zone_t *zone)
{
	if (cfg == NULL || zone == NULL) {
		return KNOTD_EINVAL;
	}
	zonedata_t *zd = malloc(sizeof(zonedata_t));
	if (!zd) {
		return KNOTD_ENOMEM;
	}
	memset(zd, 0, sizeof(zonedata_t));

	/* Link to config. */
	zd->conf = cfg;
	zd->server = 0;

	/* Initialize mutex. */
	pthread_mutex_init(&zd->lock, 0);

	/* Initialize ACLs. */
	zd->xfr_out = 0;
	zd->notify_in = 0;
	zd->notify_out = 0;

	/* Initialize XFR-IN. */
	sockaddr_init(&zd->xfr_in.master, -1);
	zd->xfr_in.timer = 0;
	zd->xfr_in.expire = 0;
	zd->xfr_in.next_id = -1;
	zd->xfr_in.acl = 0;
	zd->xfr_in.wrkr = 0;
	zd->xfr_in.bootstrap_retry = XFRIN_BOOTSTRAP_DELAY * 1000 * tls_rand();
	pthread_mutex_init(&zd->xfr_in.lock, 0);

	/* Initialize NOTIFY. */
	init_list(&zd->notify_pending);

	/* Initialize IXFR database. */
	zd->ixfr_db = journal_open(cfg->ixfr_db, cfg->ixfr_fslimit,
	                           JOURNAL_LAZY, JOURNAL_DIRTY);
	
	if (zd->ixfr_db == NULL) {
		char ebuf[256] = {0};
		strerror_r(errno, ebuf, sizeof(ebuf));
		log_server_warning("Couldn't open journal file for zone '%s', "
		                   "disabling incoming IXFR. (%s)\n", cfg->name, ebuf);
	}

	/* Initialize IXFR database syncing event. */
	zd->ixfr_dbsync = 0;

	/* Set and install destructor. */
	zone->data = zd;
	knot_zone_set_dtor(zone, zonedata_destroy);

	/* Set zonefile SOA serial. */
	const knot_rrset_t *soa_rrs = 0;
	const knot_rdata_t *soa_rr = 0;

	/* Load serial. */
	zd->zonefile_serial = 0;
	const knot_zone_contents_t *contents = knot_zone_contents(zone);
	if (contents) {
		soa_rrs = knot_node_rrset(knot_zone_contents_apex(contents),
					  KNOT_RRTYPE_SOA);
		assert(soa_rrs != NULL);
		soa_rr = knot_rrset_rdata(soa_rrs);
		int64_t serial = knot_rdata_soa_serial(soa_rr);
		zd->zonefile_serial = (uint32_t)serial;
		if (serial < 0) {
			return KNOTD_EINVAL;
		}
	}

	return KNOTD_EOK;
}

/*!
 * \brief Apply jitter to time interval.
 *
 * Amount of jitter is specified by ZONES_JITTER_PCT.
 *
 * \param interval base value.
 * \return interval value minus rand(0, ZONES_JITTER_PCT) %
 */
static uint32_t zones_jitter(uint32_t interval)
{
	return (interval * (100 - (tls_rand() * ZONES_JITTER_PCT))) / 100; 
}

/*!
 * \brief Return SOA timer value.
 *
 * \param zone Pointer to zone.
 * \param rr_func RDATA specificator.
 * \return Timer in miliseconds.
 */
static uint32_t zones_soa_timer(knot_zone_t *zone,
                                uint32_t (*rr_func)(const knot_rdata_t*))
{
	if (!zone) {
		dbg_zones_verb("zones: zones_soa_timer() called "
		               "with NULL zone\n");
	}

	uint32_t ret = 0;

	/* Retrieve SOA RDATA. */
	const knot_rrset_t *soa_rrs = 0;
	const knot_rdata_t *soa_rr = 0;

	rcu_read_lock();

	knot_zone_contents_t * zc = knot_zone_get_contents((zone));
	if (!zc) {
		rcu_read_unlock();
		return 0;
	}

	soa_rrs = knot_node_rrset(knot_zone_contents_apex(zc),
	                            KNOT_RRTYPE_SOA);
	assert(soa_rrs != NULL);
	soa_rr = knot_rrset_rdata(soa_rrs);
	ret = rr_func(soa_rr);

	rcu_read_unlock();

	/* Convert to miliseconds. */
	return ret * 1000;
}

/*!
 * \brief Return SOA REFRESH timer value.
 *
 * \param zone Pointer to zone.
 * \return REFRESH timer in miliseconds.
 */
static uint32_t zones_soa_refresh(knot_zone_t *zone)
{
	return zones_soa_timer(zone, knot_rdata_soa_refresh);
}

/*!
 * \brief Return SOA RETRY timer value.
 *
 * \param zone Pointer to zone.
 * \return RETRY timer in miliseconds.
 */
static uint32_t zones_soa_retry(knot_zone_t *zone)
{
	return zones_soa_timer(zone, knot_rdata_soa_retry);
}

/*!
 * \brief Return SOA EXPIRE timer value.
 *
 * \param zone Pointer to zone.
 * \return EXPIRE timer in miliseconds.
 */
static uint32_t zones_soa_expire(knot_zone_t *zone)
{
	return zones_soa_timer(zone, knot_rdata_soa_expire);
}

/*!
 * \brief XFR/IN expire event handler.
 */
static int zones_expire_ev(event_t *e)
{
	dbg_zones("zones: EXPIRE timer event\n");
	knot_zone_t *zone = (knot_zone_t *)e->data;
	if (zone == NULL || zone->data == NULL) {
		return KNOTD_EINVAL;
	}
	
	zonedata_t *zd = (zonedata_t *)zone->data;
	rcu_read_lock();
	
	/* Check if zone is not discarded. */
	if (knot_zone_flags(zone) & KNOT_ZONE_DISCARDED) {
		rcu_read_unlock();
		return KNOTD_EOK;
	}
	
	/* Do not issue SOA query if transfer is pending. */
	int locked = pthread_mutex_trylock(&zd->xfr_in.lock);
	if (locked != 0) {
		dbg_zones("zones: zone '%s' is being transferred, "
		          "deferring EXPIRE\n",
		          zd->conf->name);
		
		/* Reschedule as EXPIRE timer. */
		uint32_t exp_tmr = zones_soa_expire(zone);
		evsched_schedule(e->parent, e, exp_tmr);
		dbg_zones("zones: EXPIRE of '%s' after %u seconds\n",
		          zd->conf->name, exp_tmr / 1000);
		
		/* Unlock RCU. */
		rcu_read_unlock();
		return KNOTD_EOK;
	}
	dbg_zones_verb("zones: zone %s locked, no xfers are running\n",
	               zd->conf->name);
	
	/* Won't accept any pending SOA responses. */
	zd->xfr_in.next_id = -1;

	/* Mark the zone as expired. This will remove the zone contents. */
	knot_zone_contents_t *contents = knot_zonedb_expire_zone(
			zd->server->nameserver->zone_db, zone->name);

	if (contents == NULL) {
		pthread_mutex_unlock(&zd->xfr_in.lock);
		log_server_warning("Non-existent zone expired. Ignoring.\n");
		rcu_read_unlock();
		return KNOTD_EOK;
	}
	
	/* Publish expired zone. */
	/* Need to keep a reference in case zone get's deleted in meantime. */
	knot_zone_retain(zone);
	rcu_read_unlock();
	synchronize_rcu();
	rcu_read_lock();
	
	/* Log event. */
	log_server_info("Zone '%s' expired.\n", zd->conf->name);
	
	/* Early finish this event to prevent lockup during cancellation. */
	dbg_zones("zones: zone expired, removing from database\n");
	evsched_event_finished(e->parent);
	
	/* Cancel REFRESH timer. */
	if (zd->xfr_in.timer) {
		evsched_cancel(e->parent, zd->xfr_in.timer);
		evsched_event_free(e->parent, zd->xfr_in.timer);
		zd->xfr_in.timer = 0;
	}

	/* Free EXPIRE timer. */
	if (zd->xfr_in.expire) {
		evsched_event_free(e->parent, zd->xfr_in.expire);
		zd->xfr_in.expire = 0;
	}
	
	knot_zone_contents_deep_free(&contents, 0);
	pthread_mutex_unlock(&zd->xfr_in.lock);
	rcu_read_unlock();
	
	/* Release holding reference. */
	knot_zone_release(zone);
	
	return KNOTD_EOK;
}

/*!
 * \brief Zone REFRESH or RETRY event.
 */
static int zones_refresh_ev(event_t *e)
{
	dbg_zones("zones: REFRESH or RETRY timer event\n");
	rcu_read_lock();
	knot_zone_t *zone = (knot_zone_t *)e->data;
	if (zone == NULL || zone->data == NULL) {
		rcu_read_unlock();
		return KNOTD_EINVAL;
	}

	/* Cancel pending timers. */
	zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
	
	/* Check if zone is not discarded. */
	if (knot_zone_flags(zone) & KNOT_ZONE_DISCARDED) {
		rcu_read_unlock();
		return KNOTD_EOK;
	}

	/* Check for contents. */
	if (!knot_zone_contents(zone)) {

		/* Bootstrap from XFR master. */
		knot_ns_xfr_t xfr_req;
		memset(&xfr_req, 0, sizeof(knot_ns_xfr_t));
		memcpy(&xfr_req.addr, &zd->xfr_in.master, sizeof(sockaddr_t));
		memcpy(&xfr_req.saddr, &zd->xfr_in.via, sizeof(sockaddr_t));
		xfr_req.data = (void *)zone;
		xfr_req.send = zones_send_cb;

		/* Select transfer method. */
		xfr_req.type = XFR_TYPE_AIN;
		xfr_req.zone = zone;
		
		/* Select TSIG key. */
		if (zd->xfr_in.tsig_key.name) {
			xfr_req.tsig_key = &zd->xfr_in.tsig_key;
		}

		/* Enqueue XFR request. */
		int locked = pthread_mutex_trylock(&zd->xfr_in.lock);
		if (locked != 0) {
			dbg_zones("zones: already bootstrapping '%s'\n",
			          zd->conf->name);
			rcu_read_unlock();
			return KNOTD_EOK;
		}

		if (zd->xfr_in.scheduled > 0) {
			/* Already pending bootstrap (unprocessed). */
			pthread_mutex_unlock(&zd->xfr_in.lock);
			dbg_zones("zones: already bootstrapping '%s' (q'd)\n",
			          zd->conf->name);
			rcu_read_unlock();
			return KNOTD_EOK;
		}
		
//		log_zone_info("Attempting to bootstrap zone %s from master\n",
//			      zd->conf->name);
		++zd->xfr_in.scheduled;
		pthread_mutex_unlock(&zd->xfr_in.lock);
		
		/* Retain pointer to zone for processing. */
		knot_zone_retain(xfr_req.zone);
		
		/* Unlock zone contents. */
		rcu_read_unlock();
		
		/* Mark as finished to prevent stalling. */
		evsched_event_finished(e->parent);
		int ret = xfr_request(zd->server->xfr_h, &xfr_req);
		if (ret != KNOTD_EOK) {
			knot_zone_release(xfr_req.zone); /* Discard */
		}
		return ret;
		
	}
	
	/* Do not issue SOA query if transfer is pending. */
	int locked = pthread_mutex_trylock(&zd->xfr_in.lock);
	if (locked != 0) {
		dbg_zones("zones: zone '%s' is being transferred, "
		          "deferring SOA query\n",
		          zd->conf->name);
		
		/* Reschedule as RETRY timer. */
		uint32_t retry_tmr = zones_jitter(zones_soa_retry(zone));
		evsched_schedule(e->parent, e, retry_tmr);
		dbg_zones("zones: RETRY of '%s' after %u seconds\n",
		          zd->conf->name, retry_tmr / 1000);
		
		/* Unlock RCU. */
		rcu_read_unlock();
		return KNOTD_EOK;
	} else {
		pthread_mutex_unlock(&zd->xfr_in.lock);
	}
	
	/* Schedule EXPIRE timer on first attempt. */
	if (!zd->xfr_in.expire) {
		uint32_t expire_tmr = zones_jitter(zones_soa_expire(zone));
		zd->xfr_in.expire = evsched_schedule_cb(
					      e->parent,
					      zones_expire_ev,
					      zone, expire_tmr);
		dbg_zones("zones: EXPIRE of '%s' after %u seconds\n",
		          zd->conf->name, expire_tmr / 1000);
	}
	
	/* Reschedule as RETRY timer. */
	uint32_t retry_tmr = zones_jitter(zones_soa_retry(zone));
	evsched_schedule(e->parent, e, retry_tmr);
	dbg_zones("zones: RETRY of '%s' after %u seconds\n",
	          zd->conf->name, retry_tmr / 1000);
	
	/* Prepare buffer for query. */
	uint8_t *qbuf = malloc(SOCKET_MTU_SZ);
	if (qbuf == NULL) {
		log_zone_error("Not enough memory to allocate SOA query.\n");
		rcu_read_unlock();
		return KNOTD_ENOMEM;
	}
	
	size_t buflen = SOCKET_MTU_SZ;
	
	knot_ns_xfr_t req;
	memset(&req, 0, sizeof(knot_ns_xfr_t));
	req.wire = qbuf;
	
	/* Select TSIG key. */
	if (zd->xfr_in.tsig_key.name) {
		xfr_prepare_tsig(&req, &zd->xfr_in.tsig_key);
	}

	/* Create query. */
	int sock = -1;
	char strbuf[256] = "Generic error.";
	const char *errstr = strbuf;
	sockaddr_t *master = &zd->xfr_in.master;
	int ret = xfrin_create_soa_query(zone->name, &req, &buflen);
	if (ret == KNOT_EOK) {

		/* Create socket on random port. */
		sock = socket_create(master->family, SOCK_DGRAM);
		
		/* Check requested source. */
		sockaddr_t *via = &zd->xfr_in.via;
		if (via->len > 0) {
			if (bind(sock, via->ptr, via->len) < 0) {
				socket_close(sock);
				sock = -1;
				char r_addr[SOCKADDR_STRLEN];
				sockaddr_tostr(via, r_addr, sizeof(r_addr));
				snprintf(strbuf, sizeof(strbuf),
				         "Couldn't bind to \'%s\'", r_addr);
			}
		}

		/* Send query. */
		ret = KNOTD_ERROR;
		if (sock > -1) {
			int sent = sendto(sock, qbuf, buflen, 0,
			                  master->ptr, master->len);
		
			/* Store ID of the awaited response. */
			if (sent == buflen) {
				ret = KNOTD_EOK;
			} else {
				strbuf[0] = '\0';
				strerror_r(errno, strbuf, sizeof(strbuf));
				socket_close(sock);
				sock = -1;
			}
		}
		
		/* Check result. */
		if (ret == KNOTD_EOK) {
			zd->xfr_in.next_id = knot_wire_get_id(qbuf);
			dbg_zones("zones: expecting SOA response "
			          "ID=%d for '%s'\n",
			          zd->xfr_in.next_id, zd->conf->name);
		}
	} else {
		ret = KNOTD_ERROR;
		errstr = "Couldn't create SOA query";
	}

	
	/* Mark as finished to prevent stalling. */
	evsched_event_finished(e->parent);
	
	/* Watch socket. */
	req.session = sock;
	req.type = XFR_TYPE_SOA;
	req.flags |= XFR_FLAG_UDP;
	req.zone = zone;
	req.wire = NULL;
	memcpy(&req.addr, master, sizeof(sockaddr_t));
	memcpy(&req.saddr, &zd->xfr_in.via, sizeof(sockaddr_t));
	sockaddr_update(&req.addr);
	sockaddr_update(&req.saddr);
	
	/* Retain pointer to zone and issue. */
	knot_zone_retain(req.zone);
	if (ret == KNOTD_EOK) {
		ret = xfr_request(zd->server->xfr_h, &req);
	}
	if (ret != KNOTD_EOK) {
		free(req.digest);
		knot_zone_release(req.zone); /* Discard */
		log_server_warning("Failed to issue SOA query for zone '%s' (%s).\n",
		                   zd->conf->name, errstr);
	}
	
	free(qbuf);
	
	/* Unlock RCU. */
	rcu_read_unlock();

	return ret;
}

/*!
 * \brief Send NOTIFY to slave server.
 */
static int zones_notify_send(event_t *e)
{
	dbg_notify("notify: NOTIFY timer event\n");
	rcu_read_lock();
	notify_ev_t *ev = (notify_ev_t *)e->data;
	if (ev == NULL) {
		rcu_read_unlock();
		log_zone_error("NOTIFY invalid event received\n");
		return KNOTD_EINVAL;
	}
	
	knot_zone_t *zone = ev->zone;
	if (zone == NULL || zone->data == NULL) {
		rcu_read_unlock();
		log_zone_error("NOTIFY invalid event data received\n");
		evsched_event_free(e->parent, e);
		free(ev);
		return KNOTD_EINVAL;
	}
	
	/* Check if zone is not discarded. */
	if (knot_zone_flags(zone) & KNOT_ZONE_DISCARDED) {
		rcu_read_unlock(); /* Event will be freed on zonedata_destroy.*/
		return KNOTD_EOK;
	}

	/* Check for answered/cancelled query. */
	zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
	knot_zone_contents_t *contents = knot_zone_get_contents(zone);

	/* Reduce number of available retries. */
	--ev->retries;

	/* Check number of retries. */
	if (ev->retries < 0) {
		log_server_notice("NOTIFY query maximum number of retries "
				  "for zone '%s' exceeded.\n",
				  zd->conf->name);
		rcu_read_unlock();
		pthread_mutex_lock(&zd->lock);
		rem_node(&ev->n);
		evsched_event_free(e->parent, e);
		free(ev);
		pthread_mutex_unlock(&zd->lock);
		return KNOTD_EMALF;
	}

	/* RFC suggests 60s, but it is configurable. */
	int retry_tmr = ev->timeout * 1000;
 
	/* Reschedule. */
	evsched_schedule(e->parent, e, retry_tmr);
	dbg_notify("notify: Query RETRY after %u secs (zone '%s')\n",
	           retry_tmr / 1000, zd->conf->name);
	
	/* Prepare buffer for query. */
	uint8_t *qbuf = malloc(SOCKET_MTU_SZ);
	if (qbuf == NULL) {
		log_zone_error("Not enough memory to allocate NOTIFY query.\n");
		rcu_read_unlock();
		return KNOTD_ENOMEM;
	}
	
	size_t buflen = SOCKET_MTU_SZ;

	/* Create query. */
	int ret = notify_create_request(contents, qbuf, &buflen);
	if (ret == KNOTD_EOK && zd->server) {

		/* Create socket on random port. */
		int sock = socket_create(ev->addr.family, SOCK_DGRAM);
		
		/* Check requested source. */
		if (ev->saddr.len > 0) {
			if (bind(sock, ev->saddr.ptr, ev->saddr.len) < 0) {
				socket_close(sock);
				sock = -1;
			}
		}

		/* Send query. */
		ret = -1;
		if (sock > -1) {
			ret = sendto(sock, qbuf, buflen, 0,
				     ev->addr.ptr, ev->addr.len);
		}

		/* Store ID of the awaited response. */
		if (ret == buflen) {
			ev->msgid = knot_wire_get_id(qbuf);
			
		}
		
		/* Mark as finished to prevent stalling. */
		evsched_event_finished(e->parent);

		/* Watch socket. */
		knot_ns_xfr_t req;
		memset(&req, 0, sizeof(req));
		req.session = sock;
		req.type = XFR_TYPE_NOTIFY;
		req.flags |= XFR_FLAG_UDP;
		req.zone = zone;
		memcpy(&req.addr, &ev->addr, sizeof(sockaddr_t));
		memcpy(&req.saddr, &ev->saddr, sizeof(sockaddr_t));
		
		/* Retain pointer to zone and issue request. */
		knot_zone_retain(req.zone);
		ret = xfr_request(zd->server->xfr_h, &req);
		if (ret != KNOTD_EOK) {
			knot_zone_release(req.zone); /* Discard */
		}
	}
	
	free(qbuf);

	rcu_read_unlock();

	return ret;
}

/*! \brief Function for marking nodes as synced and updated. */
static int zones_ixfrdb_sync_apply(journal_t *j, journal_node_t *n)
{
	/* Check for dirty bit (not synced to permanent storage). */
	if (n->flags & JOURNAL_DIRTY) {

		/* Remove dirty bit. */
		n->flags = n->flags & ~JOURNAL_DIRTY;

		/* Sync. */
		journal_update(j, n);
	}

	return KNOTD_EOK;
}

/*!
 * \brief Sync chagnes in zone to zonefile.
 */
static int zones_zonefile_sync_ev(event_t *e)
{
	dbg_zones("zones: IXFR database SYNC timer event\n");

	/* Fetch zone. */
	knot_zone_t *zone = (knot_zone_t *)e->data;
	if (!zone) {
		return KNOTD_EINVAL;
	}

	/* Fetch zone data. */
	zonedata_t *zd = (zonedata_t *)zone->data;
	if (!zd) {
		return KNOTD_EINVAL;
	}

	/* Execute zonefile sync. */
	journal_t *j = journal_retain(zd->ixfr_db);
	int ret = zones_zonefile_sync(zone, j);
	journal_release(j);

	rcu_read_lock();
	if (ret == KNOTD_EOK) {
		log_zone_info("Applied differences of '%s' to zonefile.\n",
		              zd->conf->name);
	} else if (ret != KNOTD_ERANGE) {
		log_zone_warning("Failed to apply differences of '%s' "
		                 "to zonefile.\n",
		                 zd->conf->name);
	}
	rcu_read_unlock();

	/* Reschedule. */
	rcu_read_lock();
	evsched_schedule(e->parent, e, zd->conf->dbsync_timeout * 1000);
	dbg_zones("zones: next IXFR database SYNC of '%s' in %d seconds\n",
	          zd->conf->name, zd->conf->dbsync_timeout);
	rcu_read_unlock();

	return ret;
}

/*!
 * \brief Update ACL list from configuration.
 *
 * \param acl Pointer to existing or NULL ACL.
 * \param acl_list List of remotes from configuration.
 *
 * \retval KNOTD_EOK on success.
 * \retval KNOTD_EINVAL on invalid parameters.
 * \retval KNOTD_ENOMEM on failed memory allocation.
 */
static int zones_set_acl(acl_t **acl, list* acl_list)
{
	if (!acl || !acl_list) {
		return KNOTD_EINVAL;
	}

	/* Truncate old ACL. */
	acl_delete(acl);

	/* Create new ACL. */
	*acl = acl_new(ACL_DENY, 0);
	if (*acl == NULL) {
		return KNOTD_ENOMEM;
	}

	/* Load ACL rules. */
	sockaddr_t addr;
	conf_remote_t *r = 0;
	WALK_LIST(r, *acl_list) {

		/* Initialize address. */
		/*! Port matching disabled, port = 0. */
		sockaddr_init(&addr, -1);
		conf_iface_t *cfg_if = r->remote;
		int ret = sockaddr_set(&addr, cfg_if->family,
		                       cfg_if->address, 0);
		sockaddr_setprefix(&addr, cfg_if->prefix);

		/* Load rule. */
		if (ret > 0) {
			/*! \todo Correct search for the longest prefix match.
			 *        This just favorizes remotes with TSIG.
			 *        (issue #1675)
			 */
			unsigned flags = 0;
			if (cfg_if->key != NULL) {
				flags = ACL_PREFER;
			}
			acl_create(*acl, &addr, ACL_ACCEPT, cfg_if, flags);
		}
	}

	return KNOTD_EOK;
}

/*!
 * \brief Load zone to zone database.
 *
 * \param dst Loaded zone will be returned in this parameter.
 * \param zone_name Zone name (owner of the apex node).
 * \param source Path to zone file source.
 * \param filename Path to requested compiled zone file.
 *
 * \retval KNOTD_EOK
 * \retval KNOTD_EINVAL
 * \retval KNOTD_EZONEINVAL
 */
static int zones_load_zone(knot_zone_t **dst, const char *zone_name,
			   const char *source, const char *filename)
{
	if (dst == NULL || zone_name == NULL || source == NULL) {
		return KNOTD_EINVAL;
	}
	*dst = NULL;
	
	/* Duplicate zone name. */
	size_t zlen = strlen(zone_name);
	char *zname = NULL;
	if (zlen > 0) {
		if ((zname = strdup(zone_name)) == NULL) {
			return KNOTD_ENOMEM;
		}
	} else {
		return KNOTD_EINVAL;
	}
	zname[zlen - 1] = '\0'; /* Trim last dot */
	if (filename == NULL) {
		log_server_error("No file name for zone '%s'.\n", zname);
		free(zname);
		return KNOTD_EINVAL;
	}
	
	
	/* Check if the compiled file still exists. */
	struct stat st;
	if (stat(source, &st) != 0) {
		char reason[256] = {0};
		strerror_r(errno, reason, sizeof(reason));
		log_server_warning("Failed to open zone file '%s' (%s).\n",
		                   zname, reason);
		free(zname);
		return KNOTD_EZONEINVAL;
	}

	/* Attempt to open compiled zone for loading. */
	int ret = KNOTD_EOK;
	zloader_t *zl = NULL;
	dbg_zones("zones: parsing zone database '%s'\n", filename);
	switch(knot_zload_open(&zl, filename)) {
	case KNOT_EOK:
		/* OK */
		break;
	case KNOT_EACCES:
		log_server_error("Failed to open compiled zone '%s' "
				 "(Permission denied).\n", filename);
		free(zname);
		return KNOTD_EZONEINVAL;
	case KNOT_ENOENT:
		log_server_error("Couldn't find compiled zone. "
				 "Please recompile '%s'.\n", zname);
		free(zname);
		return KNOTD_EZONEINVAL;
	case KNOT_ECRC:
		log_server_error("Compiled zone db CRC mismatch, "
				 "db is corrupted or .crc file is "
				 "deleted. Please recompile '%s'.\n",
				 zname);
		free(zname);
		return KNOTD_EZONEINVAL;
	case KNOT_EMALF:
		log_server_error("Compiled db '%s' is too old. "
				 "Please recompile '%s'.\n",
				 filename, zname);
		free(zname);
		return KNOTD_EZONEINVAL;
	case KNOT_EFEWDATA:
	case KNOT_ERROR:
	case KNOT_ENOMEM:
	default:
		log_server_error("Failed to load compiled zone file "
				 "'%s'.\n", filename);
		free(zname);
		return KNOTD_EZONEINVAL;
	}
	
	/* Check the source file */
	assert(zl != NULL);
	int src_changed = strcmp(source, zl->source) != 0;
	if (src_changed || knot_zload_needs_update(zl)) {
		log_server_warning("Database for zone '%s' is not "
				   "up-to-date. Please recompile.\n",
				   zname);
	}
	
	*dst = knot_zload_load(zl);
	if (*dst == NULL) {
		log_server_error("Failed to load db '%s' for zone '%s'.\n",
				 filename, zname);
		knot_zload_close(zl);
		free(zname);
		return KNOTD_EZONEINVAL;
	}
	
	/* Check if loaded origin matches. */
	const knot_dname_t *dname = knot_zone_name(*dst);
	knot_dname_t *dname_req = NULL;
	dname_req = knot_dname_new_from_str(zone_name, zlen, 0);
	if (knot_dname_compare(dname, dname_req) != 0) {
		log_server_error("Origin of the zone db file is "
				 "different than '%s'\n",
				 zone_name);
		knot_zone_deep_free(dst, 0);
		ret = KNOTD_EZONEINVAL;
	} else {
		/* Save the timestamp from the zone db file. */
		if (stat(filename, &st) < 0) {
			dbg_zones("zones: failed to stat() zone db, "
				  "something is seriously wrong\n");
			knot_zone_deep_free(dst, 0);
			ret = KNOTD_EZONEINVAL;
		} else {
			knot_zone_set_version(*dst, st.st_mtime);
		}
	}
	knot_dname_free(&dname_req);
	knot_zload_close(zl);
	free(zname);
	return ret;
}

/*----------------------------------------------------------------------------*/

/*! \brief Return 'serial_from' part of the key. */
static inline uint32_t ixfrdb_key_from(uint64_t k)
{
	/*      64    32       0
	 * key = [TO   |   FROM]
	 * Need: Least significant 32 bits.
	 */
	return (uint32_t)(k & ((uint64_t)0x00000000ffffffff));
}

/*----------------------------------------------------------------------------*/

/*! \brief Return 'serial_to' part of the key. */
static inline uint32_t ixfrdb_key_to(uint64_t k)
{
	/*      64    32       0
	 * key = [TO   |   FROM]
	 * Need: Most significant 32 bits.
	 */
	return (uint32_t)(k >> (uint64_t)32);
}

/*----------------------------------------------------------------------------*/

/*! \brief Compare function to match entries with target serial. */
static inline int ixfrdb_key_to_cmp(uint64_t k, uint64_t to)
{
	/*      64    32       0
	 * key = [TO   |   FROM]
	 * Need: Most significant 32 bits.
	 */
	return ((uint64_t)ixfrdb_key_to(k)) - to;
}

/*----------------------------------------------------------------------------*/

/*! \brief Compare function to match entries with starting serial. */
static inline int ixfrdb_key_from_cmp(uint64_t k, uint64_t from)
{
	/*      64    32       0
	 * key = [TO   |   FROM]
	 * Need: Least significant 32 bits.
	 */
	return ((uint64_t)ixfrdb_key_from(k)) - from;
}

/*----------------------------------------------------------------------------*/

/*! \brief Make key for journal from serials. */
static inline uint64_t ixfrdb_key_make(uint32_t from, uint32_t to)
{
	/*      64    32       0
	 * key = [TO   |   FROM]
	 */
	return (((uint64_t)to) << ((uint64_t)32)) | ((uint64_t)from);
}

/*----------------------------------------------------------------------------*/

int zones_changesets_from_binary(knot_changesets_t *chgsets)
{
	assert(chgsets != NULL);
	assert(chgsets->allocated >= chgsets->count);
	/*
	 * Parses changesets from the binary format stored in chgsets->data
	 * into the changeset_t structures.
	 */
	knot_rrset_t *rrset = 0;
	int ret = 0;

	for (int i = 0; i < chgsets->count; ++i) {

		/* Read initial changeset RRSet - SOA. */
		knot_changeset_t* chs = chgsets->sets + i;
		size_t remaining = chs->size;
		ret = knot_zload_rrset_deserialize(&rrset, chs->data, &remaining);
		if (ret != KNOT_EOK) {
			dbg_xfr("xfr: SOA: failed to deserialize data "
			        "from changeset, %s\n", knot_strerror(ret));
			return KNOTD_EMALF;
		}

		/* in this special case (changesets loaded
		 * from journal) the SOA serial should already
		 * be set, check it.
		 */
		dbg_xfr_verb("xfr: reading RRSets to REMOVE, first RR is %hu\n",
		             knot_rrset_type(rrset));
		assert(knot_rrset_type(rrset) == KNOT_RRTYPE_SOA);
		assert(chs->serial_from ==
		       knot_rdata_soa_serial(knot_rrset_rdata(rrset)));
		knot_changeset_store_soa(&chs->soa_from, &chs->serial_from,
					 rrset);

		/* Read remaining RRSets */
		int in_remove_section = 1;
		while (remaining > 0) {

			/* Parse next RRSet. */
			rrset = 0;
			uint8_t *stream = chs->data + (chs->size - remaining);
			ret = knot_zload_rrset_deserialize(&rrset, stream, &remaining);
			if (ret != KNOT_EOK) {
				dbg_xfr("xfr: failed to deserialize data "
				        "from changeset, %s\n",
				        knot_strerror(ret));
				return KNOTD_EMALF;
			}

			/* Check for next SOA. */
			if (knot_rrset_type(rrset) == KNOT_RRTYPE_SOA) {

				/* Move to ADD section if in REMOVE. */
				if (in_remove_section) {
					knot_changeset_store_soa(
						&chgsets->sets[i].soa_to,
						&chgsets->sets[i].serial_to,
						rrset);
					dbg_xfr_verb("xfr: reading RRSets"
					             " to ADD\n");
					in_remove_section = 0;
				} else {
					/* Final SOA. */
					dbg_xfr_verb("xfr: extra SOA\n");
					knot_rrset_free(&rrset);
					break;
				}
			} else {
				/* Remove RRSets. */
				if (in_remove_section) {
					ret = knot_changeset_add_rrset(
						&chgsets->sets[i].remove,
						&chgsets->sets[i].remove_count,
						&chgsets->sets[i]
						    .remove_allocated,
						rrset);
				} else {
				/* Add RRSets. */
					ret = knot_changeset_add_rrset(
						&chgsets->sets[i].add,
						&chgsets->sets[i].add_count,
						&chgsets->sets[i].add_allocated,
						rrset);
				}

				/* Check result. */
				if (ret != KNOT_EOK) {
					dbg_xfr("xfr: failed to add/remove "
					        "RRSet to changeset: %s\n",
					        knot_strerror(ret));
					return KNOTD_ERROR;
				}
			}
		}
		
		dbg_xfr_verb("xfr: read all RRSets in changeset\n");
	}

	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

static int zones_load_changesets(const knot_zone_t *zone, 
                                 knot_changesets_t *dst,
                                 uint32_t from, uint32_t to)
{
	if (!zone || !dst) {
		dbg_zones_detail("Bad arguments: zone=%p, dst=%p\n", zone, dst);
		return KNOTD_EINVAL;
	}
	if (!zone->data) {
		dbg_zones_detail("Bad arguments: zone->data=%p\n", zone->data);
		return KNOTD_EINVAL;
	}
	
	/* Fetch zone-specific data. */
	zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
	if (!zd->ixfr_db) {
		dbg_zones_detail("Bad arguments: zd->ixfr_db=%p\n", zone->data);
		return KNOTD_EINVAL;
	}

	rcu_read_lock();
	dbg_xfr("xfr: loading changesets for zone '%s' from serial %u to %u\n",
	        zd->conf->name, from, to);
	rcu_read_unlock();
	
	/* Retain journal for changeset loading. */
	journal_t *j = journal_retain(zd->ixfr_db);
	if (j == NULL) {
		return KNOTD_EBUSY;
	}

	/* Read entries from starting serial until finished. */
	uint32_t found_to = from;
	journal_node_t *n = 0;
	int ret = journal_fetch(j, from, ixfrdb_key_from_cmp, &n);
	if (ret != KNOTD_EOK) {
		dbg_xfr("xfr: failed to fetch starting changeset: %s\n",
		        knotd_strerror(ret));
		journal_release(j);
		return ret;
	}
	
	while (n != 0 && n != journal_end(j)) {

		/* Check for history end. */
		if (to == found_to) {
			break;
		}

		/*! \todo Increment and decrement to reserve +1,
		 *        but not incremented counter.*/
		/* Check changesets size if needed. */
		++dst->count;
		ret = knot_changesets_check_size(dst);
		--dst->count;
		if (ret != KNOT_EOK) {
			//--dst->count;
			dbg_xfr("xfr: failed to check changesets size: %s\n",
			        knot_strerror(ret));
			journal_release(j);
			return KNOTD_ERROR;
		}

		/* Initialize changeset. */
		dbg_xfr_detail("xfr: reading entry #%zu id=%llu\n",
		               dst->count, (unsigned long long)n->id);
		knot_changeset_t *chs = dst->sets + dst->count;
		chs->serial_from = ixfrdb_key_from(n->id);
		chs->serial_to = ixfrdb_key_to(n->id);
		chs->data = malloc(n->len);
		if (!chs->data) {
			journal_release(j);
			return KNOTD_ENOMEM;
		}

		/* Read journal entry. */
		ret = journal_read(j, n->id, 0, (char*)chs->data);
		if (ret != KNOTD_EOK) {
			dbg_xfr("xfr: failed to read data from journal\n");
			free(chs->data);
			journal_release(j);
			return KNOTD_ERROR;
		}

		/* Update changeset binary size. */
		chs->size = chs->allocated = n->len;

		/* Next node. */
		found_to = chs->serial_to;
		++dst->count;
		++n;

		/*! \todo Check consistency. */
	}
	
	dbg_xfr_detail("xfr: finished reading journal entries\n");
	journal_release(j);

	/* Unpack binary data. */
	int unpack_ret = zones_changesets_from_binary(dst);
	if (unpack_ret != KNOTD_EOK) {
		dbg_xfr("xfr: failed to unpack changesets "
		        "from binary, %s\n", knotd_strerror(unpack_ret));
		return unpack_ret;
	}

	/* Check for complete history. */
	if (to != found_to) {
		dbg_xfr_detail("xfr: load changesets finished, ERANGE\n");
		return KNOTD_ERANGE;
	}

	/* History reconstructed. */
	dbg_xfr_detail("xfr: load changesets finished, EOK\n");
	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

/*!
 * \brief Apply changesets to zone from journal.
 *
 * \param zone Specified zone.
 *
 * \retval KNOTD_EOK if successful.
 * \retval KNOTD_EINVAL on invalid parameters.
 * \retval KNOTD_ENOENT if zone has no contents.
 * \retval KNOTD_ERROR on unspecified error.
 */
static int zones_journal_apply(knot_zone_t *zone)
{
	/* Fetch zone. */
	if (!zone) {
		return KNOTD_EINVAL;
	}

	rcu_read_lock();

	knot_zone_contents_t *contents = knot_zone_get_contents(zone);
	zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
	if (!contents || !zd) {
		rcu_read_unlock();
		return KNOTD_ENOENT;
	}

	/* Fetch SOA serial. */
	const knot_rrset_t *soa_rrs = 0;
	const knot_rdata_t *soa_rr = 0;
	soa_rrs = knot_node_rrset(knot_zone_contents_apex(contents),
	                            KNOT_RRTYPE_SOA);
	assert(soa_rrs != NULL);
	soa_rr = knot_rrset_rdata(soa_rrs);
	int64_t serial_ret = knot_rdata_soa_serial(soa_rr);
	if (serial_ret < 0) {
		rcu_read_unlock();
		return KNOTD_EINVAL;
	}
	uint32_t serial = (uint32_t)serial_ret;

	/* Load all pending changesets. */
	dbg_zones_verb("zones: loading all changesets of '%s' from SERIAL %u\n",
	               zd->conf->name, serial);
	knot_changesets_t* chsets = malloc(sizeof(knot_changesets_t));
	memset(chsets, 0, sizeof(knot_changesets_t));
	/*! \todo Check what should be the upper bound. */
	int ret = zones_load_changesets(zone, chsets, serial, serial - 1);
	if (ret == KNOTD_EOK || ret == KNOTD_ERANGE) {
		if (chsets->count > 0) {
			/* Apply changesets. */
			log_server_info("Applying '%zu' changesets from journal "
			                "to zone '%s'.\n",
			                chsets->count, zd->conf->name);
			knot_zone_contents_t *contents = NULL;
			int apply_ret = xfrin_apply_changesets(zone, chsets,
			                                       &contents);
			if (apply_ret != KNOT_EOK) {
				log_server_error("Failed to apply changesets to"
				                 " '%s' - Apply failed: %s\n",
				                 zd->conf->name,
				                 knot_strerror(apply_ret));
				ret = KNOTD_ERROR;

				// Cleanup old and new contents
				xfrin_rollback_update(zone->contents,
				                      &contents,
				                      &chsets->changes);
			} else {
				/* Switch zone immediately. */
				rcu_read_unlock();
				apply_ret = xfrin_switch_zone(zone, contents,
							      XFR_TYPE_IIN);
				rcu_read_lock();
				if (apply_ret == KNOT_EOK) {
					xfrin_cleanup_successful_update(
							&chsets->changes);
				} else {
					log_server_error("Failed to apply "
					  "changesets to '%s' - Switch failed: "
					  "%s\n", zd->conf->name,
					  knot_strerror(apply_ret));
					ret = KNOTD_ERROR;

					// Cleanup old and new contents
					xfrin_rollback_update(zone->contents,
					                      &contents,
					                      &chsets->changes);
				}
			}
		}
	} else {
		dbg_zones("zones: failed to load changesets - %s\n",
		          knotd_strerror(ret));
	}

	/* Free changesets and return. */
	rcu_read_unlock();
	knot_free_changesets(&chsets);
	return ret;
}

/*----------------------------------------------------------------------------*/

/*!
 * \brief Insert new zone to the database.
 *
 * Zones that should be retained are just added from the old database to the
 * new. New zones are loaded.
 *
 * \param z Zone configuration.
 * \param dst Used for returning new/updated zone.
 * \param ns Name server instance.
 * \param db_old Old zone database.
 *
 * \retval KNOTD_EOK if successful.
 * \retval KNOTD_EINVAL on invalid parameters.
 * \retval KNOTD_ENOENT if zone has no contents.
 * \retval KNOTD_ERROR on unspecified error.
 */
static int zones_insert_zone(conf_zone_t *z, knot_zone_t **dst,
                             knot_nameserver_t *ns)
{
	if (z == NULL || dst == NULL || ns == NULL) {
		return KNOTD_EINVAL;
	}
	
	/* Convert the zone name into a domain name. */
	/* Local allocation, will be discarded. */
	knot_dname_t *dname = knot_dname_new_from_str(z->name, strlen(z->name),
	                                              NULL);
	if (dname == NULL) {
		log_server_error("Error creating domain name from zone"
		                 " name\n");
		return KNOTD_EINVAL;
	}

	/* Try to find the zone in the current zone db. */
	rcu_read_lock();
	knot_zone_t *zone = knot_zonedb_find_zone(ns->zone_db, dname);
	rcu_read_unlock();

	/* Attempt to bootstrap if db or source does not exist. */
	int zone_changed = 0;
	struct stat s = {};
	int stat_ret = stat(z->file, &s);
	if (zone != NULL) {
		/* if found, check timestamp of the file against the
		 * loaded zone
		 */
		if (stat_ret == 0 && knot_zone_version(zone) < s.st_mtime) {
			zone_changed = 1;
		}
	} else {
		zone_changed = 1;
	}

	/* Reload zone file. */
	int is_bootstrapped = 0;
	int ret = KNOTD_ERROR;
	if (zone_changed) {
		/* Zone file not exists and has master set. */
		if (stat_ret < 0 && !EMPTY_LIST(z->acl.xfr_in)) {

			/* Create stub database. */
			dbg_zones_verb("zones: loading stub zone '%s' "
			               "for bootstrap.\n",
			               z->name);
			knot_dname_t *owner = knot_dname_deep_copy(dname);
			zone = knot_zone_new_empty(owner);
			if (zone != NULL) {
				ret = KNOTD_EOK;
				is_bootstrapped = 1;
			} else {
				dbg_zones("zones: failed to create "
				          "stub zone '%s'.\n", z->name);
				ret = KNOTD_ERROR;
			}
		} else {
			dbg_zones_verb("zones: loading zone '%s' from '%s'\n",
			               z->name, z->db);
			ret = zones_load_zone(&zone, z->name, z->file, z->db);
			const knot_node_t *apex = NULL;
			const knot_rrset_t *soa = NULL;
			if (ret == KNOTD_EOK) {
				apex = knot_zone_contents_apex(
					knot_zone_contents(zone));
				soa = knot_node_rrset(apex,
					KNOT_RRTYPE_SOA);
				int64_t sn = 0;
				if (apex && soa) {
					sn = knot_rdata_soa_serial(
					         knot_rrset_rdata(soa));
					if (sn < 0) sn = 0;
				}
				log_server_info("Loaded zone '%s' serial %u\n",
				                z->name, (uint32_t)sn);
			}
		}

		/* Evaluate. */
		if (ret == KNOTD_EOK && zone != NULL) {
			dbg_zones_verb("zones: inserted '%s' into "
			               "database, initializing data\n",
			               z->name);

			/* Initialize zone-related data. */
			zonedata_init(z, zone);
			*dst = zone;
		}
	} else {
		dbg_zones_verb("zones: found '%s' in old database, "
		               "copying to new.\n", z->name);
		if (stat_ret == 0) {
			log_server_info("Zone '%s' is up-to-date, no need "
			                "for reload.\n", z->name);
		}
		*dst = zone;
		ret = KNOTD_EOK;
	}

	/* Update zone data. */
	if (zone != NULL) {
		zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
		assert(zd != NULL);
		
		/* Log bootstrapped zone. */
		if (is_bootstrapped) {
			log_server_info("Will attempt to bootstrap zone"
			                " %s from AXFR master in %us.\n",
			                z->name,
			                zd->xfr_in.bootstrap_retry / 1000);
		}

		/* Update refs. */
		if (zd->conf != z) {
			conf_free_zone(zd->conf);
			zd->conf = z;
		}

		/* Update ACLs. */
		dbg_zones("Updating zone ACLs.\n");
		zones_set_acl(&zd->xfr_in.acl, &z->acl.xfr_in);
		zones_set_acl(&zd->xfr_out, &z->acl.xfr_out);
		zones_set_acl(&zd->notify_in, &z->acl.notify_in);
		zones_set_acl(&zd->notify_out, &z->acl.notify_out);

		/* Update server pointer. */
		zd->server = (server_t *)knot_ns_get_data(ns);

		/* Update master server address. */
		memset(&zd->xfr_in.tsig_key, 0, sizeof(knot_key_t));
		sockaddr_init(&zd->xfr_in.master, -1);
		sockaddr_init(&zd->xfr_in.via, -1);
		if (!EMPTY_LIST(z->acl.xfr_in)) {
			conf_remote_t *r = HEAD(z->acl.xfr_in);
			conf_iface_t *cfg_if = r->remote;
			sockaddr_set(&zd->xfr_in.master,
				     cfg_if->family,
				     cfg_if->address,
				     cfg_if->port);
			if (sockaddr_isvalid(&cfg_if->via)) {
				sockaddr_copy(&zd->xfr_in.via,
				              &cfg_if->via);
			}

			if (cfg_if->key) {
				memcpy(&zd->xfr_in.tsig_key,
				       cfg_if->key,
				       sizeof(knot_key_t));
			}

			dbg_zones("zones: using '%s@%d' as XFR master "
			          "for '%s'\n",
			          cfg_if->address,
			          cfg_if->port,
			          z->name);
		}

		/* Apply changesets from journal. */
		int ar = zones_journal_apply(zone);
		if (ar != KNOTD_EOK && ar != KNOTD_ERANGE && ar != KNOTD_ENOENT) {
			log_server_warning("Failed to apply changesets "
			                   "for zone '%s': %s\n",
			                   z->name, knotd_strerror(ar));
		}
		

		/* Update events scheduled for zone. */
		evsched_t *sch = ((server_t *)knot_ns_get_data(ns))->sched;
		zones_timers_update(zone, z, sch);
		
		/* Schedule IXFR database syncing. */
		/*! \note This has to remain separate as it must not be
		 *        triggered by a zone update or SOA response.
		 */
		/* Fetch zone data. */
		int sync_tmr = z->dbsync_timeout * 1000; /* s -> ms. */
		if (zd->ixfr_dbsync != NULL) {
			evsched_cancel(sch, zd->ixfr_dbsync);
			evsched_event_free(sch, zd->ixfr_dbsync);
			zd->ixfr_dbsync = NULL;
		}
		if (zd->ixfr_db != NULL) {
			zd->ixfr_dbsync = evsched_schedule_cb(
			                    sch, zones_zonefile_sync_ev,
			                    zone, sync_tmr);
			dbg_zones("zone: journal sync of '%s' "
			          "set to %d\n", z->name, sync_tmr);
		}

		/* Update ANY queries policy */
		if (zd->conf->disable_any) {
			rcu_read_lock();
			knot_zone_contents_t *contents =
			                knot_zone_get_contents(zone);

			/*! \todo This is actually updating zone contents.
			 *        It should be done in thread-safe way.
			 */
			if (contents) {
				knot_zone_contents_disable_any(contents);
			}

			rcu_read_unlock();
		}
		
		/* Calculate differences. */
		rcu_read_lock();
		knot_zone_t *z_old = knot_zonedb_find_zone(ns->zone_db,
		                                              dname);
		/* Ensure both new and old have zone contents. */
		knot_zone_contents_t *zc = knot_zone_get_contents(zone);
		knot_zone_contents_t *zc_old = knot_zone_get_contents(z_old);
		if (z->build_diffs && zc != NULL && zc_old != NULL && zone_changed) {
			int bd = zones_create_and_save_changesets(z_old, zone);
			if (bd == KNOTD_ENODIFF) {
				log_zone_warning("Zone file for '%s' changed, "
				                 "but serial didn't - "
				                 "won't create changesets.\n",
				                 z->name);
			} else if (bd != KNOTD_EOK) {
				log_zone_warning("Failed to calculate differences"
				                 " from the zone file update: "
				                 "%s\n", knotd_strerror(bd));
			}
		}
		rcu_read_unlock();
	}

	/* CLEANUP */
//	knot_zone_contents_dump(knot_zone_get_contents(zone), 1);

	/* Directly discard zone. */
	knot_dname_free(&dname);
	return ret;
}

/*! \brief Structure for multithreaded zone loading. */
struct zonewalk_t {
	knot_nameserver_t *ns;
	knot_zonedb_t *db_new;
	pthread_mutex_t lock;
	int inserted;
	unsigned qhead;
	unsigned qtail;
	conf_zone_t *q[];
	
};

/*! Thread entrypoint for loading zones. */
static int zonewalker(dthread_t *thread)
{
	if (thread == NULL) {
		return KNOTD_ERROR;
	}
	
	struct zonewalk_t *zw = (struct zonewalk_t *)thread->data;
	if (zw == NULL) {
		return KNOTD_ERROR;
	}

	unsigned i = 0;
	int inserted = 0;
	knot_zone_t **zones = NULL;
	size_t allocd = 0;
	for(;;) {
		/* Fetch queue head. */
		pthread_mutex_lock(&zw->lock);
		i = zw->qhead++;
		pthread_mutex_unlock(&zw->lock);
		if (i >= zw->qtail) {
			break;
		}
		
		if (mreserve((char **)&zones, sizeof(knot_zone_t*),
		             inserted + 1, 32, &allocd) < 0) {
			dbg_zones("zones: failed to reserve space for "
			          "loading zones\n");
			continue;
		}
		
		int ret = zones_insert_zone(zw->q[i], zones + inserted, zw->ns);
		if (ret == KNOTD_EOK) {
			++inserted;
		}
	}
	
	/* Collect results. */
	pthread_mutex_lock(&zw->lock);
	zw->inserted += inserted;
	for (int i = 0; i < inserted; ++i) {
		zonedata_t *zd = (zonedata_t *)knot_zone_data(zones[i]);
		if (knot_zonedb_add_zone(zw->db_new, zones[i]) != KNOT_EOK) {
			log_server_error("Failed to insert zone '%s' "
			                 "into database.\n", zd->conf->name);
			knot_zone_deep_free(zones + i, 0);
		} else {
			/* Unlink zone config from conf(),
			 * transferring ownership to zonedata. */
			rem_node(&zd->conf->n);
		}
	}
	pthread_mutex_unlock(&zw->lock);
	free(zones);
	
	return KNOTD_EOK;
}

/*!
 * \brief Fill the new database with zones.
 *
 * Zones that should be retained are just added from the old database to the
 * new. New zones are loaded.
 *
 * \param ns Name server instance.
 * \param zone_conf Zone configuration.
 * \param db_new New zone database.
 *
 * \return Number of inserted zones.
 */
static int zones_insert_zones(knot_nameserver_t *ns,
			      const list *zone_conf,
                              knot_zonedb_t *db_new)
{
	int inserted = 0;
	size_t zcount = 0;
	conf_zone_t *z = NULL;
	WALK_LIST(z, *zone_conf) {
		++zcount;
	}
	
	/* Initialize zonewalker. */
	size_t zwlen = sizeof(struct zonewalk_t) + zcount * sizeof(conf_zone_t*);
	struct zonewalk_t *zw = malloc(zwlen);
	if (zw != NULL) {
		memset(zw, 0, zwlen);
		zw->ns = ns;
		zw->db_new = db_new;
		zw->inserted = 0;
		if (pthread_mutex_init(&zw->lock, NULL) < 0) {
			free(zw);
			zw = NULL;
		} else {
			unsigned i = 0;
			WALK_LIST(z, *zone_conf) {
				zw->q[i++] = z;
			}
			zw->qhead = 0;
			zw->qtail = zcount;
		}
	}
	
	/* Initialize threads. */
	dt_unit_t *unit = NULL;
	if (zw != NULL) {
		unit = dt_create_coherent(dt_optimal_size(), &zonewalker, zw);
	}
	/* Single-thread fallback. */
	if (unit == NULL) {
		log_server_error("Couldn't initialize zone loading - %s\n",
		                 knotd_strerror(KNOTD_ENOMEM));
		return 0;
	}
	
	/* Start loading. */
	dt_start(unit);
	
	/* Wait for finish. */
	dt_join(unit);
	dt_delete(&unit);
	
	/* Collect counts. */
	inserted = zw->inserted;
	pthread_mutex_destroy(&zw->lock);
	free(zw);
	return inserted;
}

/*----------------------------------------------------------------------------*/
/*!
 * \brief Remove zones present in the configuration from the old database.
 *
 * After calling this function, the old zone database should contain only zones
 * that should be completely deleted.
 *
 * \param zone_conf Zone configuration.
 * \param db_old Old zone database to remove zones from.
 *
 * \retval KNOTD_EOK
 * \retval KNOTD_ERROR
 */
static int zones_remove_zones(const knot_zonedb_t *db_new,
                              knot_zonedb_t *db_old)
{
	const knot_zone_t **new_zones = knot_zonedb_zones(db_new);
	if (new_zones == NULL) {
		return KNOTD_ENOMEM;
	}

	for (int i = 0; i < knot_zonedb_zone_count(db_new); ++i) {
		/* try to find the new zone in the old DB
		 * if the pointers match, remove the zone from old DB
		 */
		/*! \todo Find better way of removing zone with given pointer.*/
		knot_zone_t *old_zone = knot_zonedb_find_zone(
		                        db_old, knot_zone_name(new_zones[i]));
		if (old_zone == new_zones[i]) {
dbg_zones_exec(
			char *name = knot_dname_to_str(knot_zone_name(old_zone));
			dbg_zones_verb("zones: zone pointers match, removing zone %s "
                                       "from database.\n", name);
			free(name);
);
			/* Invalidate ACLs - since we would need to copy each
			 * remote data and keep ownership, I think it's no harm
			 * to drop all ACLs for the discarded zone.
			 * refs #1976 */
			zonedata_t *zd = (zonedata_t*)knot_zone_data(old_zone);
			conf_zone_t *zconf = zd->conf;
			WALK_LIST_FREE(zconf->acl.xfr_in);
			WALK_LIST_FREE(zconf->acl.xfr_out);
			WALK_LIST_FREE(zconf->acl.notify_in);
			WALK_LIST_FREE(zconf->acl.notify_out);
			
			/* Remove from zone db. */
			knot_zone_t * rm = knot_zonedb_remove_zone(db_old,
			                              knot_zone_name(old_zone));
			assert(rm == old_zone);
		}
	}

	free(new_zones);

	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

static int zones_verify_tsig_query(const knot_packet_t *query,
                                   const knot_rrset_t *tsig_rr,
                                   const knot_key_t *key,
                                   knot_rcode_t *rcode, uint16_t *tsig_rcode,
                                   uint64_t *tsig_prev_time_signed)
{
	assert(tsig_rr != NULL);
	assert(key != NULL);
	assert(rcode != NULL);
	assert(tsig_rcode != NULL);

	/*
	 * 1) Check if we support the requested algorithm.
	 */
	tsig_algorithm_t alg = tsig_rdata_alg(tsig_rr);
	if (tsig_alg_digest_length(alg) == 0) {
		log_answer_info("Unsupported digest algorithm "
		                "requested, treating as bad key\n");
		/*! \todo [TSIG] It is unclear from RFC if I
		 *               should treat is as a bad key
		 *               or some other error.
		 */
		*rcode = KNOT_RCODE_NOTAUTH;
		*tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
		return KNOT_TSIG_EBADKEY;
	}

	const knot_dname_t *kname = knot_rrset_owner(tsig_rr);
	assert(kname != NULL);

	/*
	 * 2) Find the particular key used by the TSIG.
	 *    Check not only name, but also the algorithm.
	 */
	if (key && kname && knot_dname_compare(key->name, kname) == 0
	    && key->algorithm == alg) {
		dbg_zones_verb("Found claimed TSIG key for comparison\n");
	} else {
		*rcode = KNOT_RCODE_NOTAUTH;
		*tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
		return KNOT_TSIG_EBADKEY;
	}

	/*
	 * 3) Validate the query with TSIG.
	 */
	/* Prepare variables for TSIG */
	/*! \todo These need to be saved to the response somehow. */
	//size_t tsig_size = tsig_wire_maxsize(key);
	size_t digest_max_size = tsig_alg_digest_length(key->algorithm);
	//size_t digest_size = 0;
	//uint64_t tsig_prev_time_signed = 0;
	//uint8_t *digest = (uint8_t *)malloc(digest_max_size);
	//memset(digest, 0 , digest_max_size);

	/* Copy MAC from query. */
	dbg_zones_verb("Validating TSIG from query\n");

	//const uint8_t* mac = tsig_rdata_mac(tsig_rr);
	size_t mac_len = tsig_rdata_mac_length(tsig_rr);

	int ret = KNOT_EOK;

	if (mac_len > digest_max_size) {
		*rcode = KNOT_RCODE_FORMERR;
		dbg_zones("MAC length %zu exceeds digest "
		       "maximum size %zu\n", mac_len, digest_max_size);
		return KNOT_EMALF;
	} else {
		//memcpy(digest, mac, mac_len);
		//digest_size = mac_len;

		/* Check query TSIG. */
		ret = knot_tsig_server_check(tsig_rr,
		                             knot_packet_wireformat(query),
		                             knot_packet_size(query), key);
		dbg_zones_verb("knot_tsig_server_check() returned %s\n",
		               knot_strerror(ret));

		/* Evaluate TSIG check results. */
		switch(ret) {
		case KNOT_EOK:
			*rcode = KNOT_RCODE_NOERROR;
			break;
		case KNOT_TSIG_EBADKEY:
			*tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
			*rcode = KNOT_RCODE_NOTAUTH;
			break;
		case KNOT_TSIG_EBADSIG:
			*tsig_rcode = KNOT_TSIG_RCODE_BADSIG;
			*rcode = KNOT_RCODE_NOTAUTH;
			break;
		case KNOT_TSIG_EBADTIME:
			*tsig_rcode = KNOT_TSIG_RCODE_BADTIME;
			// store the time signed from the query
			*tsig_prev_time_signed = tsig_rdata_time_signed(tsig_rr);
			*rcode = KNOT_RCODE_NOTAUTH;
			break;
		case KNOT_EMALF:
			*rcode = KNOT_RCODE_FORMERR;
			break;
		default:
			*rcode = KNOT_RCODE_SERVFAIL;
		}
	}

	return ret;
}

/*----------------------------------------------------------------------------*/

static int zones_check_tsig_query(const knot_zone_t *zone,
                                  knot_packet_t *query,
                                  const sockaddr_t *addr,
                                  knot_rcode_t *rcode,
                                  uint16_t *tsig_rcode,
                                  knot_key_t **tsig_key_zone,
                                  uint64_t *tsig_prev_time_signed)
{
	assert(zone != NULL);
	assert(query != NULL);
	assert(rcode != NULL);
	assert(tsig_key_zone != NULL);

	const knot_rrset_t *tsig = knot_packet_tsig(query);

	// not required, TSIG is already found
//	if (knot_packet_additional_rrset_count(query) > 0) {
//		/*! \todo warning */
//		tsig = knot_packet_additional_rrset(query,
//		                 knot_packet_additional_rrset_count(query) - 1);
//		if (knot_rrset_type(tsig) == KNOT_RRTYPE_TSIG) {
//			dbg_zones_verb("found TSIG in normal query\n");
//		} else {
//			tsig = NULL; /* Invalidate if not TSIG RRTYPE. */
//		}
//	}

	if (tsig == NULL) {
		// no TSIG, this is completely valid
		/*! \note This function is (should be) called only in case of
		          normal query, i.e. we do not have to check ACL.
		 */
		*tsig_rcode = 0;
		return KNOT_EOK;
	}

	// if there is some TSIG in the query, find the TSIG associated with
	// the zone
	dbg_zones_verb("Checking zone and ACL.\n");
	int ret = zones_query_check_zone(zone, addr, tsig_key_zone, rcode);

	
	/* Accept found OR unknown key results. */
	if (ret == KNOTD_EOK || ret == KNOTD_EACCES) {
		if (*tsig_key_zone != NULL) {
			// everything OK, so check TSIG
			dbg_zones_verb("Verifying TSIG.\n");
			ret = zones_verify_tsig_query(query, tsig, *tsig_key_zone,
			                              rcode, tsig_rcode,
			                              tsig_prev_time_signed);
		} else {
			dbg_zones_verb("No key configured for zone.\n");
			// no key configured for zone, return BADKEY
			*tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
			*rcode = KNOT_RCODE_NOTAUTH;
			ret = KNOT_TSIG_EBADKEY;
		}
	}

	// save TSIG RR to query structure
//	knot_packet_set_tsig(query, tsig);

	return ret;
}

/*----------------------------------------------------------------------------*/
/* API functions                                                              */
/*----------------------------------------------------------------------------*/

int zones_update_db_from_config(const conf_t *conf, knot_nameserver_t *ns,
                               knot_zonedb_t **db_old)
{
	/* Check parameters */
	if (conf == NULL || ns == NULL) {
		return KNOTD_EINVAL;
	}

	/* Lock RCU to ensure none will deallocate any data under our hands. */
	rcu_read_lock();
	
	/* Grab a pointer to the old database */
	if (ns->zone_db == NULL) {
		rcu_read_unlock();
		log_server_error("Missing zone database in nameserver structure"
		                 ".\n");
		return KNOTD_ERROR;
	}
	rcu_read_unlock();

	/* Create new zone DB */
	knot_zonedb_t *db_new = knot_zonedb_new();
	if (db_new == NULL) {
		return KNOTD_ERROR;
	}

	log_server_info("Loading %d compiled zones...\n", conf->zones_count);

	/* Insert all required zones to the new zone DB. */
	/*! \warning RCU must not be locked as some contents switching will 
	             be required. */
	int inserted = zones_insert_zones(ns, &conf->zones, db_new);
	
	log_server_info("Loaded %d out of %d zones.\n", inserted,
	                conf->zones_count);

	if (inserted != conf->zones_count) {
		log_server_warning("Not all the zones were loaded.\n");
	}
	
	/* Lock RCU to ensure none will deallocate any data under our hands. */
	rcu_read_lock();
	*db_old = ns->zone_db;

	dbg_zones_detail("zones: old db in nameserver: %p, old db stored: %p, "
	                 "new db: %p\n", ns->zone_db, *db_old, db_new);

	/* Switch the databases. */
	(void)rcu_xchg_pointer(&ns->zone_db, db_new);

	dbg_zones_detail("db in nameserver: %p, old db stored: %p, new db: %p\n",
	                 ns->zone_db, *db_old, db_new);

	/*
	 * Remove all zones present in the new DB from the old DB.
	 * No new thread can access these zones in the old DB, as the
	 * databases are already switched.
	 *
	 * Beware - only the exact same zones (same pointer) may be removed.
	 * All other have been loaded again so that the old must be destroyed.
	 */
	int ret = zones_remove_zones(db_new, *db_old);
	
	/* Unlock RCU, messing with any data will not affect us now */
	rcu_read_unlock();
	
	if (ret != KNOTD_EOK) {
		return ret;
	}

	return KNOTD_EOK;
}

int zones_zonefile_sync(knot_zone_t *zone, journal_t *journal)
{
	if (!zone) {
		return KNOTD_EINVAL;
	}
	if (!zone->data) {
		return KNOTD_EINVAL;
	}
	if (journal == NULL) {
		return KNOTD_EINVAL;
	}

	/* Fetch zone data. */
	int ret = KNOTD_EOK;
	zonedata_t *zd = (zonedata_t *)zone->data;

	/* Lock zone data. */
	pthread_mutex_lock(&zd->lock);

	/* Lock RCU for zone contents. */
	rcu_read_lock();

	knot_zone_contents_t *contents = knot_zone_get_contents(zone);
	if (!contents) {
		pthread_mutex_unlock(&zd->lock);
		rcu_read_unlock();
		return KNOTD_EINVAL;
	}

	/* Latest zone serial. */
	const knot_rrset_t *soa_rrs = 0;
	const knot_rdata_t *soa_rr = 0;
	soa_rrs = knot_node_rrset(knot_zone_contents_apex(contents),
	                            KNOT_RRTYPE_SOA);
	assert(soa_rrs != NULL);

	soa_rr = knot_rrset_rdata(soa_rrs);
	int64_t serial_ret = knot_rdata_soa_serial(soa_rr);
	if (serial_ret < 0) {
		pthread_mutex_unlock(&zd->lock);
		rcu_read_unlock();
		return KNOTD_EINVAL;
	}
	uint32_t serial_to = (uint32_t)serial_ret;

	/* Check for difference against zonefile serial. */
	if (zd->zonefile_serial != serial_to) {

		/* Save zone to zonefile. */
		dbg_zones("zones: syncing '%s' differences to '%s' "
		          "(SOA serial %u)\n",
		          zd->conf->name, zd->conf->file, serial_to);
		ret = zones_dump_zone_text(contents, zd->conf->file);
		if (ret != KNOTD_EOK) {
			log_zone_warning("Failed to apply differences "
			                 "'%s' to '%s'\n",
			                 zd->conf->name, zd->conf->file);
			pthread_mutex_unlock(&zd->lock);
			rcu_read_unlock();
			return ret;
		}
		
		/* Save zone to binary db file. */
		ret = zones_dump_zone_binary(contents, zd->conf->db, zd->conf->file);
		if (ret != KNOTD_EOK) {
			log_zone_warning("Failed to apply differences "
			                 "'%s' to '%s'\n",
			                 zd->conf->name, zd->conf->db);
			pthread_mutex_unlock(&zd->lock);
			rcu_read_unlock();
			return KNOTD_ERROR;
		}

		/* Update journal entries. */
		dbg_zones_verb("zones: unmarking all dirty nodes "
		               "in '%s' journal\n",
		               zd->conf->name);
		journal_walk(journal, zones_ixfrdb_sync_apply);

		/* Update zone file serial. */
		dbg_zones("zones: new '%s' zonefile serial is %u\n",
		          zd->conf->name, serial_to);
		zd->zonefile_serial = serial_to;
	} else {
		dbg_zones("zones: '%s' zonefile is in sync "
		          "with differences\n", zd->conf->name);
		ret = KNOTD_ERANGE;
	}

	/* Unlock zone data. */
	pthread_mutex_unlock(&zd->lock);

	/* Unlock RCU. */
	rcu_read_unlock();

	return ret;
}

/*----------------------------------------------------------------------------*/

int zones_query_check_zone(const knot_zone_t *zone, const sockaddr_t *addr,
                           knot_key_t **tsig_key, knot_rcode_t *rcode)
{
	if (addr == NULL || tsig_key == NULL || rcode == NULL) {
		dbg_zones_verb("Wrong arguments.\n");

		if (rcode != NULL) {
			*rcode = KNOT_RCODE_SERVFAIL;
		}
		return KNOTD_EINVAL;
	}

	/* Check zone data. */
	const zonedata_t *zd = (const zonedata_t *)knot_zone_data(zone);
	if (zd == NULL) {
		dbg_zones("zones: invalid zone data for zone %p\n", zone);
		*rcode = KNOT_RCODE_SERVFAIL;
		return KNOTD_ERROR;
	}

	/* Check xfr-out ACL */
	acl_key_t *match = NULL;
	if (acl_match(zd->xfr_out, addr, &match) == ACL_DENY) {
		*rcode = KNOT_RCODE_REFUSED;
		return KNOTD_EACCES;
	} else {
		dbg_zones("zones: authorized query or request for XFR "
		          "'%s/OUT'. match=%p\n", zd->conf->name, match);
		if (match) {
			/* Save configured TSIG key for comparison. */
			conf_iface_t *iface = (conf_iface_t*)(match->val);
			dbg_zones_detail("iface=%p, iface->key=%p\n",
					 iface, iface->key);
			*tsig_key = iface->key;
		}
	}
	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

int zones_xfr_check_zone(knot_ns_xfr_t *xfr, knot_rcode_t *rcode)
{
	if (xfr == NULL || rcode == NULL) {
		return KNOTD_EINVAL;
	}

	/* Check if the zone is found. */
	if (xfr->zone == NULL) {
		*rcode = KNOT_RCODE_REFUSED;
		return KNOTD_EACCES;
	}

	/* Check zone contents. */
	if (knot_zone_contents(xfr->zone) == NULL) {
		dbg_zones("zones: invalid zone contents for zone %p\n",
		          xfr->zone);
		*rcode = KNOT_RCODE_SERVFAIL;
		return KNOTD_EEXPIRED;
	}

	return zones_query_check_zone(xfr->zone, &xfr->addr, &xfr->tsig_key,
	                              rcode);
}

/*----------------------------------------------------------------------------*/
/*! \todo This function is here only because TSIG key is associated with the
 *        zone via zonedata. If it was in the zone structure (which would be
 *        IMHO ok, this whole function could be moved to nameserver.c.
 */
int zones_normal_query_answer(knot_nameserver_t *nameserver,
                              knot_packet_t *query, const sockaddr_t *addr,
                              uint8_t *resp_wire, size_t *rsize,
                              knot_ns_transport_t transport)
{
	rcu_read_lock();

	knot_packet_t *resp = NULL;
	const knot_zone_t *zone = NULL;

	dbg_zones_verb("Preparing response structure.\n");
	int ret = knot_ns_prep_normal_response(nameserver, query, &resp, &zone,
	                                       (transport == NS_TRANSPORT_TCP)
	                                       ? *rsize : 0);

	// check for TSIG in the query
	// not required, TSIG is already found if it is there
//	if (knot_packet_additional_rrset_count(query) > 0) {
//		/*! \todo warning */
//		const knot_rrset_t *tsig = knot_packet_additional_rrset(query,
//		                 knot_packet_additional_rrset_count(query) - 1);
//		if (knot_rrset_type(tsig) == KNOT_RRTYPE_TSIG) {
//			dbg_zones_verb("found TSIG in normal query\n");
//			knot_packet_set_tsig(query, tsig);
//		}
//	}

	knot_rcode_t rcode = 0;

	switch (ret) {
	case KNOT_EOK:
		rcode = KNOT_RCODE_NOERROR;
		break;
	case KNOT_EMALF:
		// no TSIG signing in this case
		rcode = KNOT_RCODE_FORMERR;
		break;
	default:
		// no TSIG signing in this case
		rcode = KNOT_RCODE_SERVFAIL;
		break;
	}

	if (rcode == KNOT_RCODE_NOERROR
	    && ((zone == NULL && knot_packet_tsig(query) == NULL)
	        || (knot_packet_qclass(query) != KNOT_CLASS_IN
	            && knot_packet_qclass(query) != KNOT_CLASS_ANY))) {
		/*! \todo If there is TSIG, this should be probably handled
		 *        as a key error.
		 */
		rcode = KNOT_RCODE_REFUSED;
	}

	if (rcode != KNOT_RCODE_NOERROR) {
		dbg_zones_verb("Failed preparing response structure: %s.\n",
		               knot_strerror(rcode));
		if (resp == NULL) {
			knot_ns_error_response_from_query(nameserver, query,
			                                  rcode, resp_wire,
			                                  rsize);
			rcu_read_unlock();
			return KNOT_EOK;
		}
		knot_ns_error_response_full(nameserver, resp, rcode, resp_wire,
		                            rsize);
	} else {
		/*
		 * Now we have zone. Verify TSIG if it is in the packet.
		 */
		assert(resp != NULL);
		assert(rcode == KNOT_RCODE_NOERROR);
		uint16_t tsig_rcode = 0;
		knot_key_t *tsig_key_zone = NULL;
		uint64_t tsig_prev_time_signed = 0;
		/*! \todo Verify, as it was uninitialized! */

		size_t answer_size = *rsize;
		int ret = KNOT_EOK;

		if (zone == NULL) {
			assert(knot_packet_tsig(query) != NULL);
			// treat as BADKEY error
			/*! \todo Is this OK?? */
			rcode = KNOT_RCODE_NOTAUTH;
			tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
			ret = KNOT_TSIG_EBADKEY;
		} else {
			dbg_zones_verb("Checking TSIG in query.\n");
			ret = zones_check_tsig_query(zone, query, addr,
			                             &rcode, &tsig_rcode,
			                             &tsig_key_zone,
			                             &tsig_prev_time_signed);
		}

		if (ret == KNOT_EOK) {
			dbg_zones_verb("TSIG check successful. Answering "
			               "query.\n");
			assert(tsig_rcode == 0);

			// reserve place for the TSIG
			if (tsig_key_zone != NULL) {
				size_t tsig_max_size =
				         tsig_wire_maxsize(tsig_key_zone);
				knot_packet_set_tsig_size(resp, tsig_max_size);
			}

			// handle IXFR queries
			if (knot_packet_qtype(query) == KNOT_RRTYPE_IXFR) {
				assert(transport == NS_TRANSPORT_UDP);
				ret = knot_ns_answer_ixfr_udp(nameserver, zone,
				                              resp, resp_wire,
				                              &answer_size);
			} else {
				ret = knot_ns_answer_normal(nameserver, zone,
				                            resp, resp_wire,
				                            &answer_size,
				                            transport ==
				                            NS_TRANSPORT_UDP);
			}

			dbg_zones_detail("rsize = %zu\n", *rsize);
			dbg_zones_detail("answer_size = %zu\n", answer_size);

			assert(ret == KNOT_EOK);

			// sign the message
			if (tsig_key_zone != NULL) {
				dbg_zones_verb("Signing message with TSIG.\n");
				// TODO check
				//*rsize = answer_size;

				const knot_rrset_t *tsig =
				      knot_packet_tsig(knot_packet_query(resp));

				size_t digest_max_size =
				                tsig_alg_digest_length(
				                      tsig_key_zone->algorithm);
				uint8_t *digest = (uint8_t *)malloc(
				                        digest_max_size);
				if (digest == NULL) {
					knot_packet_free(&resp);
					rcu_read_unlock();
					return KNOT_ENOMEM;
				}
				size_t digest_size = digest_max_size;

				ret = knot_tsig_sign(resp_wire, &answer_size,
				               *rsize, tsig_rdata_mac(tsig),
				               tsig_rdata_mac_length(tsig),
				               digest, &digest_size,
				               tsig_key_zone, tsig_rcode,
				               tsig_prev_time_signed);
				
				free(digest);

				dbg_zones_detail("answer_size = %zu\n",
				                 answer_size);

				if (ret != KNOT_EOK) {
					dbg_zones_verb("Failed to sign message:"
					            "%s\n", knot_strerror(ret));
					rcode = KNOT_RCODE_SERVFAIL;
				} else {
					*rsize = answer_size;
				}
			} else {
				*rsize = answer_size;
			}
		} else {
			dbg_zones_verb("Failed TSIG check: %s, TSIG err: %u.\n",
			               knot_strerror(ret), tsig_rcode);

			if (tsig_rcode != 0) {
				dbg_zones_verb("Sending TSIG error.\n");
				// first, convert the response to wire format
				answer_size = *rsize;
				knot_response_set_rcode(resp, rcode);

				ret = ns_response_to_wire(resp, resp_wire,
				                          &answer_size);

				dbg_zones_detail("Packet to wire returned %d\n",
				                 ret);

				// then add the TSIG to the wire format
				if (ret == KNOT_EOK &&
				    tsig_rcode != KNOT_TSIG_RCODE_BADTIME) {
					dbg_zones_verb("Adding TSIG.\n");
					ret = knot_tsig_add(resp_wire,
					                    &answer_size,
					                    *rsize, tsig_rcode,
					                     knot_packet_tsig(
					                            query));

					*rsize = answer_size;

				} else if (tsig_rcode
				           == KNOT_TSIG_RCODE_BADTIME) {
					dbg_zones_verb("Signing error resp.\n");
					//*rsize = answer_size;

					const knot_rrset_t *tsig =
					      knot_packet_tsig(
					          knot_packet_query(resp));

					size_t digest_max_size =
					           tsig_alg_digest_length(
					              tsig_key_zone->algorithm);
					uint8_t *digest = (uint8_t *)malloc(
					                       digest_max_size);
					if (digest == NULL) {
						knot_packet_free(&resp);
						rcu_read_unlock();
						return KNOT_ENOMEM;
					}
					size_t digest_size = digest_max_size;

					ret = knot_tsig_sign(resp_wire,
					    &answer_size, *rsize,
					    tsig_rdata_mac(tsig),
					    tsig_rdata_mac_length(tsig),
					    digest, &digest_size, tsig_key_zone,
					    tsig_rcode, tsig_prev_time_signed);
					
					// no need to keep the digest
					free(digest);

					*rsize = answer_size;
				} else {
					dbg_zones_verb("Failed.\n");
					rcode = KNOT_RCODE_SERVFAIL;
				}
			}
			// in other case the RCODE is set and ret != KNOT_EOK
			// and a normal error is returned below
		}

		if (ret != KNOT_EOK) {
			knot_ns_error_response_full(nameserver, resp,
			                            rcode, resp_wire,
			                            rsize);
		}
	}

	knot_packet_free(&resp);
	rcu_read_unlock();

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/
/*! \todo [DDNS] Update parameters, copied from normal_query_answer() */
int zones_process_update(knot_nameserver_t *nameserver,
                         knot_packet_t *query, const sockaddr_t *addr,
                         uint8_t *resp_wire, size_t *rsize,
                         knot_ns_transport_t transport)
{
	rcu_read_lock();

	knot_packet_t *resp = NULL;
	knot_zone_t *zone = NULL;

	dbg_zones_verb("Preparing response structure.\n");
	
	// Parse rest of the query, prepare response, find zone
	int ret = knot_ns_prep_update_response(nameserver, query, &resp, &zone,
	                                       (transport == NS_TRANSPORT_TCP)
	                                       ? *rsize : 0);

	knot_rcode_t rcode = 0;

	switch (ret) {
	case KNOT_EOK:
		rcode = KNOT_RCODE_NOERROR;
		break;
	case KNOT_EMALF:
		// no TSIG signing in this case
		rcode = KNOT_RCODE_FORMERR;
		break;
	default:
		// no TSIG signing in this case
		rcode = KNOT_RCODE_SERVFAIL;
		break;
	}

	if (rcode == KNOT_RCODE_NOERROR
	    && ((zone == NULL && knot_packet_tsig(query) == NULL)
	        || (knot_packet_qclass(query) != KNOT_CLASS_IN))) {
		/*! \todo If there is TSIG, this should be probably handled
		 *        as a key error.
		 */
		rcode = KNOT_RCODE_REFUSED;
	}

	if (rcode != KNOT_RCODE_NOERROR) {
		dbg_zones_verb("Failed preparing response structure: %s.\n",
		               knot_strerror(rcode));

		// Error response without Question section
		knot_ns_error_response_from_query_wire(nameserver,
		                          knot_packet_wireformat(query),
		                          knot_packet_size(query),
		                          rcode, resp_wire, rsize);
	} else {
		/*
		 * Now we have zone. Verify TSIG if it is in the packet.
		 */
		assert(resp != NULL);
		assert(rcode == KNOT_RCODE_NOERROR);
		uint16_t tsig_rcode = 0;
		knot_key_t *tsig_key_zone = NULL;
		uint64_t tsig_prev_time_signed = 0;
		/*! \todo Verify, as it was uninitialized! */

		size_t answer_size = *rsize;
		int ret = KNOT_EOK;

		if (zone == NULL) {
			assert(knot_packet_tsig(query) != NULL);
			// treat as BADKEY error
			/*! \todo Is this OK?? */
			rcode = KNOT_RCODE_NOTAUTH;
			tsig_rcode = KNOT_TSIG_RCODE_BADKEY;
			ret = KNOT_TSIG_EBADKEY;
		} else {
			dbg_zones_verb("Checking TSIG in query.\n");
			ret = zones_check_tsig_query(zone, query, addr,
			                             &rcode, &tsig_rcode,
			                             &tsig_key_zone,
			                             &tsig_prev_time_signed);
		}

		if (ret == KNOT_EOK) {
			dbg_zones_verb("TSIG check successful. Answering "
			               "query.\n");
			assert(tsig_rcode == 0);

			/*! \todo [DDNS] Forwarding should be handled somewhere
			 *        here. 
			 */
			
			// reserve place for the TSIG
			if (tsig_key_zone != NULL) {
				size_t tsig_max_size =
				         tsig_wire_maxsize(tsig_key_zone);
				knot_packet_set_tsig_size(resp, tsig_max_size);
			}

			/* We must prepare a changestets_t structure even if
			* there is only one changeset - because of the API. */
			knot_changesets_t *chgsets = NULL;
			ret = knot_changeset_allocate(&chgsets);
			if (ret != KNOT_EOK) {
				knot_ns_error_response_from_query_wire(
					nameserver, 
					knot_packet_wireformat(query),
					knot_packet_size(query),
					KNOT_RCODE_SERVFAIL, resp_wire, rsize);
				ret = KNOT_EOK;
			}
			
			/*! \todo Is continuing after error OK??? */
			
			assert(chgsets->allocated >= 1);
			
			/* 1) Process the incoming packet, prepare 
			 *    prerequisities and changeset.
			 */
			dbg_zones_verb("Processing UPDATE packet.\n");
			ret = knot_ns_process_update(knot_packet_query(resp),
			                             knot_zone_contents(zone),
			                             &chgsets->sets[0], &rcode);
			
			if (ret != KNOT_EOK) {
				dbg_zones("Processing UPDATE packet failed: %s"
				          "\n", knot_strerror(ret));
				knot_ns_error_response_from_query_wire(
					nameserver, 
					knot_packet_wireformat(query),
					knot_packet_size(query),
					rcode, resp_wire, rsize);
				ret = KNOT_EOK;
			} else {	
				/* 2) Save changeset to journal.
				 *    Apply changeset to zone.
				 *    Commit changeset to journal.
				 *    Switch the zone.
				 */
				knot_zone_contents_t *contents_new = NULL;
				/*! \todo [DDNS] msgpref */
				dbg_zones_verb("Storing and applying "
				               "changesets.\n");
				ret = zones_store_and_apply_chgsets(chgsets, 
					zone, &contents_new, "", 
					XFR_TYPE_UPDATE);
				
				if (ret != KNOT_EOK) {
					dbg_zones_verb("Storing and applying "
					              "changesets failed: %s."
					              "\n", knot_strerror(ret));
					knot_ns_error_response_from_query_wire(
						nameserver, 
						knot_packet_wireformat(query),
						knot_packet_size(query),
						rcode, resp_wire, rsize);
					ret = KNOT_EOK;
				}
				
			}
			/* 3) Prepare DDNS response. */
			dbg_zones_verb("Preparing NOERROR UPDATE response.\n");
			knot_response_set_rcode(resp, KNOT_RCODE_NOERROR);
			ret = knot_packet_to_wire(resp, &resp_wire, 
			                          &answer_size);
			if (ret != KNOT_EOK) {
				knot_ns_error_response_from_query_wire(
					nameserver, 
					knot_packet_wireformat(query),
					knot_packet_size(query),
					rcode, resp_wire, rsize);
				ret = KNOT_EOK;
			}

			dbg_zones_detail("rsize = %zu\n", *rsize);
			dbg_zones_detail("answer_size = %zu\n", answer_size);

			assert(ret == KNOT_EOK);

			// sign the message
			if (tsig_key_zone != NULL) {
				dbg_zones_verb("Signing message with TSIG.\n");
				// TODO check
				//*rsize = answer_size;

				const knot_rrset_t *tsig =
				      knot_packet_tsig(knot_packet_query(resp));

				size_t digest_max_size =
				                tsig_alg_digest_length(
				                      tsig_key_zone->algorithm);
				uint8_t *digest = (uint8_t *)malloc(
				                        digest_max_size);
				if (digest == NULL) {
					knot_packet_free(&resp);
					rcu_read_unlock();
					return KNOT_ENOMEM;
				}
				size_t digest_size = digest_max_size;

				ret = knot_tsig_sign(resp_wire, &answer_size,
				               *rsize, tsig_rdata_mac(tsig),
				               tsig_rdata_mac_length(tsig),
				               digest, &digest_size,
				               tsig_key_zone, tsig_rcode,
				               tsig_prev_time_signed);

				free(digest);

				dbg_zones_detail("answer_size = %zu\n",
				                 answer_size);

				if (ret != KNOT_EOK) {
					dbg_zones_verb("Failed to sign message:"
					            "%s\n", knot_strerror(ret));
					rcode = KNOT_RCODE_SERVFAIL;
				} else {
					*rsize = answer_size;
				}
			} else {
				*rsize = answer_size;
			}
		} else {
			dbg_zones_verb("Failed TSIG check: %s, TSIG err: %u.\n",
			               knot_strerror(ret), tsig_rcode);

			if (tsig_rcode != 0) {
				dbg_zones_verb("Sending TSIG error.\n");
				// first, convert the response to wire format
				answer_size = *rsize;
				knot_response_set_rcode(resp, rcode);

				ret = ns_response_to_wire(resp, resp_wire,
				                          &answer_size);

				dbg_zones_detail("Packet to wire returned %d\n",
				                 ret);

				// then add the TSIG to the wire format
				if (ret == KNOT_EOK &&
				    tsig_rcode != KNOT_TSIG_RCODE_BADTIME) {
					dbg_zones_verb("Adding TSIG.\n");
					ret = knot_tsig_add(resp_wire,
					                    &answer_size,
					                    *rsize, tsig_rcode,
					                     knot_packet_tsig(
					                            query));

					*rsize = answer_size;

				} else if (tsig_rcode
				           == KNOT_TSIG_RCODE_BADTIME) {
					dbg_zones_verb("Signing error resp.\n");
					//*rsize = answer_size;

					const knot_rrset_t *tsig =
					      knot_packet_tsig(
					          knot_packet_query(resp));

					size_t digest_max_size =
					           tsig_alg_digest_length(
					              tsig_key_zone->algorithm);
					uint8_t *digest = (uint8_t *)malloc(
					                       digest_max_size);
					if (digest == NULL) {
						knot_packet_free(&resp);
						rcu_read_unlock();
						return KNOT_ENOMEM;
					}
					size_t digest_size = digest_max_size;

					ret = knot_tsig_sign(resp_wire,
					    &answer_size, *rsize,
					    tsig_rdata_mac(tsig),
					    tsig_rdata_mac_length(tsig),
					    digest, &digest_size, tsig_key_zone,
					    tsig_rcode, tsig_prev_time_signed);

					// no need to keep the digest
					free(digest);

					*rsize = answer_size;
				} else {
					dbg_zones_verb("Failed.\n");
					rcode = KNOT_RCODE_SERVFAIL;
				}
			}
			// in other case the RCODE is set and ret != KNOT_EOK
			// and a normal error is returned below
		}

		if (ret != KNOT_EOK) {
			// Error response without Question section
			knot_ns_error_response_from_query_wire(nameserver,
			                          knot_packet_wireformat(query),
			                          knot_packet_size(query),
			                          rcode, resp_wire, rsize);
		}
	}

	knot_packet_free(&resp);
	rcu_read_unlock();

	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

int zones_process_response(knot_nameserver_t *nameserver, 
                           sockaddr_t *from,
                           knot_packet_t *packet, uint8_t *response_wire,
                           size_t *rsize)
{
	if (!packet || !rsize || nameserver == NULL || from == NULL ||
	    response_wire == NULL) {
		return KNOTD_EINVAL;
	}
	
	/* Declare no response. */
	*rsize = 0;

	/* Handle SOA query response, cancel EXPIRE timer
	 * and start AXFR transfer if needed.
	 * Reset REFRESH timer on finish.
	 */
	if (knot_packet_qtype(packet) == KNOT_RRTYPE_SOA) {
		
		if (knot_packet_rcode(packet) != KNOT_RCODE_NOERROR) {
			/*! \todo Handle error response. */
			return KNOTD_ERROR;
		}

		/* Find matching zone and ID. */
		rcu_read_lock();
		const knot_dname_t *zone_name = knot_packet_qname(packet);
		/*! \todo Change the access to the zone db. */
		knot_zone_t *zone = knot_zonedb_find_zone(
		                        nameserver->zone_db,
		                        zone_name);

		/* Get zone contents. */
		const knot_zone_contents_t *contents =
				knot_zone_contents(zone);

		if (!zone || !knot_zone_data(zone) || !contents) {
			rcu_read_unlock();
			return KNOTD_EINVAL;
		}

		/* Match ID against awaited. */
		zonedata_t *zd = (zonedata_t *)knot_zone_data(zone);
		uint16_t pkt_id = knot_packet_id(packet);
		if ((int)pkt_id != zd->xfr_in.next_id) {
			rcu_read_unlock();
			return KNOTD_ERROR;
		}

		/* Check SOA SERIAL. */
		int ret = xfrin_transfer_needed(contents, packet);
		dbg_zones_verb("xfrin_transfer_needed() returned %s\n",
		               knot_strerror(ret));
		if (ret < 0) {
			/* RETRY/EXPIRE timers running, do not interfere. */
			rcu_read_unlock();
			return KNOTD_ERROR;
		}
		
		/* No updates available. */
		evsched_t *sched =
			((server_t *)knot_ns_get_data(nameserver))->sched;
		if (ret == 0) {
			/* Reinstall timers. */
			zones_timers_update(zone, zd->conf, sched);
			rcu_read_unlock();
			return KNOTD_EUPTODATE;
		}
		
		assert(ret > 0);
		
		/* Already transferring. */
		int xfrtype = zones_transfer_to_use(zd);
		if (pthread_mutex_trylock(&zd->xfr_in.lock) != 0) {
			/* Unlock zone contents. */
			dbg_zones("zones: SOA response received, but zone is "
			          "being transferred, refusing to start another "
			          "transfer\n");
			rcu_read_unlock();
			return KNOTD_EOK;
		} else {
			++zd->xfr_in.scheduled;
			pthread_mutex_unlock(&zd->xfr_in.lock);
		}

		/* Prepare XFR client transfer. */
		knot_ns_xfr_t xfr_req;
		memset(&xfr_req, 0, sizeof(knot_ns_xfr_t));
		memcpy(&xfr_req.addr, &zd->xfr_in.master, sizeof(sockaddr_t));
		memcpy(&xfr_req.saddr, &zd->xfr_in.via, sizeof(sockaddr_t));
		xfr_req.zone = (void *)zone;
		xfr_req.send = zones_send_cb;

		/* Select transfer method. */
		xfr_req.type = xfrtype;
		
		/* Select TSIG key. */
		if (zd->xfr_in.tsig_key.name) {
			xfr_req.tsig_key = &zd->xfr_in.tsig_key;
		}

		/* Unlock zone contents. */
		rcu_read_unlock();

		/* Retain pointer to zone for processing. */
		knot_zone_retain(xfr_req.zone);
		ret = xfr_request(((server_t *)knot_ns_get_data(
		                  nameserver))->xfr_h, &xfr_req);
		if (ret != KNOTD_EOK) {
			knot_zone_release(xfr_req.zone); /* Discard */
		}
	}

	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

knot_ns_xfr_type_t zones_transfer_to_use(zonedata_t *data)
{
	if (data == NULL || data->ixfr_db == NULL) {
		return XFR_TYPE_AIN;
	}
	
	return XFR_TYPE_IIN;
}

/*----------------------------------------------------------------------------*/

static int zones_open_free_filename(const char *old_name, char **new_name)
{
	/* find zone name not present on the disk */
	size_t name_size = strlen(old_name);
	*new_name = malloc(name_size + 7 + 1);
	if (*new_name == NULL) {
		return -1;
	}
	memcpy(*new_name, old_name, name_size + 1);
	strncat(*new_name, ".XXXXXX", 7);
	dbg_zones_verb("zones: creating temporary zone file\n");
	mode_t old_mode = umask(077);
	int fd = mkstemp(*new_name);
	(void) umask(old_mode);
	if (fd < 0) {
		dbg_zones_verb("zones: couldn't create temporary zone file\n");
		free(*new_name);
		*new_name = NULL;
	}
	
	return fd;
}

/*----------------------------------------------------------------------------*/

static int zones_dump_zone_text(knot_zone_contents_t *zone, const char *fname)
{
	assert(zone != NULL && fname != NULL);

	char *new_fname = NULL;
	int fd = zones_open_free_filename(fname, &new_fname);
	if (fd < 0) {
		log_zone_warning("Failed to find filename for temporary "
		                 "storage of the transferred zone.\n");
		return KNOTD_ERROR;
	}
	
	FILE *f = fdopen(fd, "w");
	if (f == NULL) {
		log_zone_warning("Failed to open file descriptor for text zone.\n");
		unlink(new_fname);
		free(new_fname);
		return KNOTD_ERROR;
	}
	
	if (zone_dump_text(zone, f) != KNOTD_EOK) {
		log_zone_warning("Failed to save the transferred zone to '%s'.\n",
		                 new_fname);
		fclose(f);
		unlink(new_fname);
		free(new_fname);
		return KNOTD_ERROR;
	}
	
	/* Set zone file rights to 0640. */
	fchmod(fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);

	/* Swap temporary zonefile and new zonefile. */
	fclose(f);
	int ret = rename(new_fname, fname);
	if (ret < 0 && ret != EEXIST) {
		log_zone_warning("Failed to replace old zone file '%s'' with a new"
		                 " zone file '%s'.\n", fname, new_fname);
		unlink(new_fname);
		free(new_fname);
		return KNOTD_ERROR;
	}
	
	free(new_fname);
	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

static int zones_dump_zone_binary(knot_zone_contents_t *zone, 
                                   const char *zonedb,
                                   const char *zonefile)
{
	assert(zone != NULL && zonedb != NULL);

	char *new_zonedb = NULL;
	int fd = zones_open_free_filename(zonedb, &new_zonedb);
	if (fd < 0) {
		dbg_zones("zones: failed to find free filename for temporary "
		          "storage of the zone binary file '%s'\n",
		          zonedb);
		return KNOTD_ERROR;
	}

	crc_t crc_value = 0;
	if (knot_zdump_dump(zone, fd, zonefile, &crc_value) != KNOT_EOK) {
		close(fd);
		unlink(new_zonedb);
		free(new_zonedb);
		return KNOTD_ERROR;
	}
	
	/* Set compiled zone rights to 0640. */
	fchmod(fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	
	/* Close compiled zone. */
	close(fd);

	/* Delete old CRC file. */
	char *zonedb_crc = knot_zdump_crc_file(zonedb);
	if (zonedb_crc == NULL) {
		unlink(new_zonedb);
		free(new_zonedb);
		return KNOTD_ENOMEM;
	}
	remove(zonedb_crc);

	/* New CRC file. */
	char *new_zonedb_crc = knot_zdump_crc_file(new_zonedb);
	if (new_zonedb_crc == NULL) {
		dbg_zdump("Failed to create CRC file path from %s.\n",
		          new_zonedb);
		free(zonedb_crc);
		unlink(new_zonedb);
		free(new_zonedb);
		return KNOTD_ENOMEM;
	}

	/* Write CRC value to CRC file. */
	FILE *f_crc = fopen(new_zonedb_crc, "w");
	if (f_crc == NULL) {
		dbg_zdump("Cannot open CRC file %s!\n",
		          zonedb_crc);
		free(zonedb_crc);
		unlink(new_zonedb);
		free(new_zonedb);
		return KNOTD_ERROR;
	} else {
		fprintf(f_crc, "%lu\n",
		        (unsigned long)crc_value);
		fclose(f_crc);
	}
	
	/* Set CRC file rights to 0640. */
	chmod(new_zonedb_crc, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);

	/* Swap CRC files. */
	int ret = KNOTD_EOK;
	if (rename(new_zonedb_crc, zonedb_crc) < 0) {
		dbg_zdump("Failed to replace old zonedb CRC %s "
		          "with new CRC zone file %s.\n",
		          zonedb_crc,
		          new_zonedb_crc);
		unlink(new_zonedb);
		unlink(new_zonedb_crc);
		ret = KNOTD_ERROR;
	} else {
		/* Swap zone databases. */
		int swap_res = rename(new_zonedb, zonedb);
		if (swap_res < 0 && swap_res != EEXIST) {
			dbg_zdump("Failed to replace old zonedb %s "
			          "with new zone file %s.\n",
			          new_zonedb,
			          zonedb);
			ret = KNOTD_ERROR;
			unlink(new_zonedb);
		} else {

		}
	}

	free(new_zonedb_crc);
	free(zonedb_crc);
	free(new_zonedb);


	return ret;
}

/*----------------------------------------------------------------------------*/

int zones_save_zone(const knot_ns_xfr_t *xfr)
{
	if (xfr == NULL || xfr->new_contents == NULL || xfr->zone == NULL) {
		return KNOTD_EINVAL;
	}
	
	rcu_read_lock();
	
	zonedata_t *zd = (zonedata_t *)knot_zone_data(xfr->zone);
	knot_zone_contents_t *new_zone = xfr->new_contents;
	
	const char *zonefile = zd->conf->file;
	const char *zonedb = zd->conf->db;
	
	/* Check if the new zone apex dname matches zone name. */
	knot_dname_t *cur_name = knot_dname_new_from_str(zd->conf->name,
	                                                 strlen(zd->conf->name),
	                                                 NULL);
	const knot_dname_t *new_name = NULL;
	new_name = knot_node_owner(knot_zone_contents_apex(new_zone));
	int r = knot_dname_compare(cur_name, new_name);
	knot_dname_free(&cur_name);
	if (r != 0) {
		rcu_read_unlock();
		return KNOTD_EINVAL;
	}
	
	assert(zonefile != NULL && zonedb != NULL);
	
	/* dump the zone into text zone file */
	int ret = zones_dump_zone_text(new_zone, zonefile);
	if (ret != KNOTD_EOK) {
		rcu_read_unlock();
		return KNOTD_ERROR;
	}
	/* dump the zone into binary db file */
	ret = zones_dump_zone_binary(new_zone, zonedb, zonefile);
	if (ret != KNOTD_EOK) {
		rcu_read_unlock();
		return KNOTD_ERROR;
	}
	
	rcu_read_unlock();
	
	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

int zones_ns_conf_hook(const struct conf_t *conf, void *data)
{
	knot_nameserver_t *ns = (knot_nameserver_t *)data;
	dbg_zones_verb("zones: reconfiguring name server.\n");
	
	/* Set NSID. */
	knot_ns_set_nsid(ns, conf->nsid, conf->nsid_len);

	knot_zonedb_t *old_db = 0;

	int ret = zones_update_db_from_config(conf, ns, &old_db);
	if (ret != KNOTD_EOK) {
		return ret;
	}
	/* Wait until all readers finish with reading the zones. */
	synchronize_rcu();

	dbg_zones_verb("zones: nameserver's zone db: %p, old db: %p\n",
	               ns->zone_db, old_db);

	/* Delete all deprecated zones and delete the old database. */
	knot_zonedb_deep_free(&old_db);

	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/
/* Counting size of changeset in serialized form.                             */
/*----------------------------------------------------------------------------*/

static inline size_t zones_dname_binary_size(const knot_dname_t *dname)
{
	if (dname == NULL) {
		return 0;
	}

	size_t size = 10; // 4B ID, 4B size, 2B label count

	// dname size in wire format
	size += knot_dname_size(dname);
	// label array size
	size += knot_dname_label_count(dname);

	return size;
}

/*----------------------------------------------------------------------------*/

static size_t zones_rdata_binary_size(const knot_rdata_t *rdata,
                                      knot_rrtype_descriptor_t *desc)
{
	if (rdata == NULL) {
		return 0;
	}

	assert(desc != NULL);

	size_t size = sizeof(unsigned int); // RDATA item count

	for (int i = 0; i < rdata->count; ++i) {
		if (desc->wireformat[i] == KNOT_RDATA_WF_COMPRESSED_DNAME
		    || desc->wireformat[i] == KNOT_RDATA_WF_UNCOMPRESSED_DNAME
		    || desc->wireformat[i] == KNOT_RDATA_WF_LITERAL_DNAME) {
			size += zones_dname_binary_size(rdata->items[i].dname);
			size += 2; // flags
		} else {
			if (rdata->items[i].raw_data != NULL) {
				size += rdata->items[i].raw_data[0] + 2;
			}
		}
	}

	return size;
}

/*----------------------------------------------------------------------------*/

static size_t zones_rrset_binary_size(const knot_rrset_t *rrset)
{
	assert(rrset != NULL);

	size_t size = 0;

	size += 13; // 2B type, 2B class, 4B TTL, 4B RDATA count, 1B flags
	size += zones_dname_binary_size(rrset->owner);

	knot_rrtype_descriptor_t *desc = knot_rrtype_descriptor_by_type(
	                        knot_rrset_type(rrset));
	assert(desc != NULL);

	const knot_rdata_t *rdata = knot_rrset_rdata(rrset);
	while (rdata != NULL) {
		size += zones_rdata_binary_size(rdata, desc);
		rdata = knot_rrset_rdata_next(rrset, rdata);
	}

	return size;
}

/*----------------------------------------------------------------------------*/

int zones_changeset_binary_size(const knot_changeset_t *chgset, size_t *size)
{
	if (chgset == NULL || size == NULL) {
		return KNOTD_EINVAL;
	}

	size_t soa_from_size = zones_rrset_binary_size(chgset->soa_from);
	size_t soa_to_size = zones_rrset_binary_size(chgset->soa_to);

	size_t remove_size = 0;
	for (int i = 0; i < chgset->remove_count; ++i)
	{
		remove_size += zones_rrset_binary_size(chgset->remove[i]);
	}

	size_t add_size = 0;
	for (int i = 0; i < chgset->add_count; ++i)
	{
		add_size += zones_rrset_binary_size(chgset->add[i]);
	}

	/*! \todo How is the changeset serialized? Any other parts? */
	*size += soa_from_size + soa_to_size + remove_size + add_size;

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/
/* Changeset serialization and storing (new)                                  */
/*----------------------------------------------------------------------------*/

static int zones_rrset_write_to_mem(const knot_rrset_t *rr, char **entry,
                                    size_t *remaining) {
	size_t written = 0;
	int ret = knot_zdump_rrset_serialize(rr, *((uint8_t **)entry),
	                                     *remaining, &written);
	if (ret == KNOT_EOK) {
		assert(written <= *remaining);
		*remaining -= written;
		*entry += written;
	}
	
	return ret;
}

static int zones_serialize_and_store_chgset(const knot_changeset_t *chs,
                                            char *entry, size_t max_size)
{
	/* Serialize SOA 'from'. */
	int ret = zones_rrset_write_to_mem(chs->soa_from, &entry, &max_size);
	if (ret != KNOT_EOK) {
		dbg_zones("knot_zdump_rrset_serialize() returned %s\n",
		          knot_strerror(ret));
		return KNOTD_ERROR;  /*! \todo Other code? */
	}

	/* Serialize RRSets from the 'remove' section. */
	for (int i = 0; i < chs->remove_count; ++i) {
		ret = zones_rrset_write_to_mem(chs->remove[i], &entry, &max_size);
		if (ret != KNOT_EOK) {
			dbg_zones("knot_zdump_rrset_serialize() returned %s\n",
			          knot_strerror(ret));
			return KNOTD_ERROR;  /*! \todo Other code? */
		}
	}

	/* Serialize SOA 'to'. */
	ret = zones_rrset_write_to_mem(chs->soa_to, &entry, &max_size);
	if (ret != KNOT_EOK) {
		dbg_zones("knot_zdump_rrset_serialize() returned %s\n",
		          knot_strerror(ret));
		return KNOTD_ERROR;  /*! \todo Other code? */
	}

	/* Serialize RRSets from the 'add' section. */
	for (int i = 0; i < chs->add_count; ++i) {
		ret = zones_rrset_write_to_mem(chs->add[i], &entry, &max_size);
		if (ret != KNOT_EOK) {
			dbg_zones("knot_zdump_rrset_serialize() returned %s\n",
			          knot_strerror(ret));
			return KNOTD_ERROR;  /*! \todo Other code? */
		}

	}


	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

static int zones_store_changeset(const knot_changeset_t *chs, journal_t *j,
                                 knot_zone_t *zone, zonedata_t *zd)
{
	assert(chs != NULL);
	assert(j != NULL);

	dbg_xfr("Saving changeset from %u to %u.\n",
	        chs->serial_from, chs->serial_to);

	uint64_t k = ixfrdb_key_make(chs->serial_from, chs->serial_to);

	/* Count the size of the entire changeset in serialized form. */
	size_t entry_size = 0;

	int ret = zones_changeset_binary_size(chs, &entry_size);
	assert(ret == KNOTD_EOK);

	dbg_xfr_verb("Size in serialized form: %zu\n", entry_size);

	/* Reserve space for the journal entry. */
	char *journal_entry = NULL;
	ret = journal_map(j, k, &journal_entry, entry_size);

	/* Sync to zonefile may be needed. */
	while (ret == KNOTD_EAGAIN) {
		/* Cancel sync timer. */
		event_t *tmr = zd->ixfr_dbsync;
		if (tmr) {
			dbg_xfr_verb("xfr: cancelling zonefile "
			             "SYNC timer of '%s'\n",
			             zd->conf->name);
			evsched_cancel(tmr->parent, tmr);
		}

		/* Synchronize. */
		dbg_xfr_verb("xfr: forcing zonefile SYNC "
		             "of '%s'\n",
		             zd->conf->name);
		ret = zones_zonefile_sync(zone, j);
		if (ret != KNOTD_EOK && ret != KNOTD_ERANGE) {
			continue;
		}

		/* Reschedule sync timer. */
		if (tmr) {
			/* Fetch sync timeout. */
			rcu_read_lock();
			int timeout = zd->conf->dbsync_timeout;
			timeout *= 1000; /* Convert to ms. */
			rcu_read_unlock();

			/* Reschedule. */
			dbg_xfr_verb("xfr: resuming SYNC "
			             "of '%s'\n",
			             zd->conf->name);
			evsched_schedule(tmr->parent, tmr,
			                 timeout);

		}

		/* Attempt to map again. */
		ret = journal_map(j, k, &journal_entry, entry_size);
	}

	if (ret != KNOTD_EOK) {
		dbg_xfr("Failed to map space for journal entry: %s.\n",
		        knotd_strerror(ret));
		return ret;
	}

	assert(journal_entry != NULL);

	/* Serialize changeset, saving it bit by bit. */
	ret = zones_serialize_and_store_chgset(chs, journal_entry, entry_size);

	if (ret != KNOTD_EOK) {
		dbg_xfr("Failed to serialize and store changeset: %s\n",
		        knotd_strerror(ret));
	}

	/* Unmap the journal entry.
	   If successfuly written changeset to journal, validate the entry. */
	ret = journal_unmap(j, k, journal_entry, ret == KNOTD_EOK);

	return ret;
}

/*----------------------------------------------------------------------------*/

journal_t *zones_store_changesets_begin(knot_zone_t *zone)
{
	if (zone == NULL) {
		return NULL;
	}

	/* Fetch zone-specific data. */
	//knot_zone_t *zone = xfr->zone;
	zonedata_t *zd = (zonedata_t *)zone->data;
	if (!zd->ixfr_db) {
		return NULL;
	}

	/* Begin transaction, will be release on commit/rollback. */
	journal_t *j = journal_retain(zd->ixfr_db);
	if (journal_trans_begin(j) != KNOTD_EOK) {
		journal_release(j);
		j = NULL;
	}
	
	return j;
}

/*----------------------------------------------------------------------------*/

int zones_store_changesets_commit(journal_t *j)
{
	if (j == NULL) {
		return KNOTD_EINVAL;
	}
	
	int ret = journal_trans_commit(j);
	journal_release(j);
	return ret;
}

/*----------------------------------------------------------------------------*/

int zones_store_changesets_rollback(journal_t *j)
{
	if (j == NULL) {
		return KNOTD_EINVAL;
	}
	
	int ret = journal_trans_rollback(j);
	journal_release(j);
	return ret;
}

/*----------------------------------------------------------------------------*/

int zones_store_changesets(knot_zone_t *zone, knot_changesets_t *src)
{
	if (zone == NULL || src == NULL) {
		return KNOTD_EINVAL;
	}

//	knot_zone_t *zone = xfr->zone;
//	knot_changesets_t *src = (knot_changesets_t *)xfr->data;

	/* Fetch zone-specific data. */
	zonedata_t *zd = (zonedata_t *)zone->data;
	if (!zd->ixfr_db) {
		return KNOTD_EINVAL;
	}

	/* Retain journal for changeset writing. */
	journal_t *j = journal_retain(zd->ixfr_db);
	if (j == NULL) {
		return KNOTD_EBUSY;
	}
	int ret = 0;

	/* Begin writing to journal. */
	for (unsigned i = 0; i < src->count; ++i) {
		/* Make key from serials. */
		knot_changeset_t* chs = src->sets + i;

		ret = zones_store_changeset(chs, j, zone, zd);
		if (ret != KNOTD_EOK) {
			journal_release(j);
			return ret;
		}
	}

	/* Release journal. */
	journal_release(j);

	/* Written changesets to journal. */
	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

int zones_xfr_load_changesets(knot_ns_xfr_t *xfr, uint32_t serial_from,
                              uint32_t serial_to) 
{
	if (!xfr || !xfr->zone || !knot_zone_contents(xfr->zone)) {
		dbg_zones_detail("Wrong parameters: xfr=%p,"
		                " xfr->zone = %p\n", xfr, xfr->zone);
		return KNOTD_EINVAL;
	}
	
	knot_changesets_t *chgsets = (knot_changesets_t *)
	                               calloc(1, sizeof(knot_changesets_t));
	CHECK_ALLOC_LOG(chgsets, KNOTD_ENOMEM);
	
	int ret = ns_serial_compare(serial_to, serial_from);
	dbg_zones_verb("Compared serials, result: %d\n", ret);
	
	/* if serial_to is not larger than serial_from, do not load anything */
	if (ret <= 0) {
		xfr->data = chgsets;
		return KNOTD_EOK;
	}
	
	dbg_xfr_verb("xfr: loading changesets\n");
	ret = zones_load_changesets(xfr->zone, chgsets,
	                                serial_from, serial_to);
	if (ret != KNOTD_EOK) {
		dbg_xfr("xfr: failed to load changesets: %s\n",
		        knotd_strerror(ret));
		knot_free_changesets(&chgsets);
		return ret;
	}
	
	xfr->data = chgsets;
	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

int zones_create_and_save_changesets(const knot_zone_t *old_zone,
                                     const knot_zone_t *new_zone)
{
	if (old_zone == NULL || old_zone->contents == NULL
	    || new_zone == NULL || new_zone->contents == NULL) {
		dbg_zones("zones: create_changesets: "
		          "NULL arguments.\n");
		return KNOTD_EINVAL;
	}
	
	knot_ns_xfr_t xfr;
	memset(&xfr, 0, sizeof(xfr));
	xfr.zone = (knot_zone_t *)old_zone;
	knot_changesets_t *changesets;
	int ret = knot_zone_diff_create_changesets(old_zone->contents,
	                                           new_zone->contents,
	                                           &changesets);
	if (ret != KNOT_EOK) {
		if (ret == KNOT_ERANGE) {
			dbg_zones_detail("zones: create_changesets: "
			                 "New serial was lower than the old "
			                 "one.\n");
			knot_free_changesets(&changesets);
			return KNOTD_ERANGE;
		} else if (ret == KNOT_ENODIFF) {
			dbg_zones_detail("zones: create_changesets: "
			                 "New serial was the same as the old "
			                 "one.\n");
			knot_free_changesets(&changesets);
			return KNOTD_ENODIFF;
		} else {
			dbg_zones("zones: create_changesets: "
			          "Could not create changesets. Reason: %s\n",
			          knot_strerror(ret));
			knot_free_changesets(&changesets);
			return KNOTD_ERROR;
		}
	}
	
	xfr.data = changesets;
	journal_t *journal = zones_store_changesets_begin(xfr.zone);
	if (journal == NULL) {
		dbg_zones("zones: create_changesets: "
		          "Could not start journal operation.\n");
		return KNOTD_ERROR;
	}
	
	ret = zones_store_changesets(xfr.zone, (knot_changesets_t *)xfr.data);
	if (ret != KNOTD_EOK) {
		zones_store_changesets_rollback(journal);
		dbg_zones("zones: create_changesets: "
		          "Could not store in the journal. Reason: %s.\n",
		          knotd_strerror(ret));
		
		return ret;
	}
	
	ret = zones_store_changesets_commit(journal);
	if (ret != KNOTD_EOK) {
		dbg_zones("zones: create_changesets: "
		          "Could not commit to journal. Reason: %s.\n",
		          knotd_strerror(ret));
		
		return ret;
	}
	
	knot_free_changesets(&changesets);
	
	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

int zones_store_and_apply_chgsets(knot_changesets_t *chs,
                                  knot_zone_t *zone,
                                  knot_zone_contents_t **new_contents,
                                  const char *msgpref, int type)
{
	int ret = KNOT_EOK;
	int apply_ret = KNOT_EOK;
	int switch_ret = KNOT_EOK;

	/* Serialize and store changesets. */
	dbg_xfr("xfr: IXFR/IN serializing and saving changesets\n");
	journal_t *transaction = zones_store_changesets_begin(zone);
	if (transaction != NULL) {
		ret = zones_store_changesets(zone, chs);
	} else {
		ret = KNOTD_ERROR;
	}
	if (ret != KNOTD_EOK) {
		log_zone_error("%s Failed to serialize and store "
		               "changesets - %s\n", msgpref,
		               knotd_strerror(ret));
		/* Free changesets, but not the data. */
		knot_free_changesets(&chs);
//		data->data = NULL;
		return ret;
	}

	/* Now, try to apply the changesets to the zone. */
	apply_ret = xfrin_apply_changesets(zone, chs, new_contents);

	if (apply_ret != KNOT_EOK) {
		zones_store_changesets_rollback(transaction);
		log_zone_error("%s Failed to apply changesets - %s\n",
		               msgpref, knot_strerror(apply_ret));

		/* Free changesets, but not the data. */
		knot_free_changesets(&chs);
//		data->data = NULL;
		return KNOTD_ERROR;
	}

	/* Commit transaction. */
	ret = zones_store_changesets_commit(transaction);
	if (ret != KNOTD_EOK) {
		log_zone_error("%s Failed to commit stored changesets "
		               "- %s\n", msgpref, knot_strerror(apply_ret));
		knot_free_changesets(&chs);
//		data->data = NULL;
		return ret;
	}

	/* Switch zone contents. */
	switch_ret = xfrin_switch_zone(zone, *new_contents, type);

	if (switch_ret != KNOT_EOK) {
		log_zone_error("%s Failed to replace current zone - %s\n",
		               msgpref, knot_strerror(switch_ret));
		// Cleanup old and new contents
		xfrin_rollback_update(zone->contents, new_contents,
		                      &chs->changes);

		/* Free changesets, but not the data. */
		knot_free_changesets(&chs);
//		data->data = NULL;
		return KNOTD_ERROR;
	}

	xfrin_cleanup_successful_update(&chs->changes);

	/* Free changesets, but not the data. */
	knot_free_changesets(&chs);
//	data->data = NULL;
	assert(ret == KNOTD_EOK);
	log_zone_info("%s Finished.\n", msgpref);
	return KNOTD_EOK;
}

/*----------------------------------------------------------------------------*/

int zones_timers_update(knot_zone_t *zone, conf_zone_t *cfzone, evsched_t *sch)
{
	if (!sch || !zone) {
		return KNOTD_EINVAL;
	}

	/* Fetch zone data. */
	zonedata_t *zd = (zonedata_t *)zone->data;
	if (!zd) {
		return KNOTD_EINVAL;
	}

	/* Cancel REFRESH timer. */
	if (zd->xfr_in.timer) {
		evsched_cancel(sch, zd->xfr_in.timer);
		evsched_event_free(sch, zd->xfr_in.timer);
		zd->xfr_in.timer = 0;
	}

	/* Cancel EXPIRE timer. */
	if (zd->xfr_in.expire) {
		evsched_cancel(sch, zd->xfr_in.expire);
		evsched_event_free(sch, zd->xfr_in.expire);
		zd->xfr_in.expire = 0;
	}

	/* Remove list of pending NOTIFYs. */
	pthread_mutex_lock(&zd->lock);
	notify_ev_t *ev = 0, *evn = 0;
	WALK_LIST_DELSAFE(ev, evn, zd->notify_pending) {
		zones_cancel_notify(zd, ev);
	}
	pthread_mutex_unlock(&zd->lock);

	/* Check XFR/IN master server. */
	rcu_read_lock();
	if (zd->xfr_in.master.ptr) {

		/* Schedule REFRESH timer. */
		uint32_t refresh_tmr = 0;
		if (knot_zone_contents(zone)) {
			refresh_tmr = zones_jitter(zones_soa_refresh(zone));
		} else {
			refresh_tmr = zd->xfr_in.bootstrap_retry;
		}
		zd->xfr_in.timer = evsched_schedule_cb(sch, zones_refresh_ev,
							 zone, refresh_tmr);
		dbg_zones("zone: REFRESH '%s' set to %u\n",
		          cfzone->name, refresh_tmr);
	}

	/* Do not issue NOTIFY queries if stub. */
	if (!knot_zone_contents(zone)) {
		rcu_read_unlock();
		return KNOTD_EOK;
	}

	/* Schedule NOTIFY to slaves. */
	conf_remote_t *r = 0;
	WALK_LIST(r, cfzone->acl.notify_out) {

		/* Fetch remote. */
		conf_iface_t *cfg_if = r->remote;

		/* Create request. */
		notify_ev_t *ev = malloc(sizeof(notify_ev_t));
		if (!ev) {
			free(ev);
			dbg_zones("notify: out of memory to create "
				    "NOTIFY query for %s\n", cfg_if->name);
			continue;
		}

		/* Parse server address. */
		sockaddr_init(&ev->saddr, -1);
		int ret = sockaddr_set(&ev->addr, cfg_if->family,
				       cfg_if->address,
				       cfg_if->port);
		sockaddr_t *via = &cfg_if->via;
		if (ret > 0) {
			if (sockaddr_isvalid(via)) {
				sockaddr_copy(&ev->saddr, via);
			}
		} else {
			free(ev);
			log_server_warning("NOTIFY slave '%s' has invalid "
			                   "address '%s@%d', couldn't create"
			                   "query.\n", cfg_if->name,
			                   cfg_if->address, cfg_if->port);
			continue;
		}

		/* Prepare request. */
		ev->retries = cfzone->notify_retries + 1; /* first + N retries*/
		ev->msgid = 0;
		ev->zone = zone;
		ev->timeout = cfzone->notify_timeout;

		/* Schedule request (30 - 60s random delay). */
		int tmr_s = 30 + (int)(30.0 * tls_rand());
		pthread_mutex_lock(&zd->lock);
		ev->timer = evsched_schedule_cb(sch, zones_notify_send, ev,
						tmr_s * 1000);
		add_tail(&zd->notify_pending, &ev->n);
		pthread_mutex_unlock(&zd->lock);

		log_server_info("Scheduled '%s' NOTIFY query "
				"after %d s to '%s@%d'.\n", zd->conf->name,
			    tmr_s, cfg_if->address, cfg_if->port);
	}

	rcu_read_unlock();

	return KNOTD_EOK;
}

int zones_cancel_notify(zonedata_t *zd, notify_ev_t *ev)
{
	if (!zd || !ev || !ev->timer) {
		return KNOTD_EINVAL;
	}

	/* Wait for event to finish running. */
#ifdef KNOTD_NOTIFY_DEBUG
	int pkt_id = ev->msgid; /*< Do not optimize! */
#endif
	event_t *tmr = ev->timer;
	ev->timer = 0;
	pthread_mutex_unlock(&zd->lock);
	if (evsched_cancel(tmr->parent, tmr) == 0) {
		dbg_notify("notify: NOTIFY event %p designated for cancellation "
			   "not found\n", tmr);
	}

	/* Re-lock and find again (if not deleted). */
	pthread_mutex_lock(&zd->lock);
	int match_exists = 0;
	notify_ev_t *tmpev = 0;
	WALK_LIST(tmpev, zd->notify_pending) {
		if (tmpev == ev) {
			match_exists = 1;
			break;
		}
	}

	/* Event deleted before cancelled. */
	if (!match_exists) {
		dbg_notify("notify: NOTIFY event for query ID=%u was "
		           "deleted before cancellation.\n",
		           pkt_id);
		return KNOTD_EOK;

	}

	/* Free event (won't be scheduled again). */
	dbg_notify("notify: NOTIFY query ID=%u event cancelled.\n",
	           pkt_id);
	rem_node(&ev->n);
	evsched_event_free(tmr->parent, tmr);
	free(ev);
	return KNOTD_EOK;
}
