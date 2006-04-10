/*
 * relay.cpp - Teredo relay core
 * $Id$
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright © 2004-2006 Rémi Denis-Courmont.                         *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license.         *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <gettext.h>

#include <string.h>
#include <time.h>
#include <stdlib.h> // malloc()
#include <assert.h>

#if HAVE_STDINT_H
# include <stdint.h>
#elif HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip6.h> // struct ip6_hdr
#include <netinet/icmp6.h> // ICMP6_DST_UNREACH_*
#include <syslog.h>
#include <pthread.h>

#include "teredo.h"
#include "v4global.h" // is_ipv4_global_unicast()
#include "teredo-udp.h"

#include "packets.h"
#include "tunnel.h"
#include "maintain.h"
#include "peerlist.h"
#ifdef MIREDO_TEREDO_CLIENT
# include "security.h"
#endif

class TeredoRelay;

struct libteredo_tunnel
{
	TeredoRelay *object;
	struct teredo_peerlist *list;
	void *opaque;

	libteredo_recv_cb recv_cb;
	libteredo_icmpv6_cb icmpv6_cb;
#ifdef MIREDO_TEREDO_CLIENT
	libteredo_state_up_cb up_cb;
	libteredo_state_down_cb down_cb;
#endif

	teredo_state state;
	pthread_rwlock_t state_lock;

	int fd;
	bool allow_cone;
};


// big TODO: make all functions re-entrant safe
//           make all functions thread-safe
class TeredoRelay
{
	private:
		libteredo_tunnel *tunnel;

		void SendUnreach (int code, const void *in, size_t inlen);

#ifdef MIREDO_TEREDO_CLIENT
		struct teredo_maintenance *maintenance;

		void NotifyUp (const struct in6_addr *, uint16_t);
		void NotifyDown (void);

		static void StateChange (const teredo_state *, void *self);
#endif
		void EmitICMPv6Error (const void *packet, size_t length,
		                      const struct in6_addr *dst);

		int SendIPv6Packet (const void *packet, size_t length);
		static void _sendIPv6Packet (void *self,
		                             const void *packet, size_t length)
		{
			(void)((TeredoRelay *)self)->SendIPv6Packet (packet, length);
		}

	public:
		TeredoRelay (libteredo_tunnel *t);
		TeredoRelay (libteredo_tunnel *t, const char *server,
		             const char *server2);

		~TeredoRelay (void);
		int SendPacket (const struct ip6_hdr *packet, size_t len);
		int ReceivePacket (void);

#ifdef MIREDO_TEREDO_CLIENT
		bool IsClient (void) const
		{
			return maintenance != NULL;
		}

		/*static unsigned QualificationRetries;
		static unsigned QualificationTimeOut;
		static unsigned ServerNonceLifetime;
		static unsigned RestartDelay;*/
#endif
		//static unsigned MaxQueueBytes;
		//static unsigned MaxPeers;
		static unsigned IcmpRateLimitMs;
};

#ifdef HAVE_LIBJUDY
# define MAX_PEERS 1048576
#else
# define MAX_PEERS 1024
#endif


TeredoRelay::TeredoRelay (libteredo_tunnel *t)
	:  tunnel (t)
{
#ifdef MIREDO_TEREDO_CLIENT
	maintenance = NULL;
#endif
}


#ifdef MIREDO_TEREDO_CLIENT
TeredoRelay::TeredoRelay (libteredo_tunnel *t, const char *server,
                          const char *server2)
	: tunnel (t), maintenance (NULL)
{
	maintenance = libteredo_maintenance_start (t->fd, StateChange, this,
	                                           server, server2);
	if (maintenance != NULL)
		return; /* success */

	/* failure */
	throw new (void *)(NULL);
}
#endif /* ifdef MIREDO_TEREDO_CLIENT */

/* Releases peers list entries */
TeredoRelay::~TeredoRelay (void)
{
#ifdef MIREDO_TEREDO_CLIENT
	if (maintenance != NULL)
		libteredo_maintenance_stop (maintenance);
#endif
}


/*
 * Sends an ICMPv6 Destination Unreachable error to the IPv6 Internet.
 * Unfortunately, this will use a local-scope address as source, which is not
 * quite good.
 */
unsigned TeredoRelay::IcmpRateLimitMs = 100;

void
TeredoRelay::SendUnreach (int code, const void *in, size_t len)
{
	/* FIXME: should probably not be static */
	static struct
	{
		pthread_mutex_t lock;
		int count;
		time_t last;
	} ratelimit = { PTHREAD_MUTEX_INITIALIZER, 1, 0 };
	struct
	{
		struct icmp6_hdr hdr;
		char fill[1280 - sizeof (struct ip6_hdr) - sizeof (struct icmp6_hdr)];
	} buf;
	time_t now;

	/* ICMPv6 rate limit */
	time (&now);
	pthread_mutex_lock (&ratelimit.lock);
	if (memcmp (&now, &ratelimit.last, sizeof (now)))
	{
		memcpy (&ratelimit.last, &now, sizeof (now));
		ratelimit.count =
			IcmpRateLimitMs ? (int)(1000 / IcmpRateLimitMs) : -1;
	}

	if (ratelimit.count == 0)
	{
		/* rate limit exceeded */
		pthread_mutex_unlock (&ratelimit.lock);
		return;
	}
	if (ratelimit.count > 0)
		ratelimit.count--;
	pthread_mutex_unlock (&ratelimit.lock);

	len = BuildICMPv6Error (&buf.hdr, ICMP6_DST_UNREACH, code, in, len);
	(void)EmitICMPv6Error (&buf.hdr, len,
	                       &((const struct ip6_hdr *)in)->ip6_src);
}

#if 0
void
TeredoRelay::EmitICMPv6Error (const void *packet, size_t length,
							  const struct in6_addr *dst)
{
	/* TODO should be implemented with BuildIPv6Error() */
	/* that is currently dead code */

	/* F-I-X-M-E: using state implies locking */
	size_t outlen = BuildIPv6Error (&buf.hdr, &state.addr.ip6,
	                                ICMP6_DST_UNREACH, code, in, inlen);
	(void)SendIPv6Packet (&buf, outlen);
}
#endif


#ifdef MIREDO_TEREDO_CLIENT
void TeredoRelay::StateChange (const teredo_state *state, void *self)
{
	TeredoRelay *r = (TeredoRelay *)self;
	bool previously_up;

	pthread_rwlock_wrlock (&r->tunnel->state_lock);
	previously_up = r->tunnel->state.up;
	memcpy (&r->tunnel->state, state, sizeof (r->tunnel->state));

	if (r->tunnel->state.up)
	{
		/*
		 * NOTE: we get an hold on both state and peer list locks here.
		 * As such, in any case, attempting to acquire the state lock while
		 * the peer list is locked is STRICTLY FORBIDDEN to avoid an obvious
		 * inter-locking deadlock.
		 */
		teredo_list_reset (r->tunnel->list, MAX_PEERS);
		r->NotifyUp (&r->tunnel->state.addr.ip6, r->tunnel->state.mtu);
	}
	else
	if (previously_up)
		r->NotifyDown ();

	/*
	 * NOTE: the lock is retained until here to ensure notifications remain
	 * properly ordered. Unfortunately, we cannot be re-entrant from within
	 * NotifyUp/Down.
	 */
	pthread_rwlock_unlock (&r->tunnel->state_lock);
}

/**
 * @return 0 if a ping may be sent. 1 if one was sent recently
 * -1 if the peer seems unreachable.
 */
static int CountPing (teredo_peer *peer, time_t now)
{
	int res;

	if (peer->pings == 0)
		res = 0;
	// don't test more than 4 times (once + 3 repeats)
	else if (peer->pings >= 4)
		res = -1;
	// test must be separated by at least 2 seconds
	else
	if (((now - peer->last_ping) & 0x1ff) <= 2)
		res = 1;
	else
		res = 0; // can test again!

	if (res == 0)
	{
		peer->last_ping = now;
		peer->pings++;
	}

	return res;
}


static int PingPeer (int fd, teredo_peer *p, time_t now,
                     const union teredo_addr *src, const struct in6_addr *dst)
{
	int res = CountPing (p, now);

	if (res == 0)
		return SendPing (fd, src, dst);
	return res;
}
#endif


/*
 * Returs true if the packet whose header is passed as a parameter looks
 * like a Teredo bubble.
 */
inline bool IsBubble (const struct ip6_hdr *hdr)
{
	return (hdr->ip6_plen == 0) && (hdr->ip6_nxt == IPPROTO_NONE);
}


/*
 * Returns 0 if a bubble may be sent, -1 if no more bubble may be sent,
 * 1 if a bubble may be sent later.
 */
static int CountBubble (teredo_peer *peer, time_t now)
{
	/* § 5.2.6 - sending bubbles */
	int res;

	if (peer->bubbles > 0)
	{
		if (peer->bubbles >= 4)
		{
			// don't send if 4 bubbles already sent within 300 seconds
			if ((now - peer->last_tx) <= 300)
				res = -1;
			else
			{
				// reset counter every 300 seconds
				peer->bubbles = 0;
				res = 0;
			}
		}
		else
		// don't send if last tx was 2 seconds ago or fewer
		if ((now - peer->last_tx) <= 2)
			res = 1;
		else
			res = 0;
	}
	else
		res = 0;

	if (res == 0)
	{
		peer->last_tx = now;
		peer->bubbles++;
	}

	return res;
}


static inline void SetMappingFromPacket (teredo_peer *peer,
										 const struct teredo_packet *p)
{
	SetMapping (peer, p->source_ipv4, p->source_port);
}


/*
 * Handles a packet coming from the IPv6 Internet, toward a Teredo node
 * (as specified per paragraph 5.4.1). That's what the specification calls
 * "Packet transmission".
 *
 * It is assumed that the packet is valid (if not, it will be dropped by
 * the receiving Teredo peer). It is furthermore assumed that the packet
 * is at least 40 bytes long (room for the IPv6 header and that it is
 * properly aligned.
 *
 * The packet size should not exceed the MTU (1280 bytes by default).
 * In any case, sending will fail if the packets size exceeds 65507 bytes
 * (maximum size for a UDP packet's payload).
 *
 * Returns 0 on success, -1 on error.
 */
int TeredoRelay::SendPacket (const struct ip6_hdr *packet, size_t length)
{
	const union teredo_addr *dst = (union teredo_addr *)&packet->ip6_dst,
				*src = (union teredo_addr *)&packet->ip6_src;
	teredo_state s;

	/* Drops multicast destination, we cannot handle these */
	if ((dst->ip6.s6_addr[0] == 0xff)
	/* Drops multicast source, these are invalid */
	 || (src->ip6.s6_addr[0] == 0xff))
		return 0;

	pthread_rwlock_rdlock (&tunnel->state_lock);
	memcpy (&s, &tunnel->state, sizeof (s));
	/*
	 * We can afford to use a slightly outdated state, but we cannot afford to
	 * use an inconsistent state, hence this lock.
	*/
	pthread_rwlock_unlock (&tunnel->state_lock);

#ifdef MIREDO_TEREDO_CLIENT
	if (IsClient ())
	{
		if (!s.up) /* not qualified */
		{
			SendUnreach (ICMP6_DST_UNREACH_ADDR, packet, length);
			return 0;
		}
	
		if ((dst->teredo.prefix != s.addr.teredo.prefix)
		 && (src->teredo.prefix != s.addr.teredo.prefix))
		{
			/*
			 * Routing packets not from a Teredo client,
			 * neither toward a Teredo client is NOT allowed through a
			 * Teredo tunnel. The Teredo server will reject the packet.
			 *
			 * We also drop link-local unicast and multicast packets as
			 * they can't be routed through Teredo properly.
			 */
			SendUnreach (ICMP6_DST_UNREACH_ADMIN, packet, length);
			return 0;
		}
	}
	else
#endif
	{
		if (dst->teredo.prefix != s.addr.teredo.prefix)
		{
			/*
			 * If we are not a qualified client, ie. we have no server
			 * IPv4 address to contact for direct IPv6 connectivity, we
			 * cannot route packets toward non-Teredo IPv6 addresses, and
			 * we are not allowed to do it by the specification either.
			 *
			 * NOTE:
			 * The specification mandates silently ignoring such
			 * packets. However, this only happens in case of
			 * misconfiguration, so I believe it could be better to
			 * notify the user. An alternative is to send an ICMPv6 error
			 * back to the kernel.
			 */
			SendUnreach (ICMP6_DST_UNREACH_ADDR, packet, length);
			return 0;
		}
	}

	if (dst->teredo.prefix == s.addr.teredo.prefix)
	{
		/*
		 * Ignores Teredo clients with incorrect server IPv4.
		 * This check is only specified for client case 4 & 5.
		 * That said, it can never fail in the other client cases (either
		 * because the peer is already known which means it already passed
		 * this check, or because the peer is not a Teredo client.
		 * As for the relay, I consider the check should also be done, even if
		 * it wasn't specified (TBD: double check the spec).
		 * Doing the check earlier, while it has an additionnal cost, makes
		 * sure that the peer will be added to the list if it is not already
		 * in it, which avoids a double peer list lookup (failed lookup, then
		 * insertion), which is a big time saver under heavy load.
		 */
		uint32_t peer_server = IN6_TEREDO_SERVER (dst);
		if (!is_ipv4_global_unicast (peer_server) || (peer_server == 0))
			return 0;
	}

	bool created;
	time_t now = time (NULL);
	struct teredo_peerlist *list = tunnel->list;

//	syslog (LOG_DEBUG, "packet to be sent");
	teredo_peer *p = teredo_list_lookup (list, now, &dst->ip6, &created);
	if (p == NULL)
		return -1; /* error */

	if (!created)
	{
//		syslog (LOG_DEBUG, " peer is %strusted", p->trusted ? "" : "NOT ");
//		syslog (LOG_DEBUG, " peer is %svalid", IsValid (p, now) ? "" : "NOT ");
//		syslog (LOG_DEBUG, " pings = %u, bubbles = %u", p->pings, p->bubbles);

		/* Case 1 (paragraphs 5.2.4 & 5.4.1): trusted peer */
		if (p->trusted && IsValid (p, now))
		{
			int res;

			/* Already known -valid- peer */
			TouchTransmit (p, now);
			res = teredo_send (tunnel->fd, packet, length, p->mapped_addr,
			                   p->mapped_port) == (int)length ? 0 : -1;
			teredo_list_release (list);
			return res;
		}
	}
 	else
	{
 		p->trusted = p->bubbles = p->pings = 0;
//		syslog (LOG_DEBUG, " peer unknown and created");
	}

	// Unknown, untrusted, or too old peer
	// (thereafter refered to as simply "untrusted")

#ifdef MIREDO_TEREDO_CLIENT
	/* Untrusted non-Teredo node */
	if (dst->teredo.prefix != s.addr.teredo.prefix)
	{
		int res;

		assert (IsClient ());

		/* Client case 2: direct IPv6 connectivity test */
		// TODO: avoid code duplication
		if (created)
		{
			p->mapped_port = 0;
			p->mapped_addr = 0;
		}

		QueueOutgoing (p, packet, length);
		res = PingPeer (tunnel->fd, p, now, &s.addr, &dst->ip6);

 		teredo_list_release (list);
		if (res == -1)
			SendUnreach (ICMP6_DST_UNREACH_ADDR, packet, length);

// 		syslog (LOG_DEBUG, " ping peer returned %d", res);
		return 0;
	}
#endif

	// Untrusted Teredo client

	/* Client case 3: TODO: implement local discovery */

	if (created)
		/* Unknown Teredo clients */
		SetMapping (p, IN6_TEREDO_IPV4 (dst), IN6_TEREDO_PORT (dst));

	/* Client case 4 & relay case 2: new cone peer */
	if (tunnel->allow_cone && IN6_IS_TEREDO_ADDR_CONE (dst))
	{
		int res;

		p->trusted = 1;
		p->bubbles = /*p->pings -USELESS- =*/ 0;
		TouchTransmit (p, now);
		res = teredo_send (tunnel->fd, packet, length, p->mapped_addr,
		                   p->mapped_port) == (int)length ? 0 : -1;
		teredo_list_release (list);
		return res;
	}

	/* Client case 5 & relay case 3: untrusted non-cone peer */
	QueueOutgoing (p, packet, length);

	// Sends bubble, if rate limit allows
	int res = CountBubble (p, now);
	teredo_list_release (list);
	switch (res)
	{
		case 0:
			/*
			 * Open the return path if we are behind a
			 * restricted NAT.
			 */
			if ((!s.cone)
			 && SendBubbleFromDst (tunnel->fd, &dst->ip6, false, false))
				return -1;

			return SendBubbleFromDst (tunnel->fd, &dst->ip6, s.cone, true);

		case -1: // Too many bubbles already sent
			SendUnreach (ICMP6_DST_UNREACH_ADDR, packet, length);

		//case 1: -- between two bubbles -- nothing to do
	}

	return 0;
}


/*
 * Handles a packet coming from the Teredo tunnel
 * (as specified per paragraph 5.4.2). That's called "Packet reception".
 * Returns 0 on success, -1 on error.
 */
int TeredoRelay::ReceivePacket (void)
{
	struct teredo_packet packet;
	teredo_state s;

	if (teredo_recv (tunnel->fd, &packet))
		return -1;

	const uint8_t *buf = packet.ip6;
	size_t length = packet.ip6_len;
	struct ip6_hdr ip6;

	// Checks packet
	if ((length < sizeof (ip6)) || (length > 65507))
		return 0; // invalid packet

	memcpy (&ip6, buf, sizeof (ip6));
	if (((ip6.ip6_vfc >> 4) != 6)
	 || ((ntohs (ip6.ip6_plen) + sizeof (ip6)) != length))
		return 0; // malformatted IPv6 packet

	pthread_rwlock_rdlock (&tunnel->state_lock);
	memcpy (&s, &tunnel->state, sizeof (s));
	/*
	 * We can afford to use a slightly outdated state, but we cannot afford to
	 * use an inconsistent state, hence this lock. Also, we cannot call
	 * libteredo_maintenance_process() while holding the lock, as that would
	 * cause a deadlock at StateChange().
	 */
	pthread_rwlock_unlock (&tunnel->state_lock);

#ifdef MIREDO_TEREDO_CLIENT
	/* Maintenance */
	if (IsClient () && (packet.source_port == htons (IPPORT_TEREDO)))
	{
		if (packet.auth_nonce != NULL)
		{
			libteredo_maintenance_process (maintenance, &packet);
			return 0;
		}
		else
		if (!s.up)
		{
			/* Not qualified -> do not accept incoming packets */
			return 0;
		}
		else
		if (packet.source_ipv4 != s.addr.teredo.server_ip)
		{
			/* Not from primary server IPv4 address
			   -> force normal packet reception */
// 			syslog (LOG_DEBUG, "packet from relay");
		}
		else
		if (packet.orig_ipv4)
		{
// 			syslog (LOG_DEBUG, "bubble from server (+reply)");
			/* TODO: record sending of bubble, create a peer, etc ? */
			SendBubble (tunnel->fd, packet.orig_ipv4, packet.orig_port,
			            &ip6.ip6_dst, &ip6.ip6_src);
			if (IsBubble (&ip6))
				return 0; // don't pass bubble to kernel
		}
		else
		if (IsBubble (&ip6))
		{
			/*
			 * Some servers do not insert an origin indication.
			 * When the source IPv6 address is a Teredo address,
			 * we can guess the mapping. Otherwise, we're stuck.
			 */
			if (IN6_TEREDO_PREFIX (&ip6.ip6_src) == s.addr.teredo.prefix)
				/* TODO: record sending of bubble, create a peer, etc ? */
				SendBubble (tunnel->fd, IN6_TEREDO_IPV4 (&ip6.ip6_src),
				            IN6_TEREDO_PORT (&ip6.ip6_src), &ip6.ip6_dst,
				            &ip6.ip6_src);
			else
			{
				syslog (LOG_WARNING, _("Ignoring invalid bubble: "
				        "your Teredo server is probably buggy."));
			}
			return 0; // don't pass bubble to kernel
		}
		/*
		 * Normal reception of packet must only occur if it does not
		 * come from the server, as specified. However, it is not
		 * unlikely that our server is a relay too. Hence, we must
		 * further process packets from it.
		 * At the moment, we only drop bubble (see above).
		 */
	}
	else if (!s.up)
		/* Not qualified -> do not accept incoming packets */
		return 0;
#endif /* MIREDO_TEREDO_CLIENT */

	/*
	 * NOTE:
	 * In the client case, the spec says we should check that the
	 * destination is our Teredo IPv6 address. However, this library makes
	 * no difference between relay, host-specific relay and client
	 * (it very much sounds like market segmentation to me).
	 * We purposedly leave it up to the kernel to determine whether he
	 * should accept, route, or drop the packet, according to its
	 * configuration. It is expected that client will normally not have
	 * IPv6 forwarding enabled, so that the kernel will actually make said
	 * destination address check itself.
	 *
	 * In the relay case, it is sayd we should accept packet toward the range
	 * of hosts for which we serve as a Teredo relay, and should otherwise
	 * drop it. That should be done just before sending the packet. We leave
	 * it up to the network administrator to configure or not configure
	 * source address filtering on its Teredo relay/router, via standard
	 * firewalling (i.e. NetFilter/iptables on Linux).
	 *
	 * It should also be noted that dropping packets with link-local
	 * destination here, before further processing, breaks connectivity
	 * with restricted Teredo clients: we send them Teredo bubbles with
	 * a link-local source, to which they reply with Teredo bubbles with
	 * a link-local destination. Indeed, the specification specifies that
	 * the relay MUST look up the peer in the list and update last
	 * reception date even if the destination is incorrect.
	 *
	 * Therefore, we don't check that the destination in any case. However, we
	 * DO check source address quite a lot...
	 */

	/*
	 * Packets with a link-local source address are purposedly dropped to
	 * prevent the kernel from receiving faked Router Advertisement which
	 * could break IPv6 routing completely. Router advertisements MUST
	 * have a link-local source address (RFC 2461).
	 *
	 * Note: only Linux defines convenient s6_addr16, so we don't use it.
	 *
	 * In no case are relays and clients supposed to receive and process
	 * such a packet *except* from their server (that processing is done
	 * in the "Maintenance" case above), or bubbles from other restricted
	 * clients/relays, which can safely be ignored (so long as the other
	 * bubbles sent through the server are not ignored).
	 *
	 * This check is not part of the Teredo specification.
	 */
	if ((((uint16_t *)ip6.ip6_src.s6_addr)[0] & 0xfec0) == 0xfe80)
		return 0;

	/* Actual packet reception, either as a relay or a client */

	time_t now = time (NULL);

	// Checks source IPv6 address / looks up peer in the list:
	struct teredo_peerlist *list = tunnel->list;
	teredo_peer *p = teredo_list_lookup (list, now, &ip6.ip6_src, NULL);

	if (p != NULL)
	{
//		syslog (LOG_DEBUG, " peer is %strusted", p->trusted ? "" : "NOT ");
//		syslog (LOG_DEBUG, " not checking validity");
//		syslog (LOG_DEBUG, " pings = %u, bubbles = %u", p->pings, p->bubbles);

		// Client case 1 (trusted node or (trusted) Teredo client):
		if (p->trusted
		 && (packet.source_ipv4 == p->mapped_addr)
		 && (packet.source_port == p->mapped_port))
		{
			TouchReceive (p, now);
			Dequeue (p, tunnel->fd, _sendIPv6Packet, this);
			p->bubbles = p->pings = 0;
			teredo_list_release (list);
			return SendIPv6Packet (buf, length);
		}

#ifdef MIREDO_TEREDO_CLIENT
		// Client case 2 (untrusted non-Teredo node):
		if (IsClient () && (!p->trusted) && (CheckPing (&packet) == 0))
		{
			p->trusted = 1;
			p->bubbles = p->pings = 0;

			SetMappingFromPacket (p, &packet);
			TouchReceive (p, now);
			Dequeue (p, tunnel->fd, _sendIPv6Packet, this);
			teredo_list_release (list);
			return 0; /* don't pass ping to kernel */
		}
#endif /* ifdef MIREDO_TEREDO_CLIENT */
	}
//	else
//		syslog (LOG_DEBUG, " unknown peer");

	/*
	 * At this point, we have either a trusted mapping mismatch,
	 * an unlisted peer, or an un-trusted client peer.
	 */
	if (IN6_TEREDO_PREFIX (&ip6.ip6_src) == s.addr.teredo.prefix)
	{
		// Client case 3 (unknown or untrusted matching Teredo client):
		if (IN6_MATCHES_TEREDO_CLIENT (&ip6.ip6_src, packet.source_ipv4,
		                               packet.source_port))
		{
			if (p == NULL)
			{
#ifdef MIREDO_TEREDO_CLIENT
				if (IsClient ())
				{
					bool create;

					// TODO: do not duplicate this code
					p = teredo_list_lookup (list, now, &ip6.ip6_src, &create);
					if (p == NULL)
						return -1; // insufficient memory

					/* FIXME: seemingly useless*/
					if (create)
						p->trusted = p->bubbles = p->pings = 0;
					//else race condition - peer created by another thread
					SetMapping (p, IN6_TEREDO_IPV4 (&ip6.ip6_src),
					            IN6_TEREDO_PORT (&ip6.ip6_src));
				}
				else
#endif
				/*
				 * Relays are explicitly allowed to drop packets from
				 * unknown peers. It makes it a little more difficult to route
				 * packets through the wrong relay. The specification leaves
				 * us a choice here. It is arguable whether accepting these
				 * packets would make it easier to DoS the peer list.
				 */
					return 0; // list not locked
			}
			else
				Dequeue (p, tunnel->fd, _sendIPv6Packet, this);

			p->trusted = 1;
			p->bubbles = /*p->pings -USELESS- =*/ 0;
			TouchReceive (p, now);
			teredo_list_release (list);

			if (IsBubble (&ip6))
				return 0; // discard Teredo bubble
			return SendIPv6Packet (buf, length);
		}

		// TODO: remove this line if we implement local teredo
		if (p != NULL)
			teredo_list_release (list);
		return 0;
	}

	assert (IN6_TEREDO_PREFIX (&ip6.ip6_src) != s.addr.teredo.prefix);

#ifdef MIREDO_TEREDO_CLIENT
	if (IsClient ())
	{
		// TODO: implement client cases 4 & 5 for local Teredo
	
		/*
		* Default: Client case 6:
		* (unknown non-Teredo node or Tereco client with incorrect mapping):
		* We should be cautious when accepting packets there, all the
		* more as we don't know if we are a really client or just a
		* qualified relay (ie. whether the host's default route is
		* actually the Teredo tunnel).
		*/
	
		// TODO: avoid code duplication (direct IPv6 connectivity test)
		if (p == NULL)
		{
			bool create;

			// TODO: do not duplicate this code
			p = teredo_list_lookup (list, now, &ip6.ip6_src, &create);
			if (p == NULL)
				return -1; // memory error

			if (create)
			{
				p->mapped_port = 0;
				p->mapped_addr = 0;
				p->trusted = p->bubbles = p->pings = 0;
			}
			//else race condition - peer already created by another thread
				// -> nothing to set in that case
//			syslog (LOG_DEBUG, " peer created");
		}

//		syslog (LOG_DEBUG, " packet queued pending Echo Reply");
		QueueIncoming (p, buf, length);
		TouchReceive (p, now);
	
		int res = PingPeer (tunnel->fd, p, now, &s.addr, &ip6.ip6_src) ? -1:0;
		teredo_list_release (list);
// 		syslog (LOG_DEBUG, " PingPeer returned %d", res);
		return res;
	}
#endif /* ifdef MIREDO_TEREDO_CLIENT */

	// Relays don't accept packets not from Teredo clients,
	// nor from mismatching packets
	if (p != NULL)
		teredo_list_release (list);

	return 0;
}



static void libteredo_dummy_recv_cb (void *, const void *, size_t)
{
}


static void libteredo_dummy_icmpv6_cb (void *, const void *, size_t,
                                       const struct in6_addr *)
{
}


#ifdef MIREDO_TEREDO_CLIENT
static void libteredo_dummy_state_up_cb (void *, const struct in6_addr *,
                                         uint16_t)
{
}


static void libteredo_dummy_state_down_cb (void *)
{
}
#endif


/**
 * Creates a libteredo_tunnel instance. libteredo_preinit() must have been
 * called first.
 *
 * This function is thread-safe.
 *
 * @param ipv4 IPv4 (network byte order) to bind to, or 0 if unspecified.
 * @param port UDP/IPv4 port number (network byte order) or 0 if unspecified.
 * Note that some campus firewall drop certain UDP ports (typically those used
 * by some P2P application); in that case, you should use a fixed port so that
 * the kernel does not select a possibly blocked port. Also note that some
 * severely broken NAT devices might fail if multiple NAT-ed computers use the
 * same source UDP port number at the same time, so avoid you should
 * paradoxically avoid querying a fixed port.
 *
 * @return NULL in case of failure.
 */
extern "C"
libteredo_tunnel *libteredo_create (uint32_t ipv4, uint16_t port)
{
	libteredo_tunnel *tunnel = (libteredo_tunnel *)malloc (sizeof (*tunnel));
	if (tunnel == NULL)
		return NULL;

	memset (tunnel, 0, sizeof (tunnel));
	tunnel->state.addr.teredo.prefix = htonl (TEREDO_PREFIX);

	/*
	 * That doesn't really need to match our mapping: the address is only
	 * used to send Unreachable message... with the old method that is no
	 * longer supported (the one that involves building the IPv6 header as
	 * well as the ICMPv6 header).
	 */
	tunnel->state.addr.teredo.client_port = ~port;
	tunnel->state.addr.teredo.client_ip = ~ipv4;

	tunnel->recv_cb = libteredo_dummy_recv_cb;
	tunnel->icmpv6_cb = libteredo_dummy_icmpv6_cb;
#ifdef MIREDO_TEREDO_CLIENT
	tunnel->up_cb = libteredo_dummy_state_up_cb;
	tunnel->down_cb = libteredo_dummy_state_down_cb;
#endif

	if ((tunnel->fd = teredo_socket (ipv4, port)) != -1)
	{
		if ((tunnel->list = teredo_list_create (MAX_PEERS, 30)) != NULL)
		{
			/* FIXME: use debug attributes when appropriate */
			if (pthread_rwlock_init (&tunnel->state_lock, NULL) == 0)
				return tunnel;

			teredo_list_destroy (tunnel->list);
		}
		teredo_close (tunnel->fd);
	}

	free (tunnel);
	return NULL;
}


/**
 * Releases all resources (sockets, memory chunks...) and terminates all
 * threads associated with a libteredo_tunnel instance.
 *
 * @param t tunnel to be destroyed. No longer useable thereafter.
 *
 * @return nothing (always succeeds).
 */
extern "C"
void libteredo_destroy (libteredo_tunnel *t)
{
	assert (t != NULL);
	assert (t->fd != -1);
	assert (t->list != NULL);

	if (t->object != NULL)
		delete t->object;

	teredo_list_destroy (t->list);
	pthread_rwlock_destroy (&t->state_lock);
	teredo_close (t->fd);
	free (t);
}


/* FIXME: document */
extern "C"
int libteredo_register_readset (libteredo_tunnel *t, fd_set *rdset)
{
	assert (t != NULL);
	assert (t->fd != -1);

	// FIXME: May be problematic once multicast local discovery gets
	// implemented.

	FD_SET (t->fd, rdset);
	return t->fd;

}


/**
 * Receives a packet from Teredo to IPv6 Internet, i.e.
 * performs “Packet reception”. This function will NOT block until
 * a Teredo packet is received (but maybe it should).
 * Not thread-safe yet.
*/
/* FIXME: kill (run in a separate thread) or document */
/* FIXME: document assumption about t->object */
extern "C"
void libteredo_run (libteredo_tunnel *t)
{
	assert (t != NULL);
	assert (t->object != NULL);

	t->object->ReceivePacket ();
}


/**
 * Overrides the Teredo prefix of a Teredo relay. It is ignored if the tunnel
 * is configured as a Teredo client. libteredo_set_prefix() is undefined if
 * libteredo_set_cone_flag() was already invoked.
 *
 * @param prefix Teredo 32-bits (network byte order) prefix.
 *
 * @return 0 on success, -1 if the prefix is invalid (in which case the
 * libteredo_tunnel instance is not modified).
 */
extern "C"
int libteredo_set_prefix (libteredo_tunnel *t, uint32_t prefix)
{
	assert (t != NULL);

	if (!is_valid_teredo_prefix (prefix))
		return -1;

	t->state.addr.teredo.prefix = prefix;
	return 0;
}


/**
 * Enables Teredo relay mode for a libteredo_tunnel,
 * defines whether it will operate as a “cone” or “restricted” relay,
 * and starts processing of encapsulated IPv6 packets.
 * This is ignored if Teredo client mode was previously enabled.
 *
 * @param cone true if the cone flag should be enabled. This only works if the
 * relay runs from behind a cone NAT and has no stateful firewall for incoming
 * UDP packets. If there is a stateful firewall or a restricted-port NAT,
 * flag must be false.
 *
 * @return 0 if the initialization was succesful, -1 in case of error.
 * In case of error, the libteredo_tunnel instance is not modifed.
 */
extern "C"
int libteredo_set_cone_flag (libteredo_tunnel *t, bool cone)
{
	assert (t != NULL);

	if (cone)
	{
		t->state.addr.teredo.flags = htons (TEREDO_FLAG_CONE);
		t->state.cone = true;
	}
	t->state.up = true;

	TeredoRelay *r;
	try
	{
		r = new TeredoRelay (t);
	}
	catch (...)
	{
		r = NULL;
	}

	if (r != NULL)
	{
		t->object = r;
		return 0;
	}

	return -1;
}


/**
 * Enables Teredo client mode for a libteredo_tunnel and starts the Teredo
 * client maintenance procedure in a separate thread. This is undefined if
 * either libteredo_set_cone_flag() or libteredo_set_prefix() were previously
 * called for this tunnel.
 *
 * @param s1 Teredo server's host name or “dotted quad” primary IPv4 address.
 * @param s2 Teredo server's secondary address (or host name), or NULL to
 * infer it from <s1>.
 *
 * @return 0 on success, -1 in case of error.
 * In case of error, the libteredo_tunnel instance is not modifed.
 */
extern "C"
int libteredo_set_client_mode (libteredo_tunnel *t, const char *s1,
                               const char *s2)
{
	assert (t != NULL);

#ifdef MIREDO_TEREDO_CLIENT
	TeredoRelay *r;
	try
	{
		r = new TeredoRelay (t, s1, s2);
	}
	catch (...)
	{
		r = NULL;
	}

	if (r != NULL)
	{
		t->object = r;
		return 0;
	}
#else
	(void)t;
	(void)s1;
	(void)s2;
#endif
	return -1;
}


/**
 * Enables/disables the processing of the cone flag found in other Teredo
 * client's IPv6 addresses. By default, the cone flag is ignored, because this
 * supposedly increase reliability of the Teredo tunneling mechanism.
 *
 * @param ignore true to enable processing, false to disable.
 */
extern "C"
void libteredo_set_cone_ignore (libteredo_tunnel *t, bool ignore)
{
	assert (t != NULL);
	t->allow_cone = !ignore;
}

extern "C"
void *libteredo_set_privdata (libteredo_tunnel *t, void *opaque)
{
	assert (t != NULL);

	void *prev = t->opaque;
	t->opaque = opaque;
	return prev;
}


extern "C"
void *libteredo_get_privdata (const libteredo_tunnel *t)
{
	assert (t != NULL);

	return t->opaque;
}


extern "C"
void libteredo_set_recv_callback (libteredo_tunnel *t, libteredo_recv_cb cb)
{
	assert (t != NULL);
	t->recv_cb = (cb != NULL) ? cb : libteredo_dummy_recv_cb;
}


/**
 * Transmits a packet from IPv6 Internet via Teredo, i.e. performs
 * Teredo “Packet transmission”. Not thread-safe yet.
 *
 * It is assumed that <len> > 40 and that <packet> is properly
 * aligned. Otherwise, behavior is undefined.
 *
 * @return 0 on success, -1 on error.
 */
extern "C"
int libteredo_send (libteredo_tunnel *t,
                    const struct ip6_hdr *packet, size_t len)
{
	assert (t != NULL);
	assert (t->object != NULL); /* FIXME: document assumption */

	return t->object->SendPacket (packet, len);
}


void libteredo_set_icmpv6_callback (libteredo_tunnel *t,
                                    libteredo_icmpv6_cb cb)
{
	assert (t != NULL);
	t->icmpv6_cb = (cb != NULL) ? cb : libteredo_dummy_icmpv6_cb;
}


/**
 * Registers callbacks to be called when the Teredo client maintenance
 * procedure detects that the tunnel becomes usable (or has got a new IPv6
 * address, or a new MTU), or unusable respectively.
 * These callbacks are ignored for a Teredo relay tunnel.
 *
 * Any packet sent when the relay/client is down will be ignored.
 * The callbacks function might be called from a separate thread.
 *
 * If a callback is set to NULL, it is ignored.
 */
void libteredo_set_state_cb (libteredo_tunnel *t, libteredo_state_up_cb u,
                             libteredo_state_down_cb d)
{
#ifdef MIREDO_TEREDO_CLIENT
	assert (t != NULL);
	t->up_cb = (u != NULL) ? u : libteredo_dummy_state_up_cb;
	t->down_cb = (d != NULL) ? d : libteredo_dummy_state_down_cb;
#else
	(void)t;
	(void)u;
	(void)d;
#endif
}


#ifdef MIREDO_TEREDO_CLIENT
void TeredoRelay::NotifyUp (const struct in6_addr *addr, uint16_t mtu)
{
	libteredo_state_up_cb cb = tunnel->up_cb;
	assert (cb != NULL);
	cb (tunnel->opaque, addr, mtu);
}

void TeredoRelay::NotifyDown (void)
{
	libteredo_state_down_cb cb = tunnel->down_cb;
	assert (cb != NULL);
	cb (tunnel->opaque);
}
#endif

void TeredoRelay::EmitICMPv6Error (const void *packet, size_t length,
                                    const struct in6_addr *dst)
{
	libteredo_icmpv6_cb cb = tunnel->icmpv6_cb;
	assert (cb != NULL);
	cb (tunnel->opaque, packet, length, dst);
}

int TeredoRelay::SendIPv6Packet (const void *packet, size_t length)
{
	libteredo_recv_cb cb = tunnel->recv_cb;
	assert (cb != NULL);
	cb (tunnel->opaque, packet, length);
	return 0;
}
