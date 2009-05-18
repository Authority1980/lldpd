/*
 * Copyright (c) 2008 Vincent Bernat <bernat@luffy.cx>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "lldpd.h"
#include "frame.h"

#ifdef ENABLE_EDP

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <fnmatch.h>

static int seq = 0;

int
edp_send(struct lldpd *global, struct lldpd_chassis *chassis,
    struct lldpd_hardware *hardware)
{
	const u_int8_t mcastaddr[] = EDP_MULTICAST_ADDR;
	const u_int8_t llcorg[] = LLC_ORG_EXTREME;
	int length, i, v;
	u_int8_t *packet, *pos, *pos_llc, *pos_len_eh, *pos_len_edp, *pos_edp, *tlv, *end;
	u_int16_t checksum;
#ifdef ENABLE_DOT1
	struct lldpd_vlan *vlan;
	unsigned int state = 0;
#endif
	u_int8_t edp_fakeversion[] = {7, 6, 4, 99};
	/* Subsequent XXX can be replaced by other values. We place
	   them here to ensure the position of "" to be a bit
	   invariant with version changes. */
	char *deviceslot[] = { "eth", "veth", "XXX", "XXX", "XXX", "XXX", "XXX", "XXX", "", NULL };

#ifdef ENABLE_DOT1
	while (state != 2) {
#endif
		length = hardware->h_mtu;
		if ((packet = (u_int8_t*)malloc(length)) == NULL)
			return ENOMEM;
		memset(packet, 0, length);
		pos = packet;
		v = 0;

		/* Ethernet header */
		if (!(
		      POKE_BYTES(mcastaddr, sizeof(mcastaddr)) &&
		      POKE_BYTES(&hardware->h_lladdr, sizeof(hardware->h_lladdr)) &&
		      POKE_SAVE(pos_len_eh) && /* We compute the len later */
		      POKE_UINT16(0)))
			goto toobig;

		/* LLC */
		if (!(
		      POKE_SAVE(pos_llc) && /* We need to save our
					       current position to
					       compute ethernet len */
		      /* SSAP and DSAP */
		      POKE_UINT8(0xaa) && POKE_UINT8(0xaa) &&
		      /* Control field */
		      POKE_UINT8(0x03) &&
		      /* ORG */
		      POKE_BYTES(llcorg, sizeof(llcorg)) &&
		      POKE_UINT16(LLC_PID_EDP)))
			goto toobig;

		/* EDP header */
		if ((chassis->c_id_len != ETH_ALEN) ||
		    (chassis->c_id_subtype != LLDP_CHASSISID_SUBTYPE_LLADDR)) {
			LLOG_WARNX("local chassis does not use MAC address as chassis ID!?");
			free(packet);
			return EINVAL;
		}
		if (!(
		      POKE_SAVE(pos_edp) && /* Save the start of EDP frame */
		      POKE_UINT8(1) && POKE_UINT8(0) &&
		      POKE_SAVE(pos_len_edp) && /* We compute the len
						   and the checksum
						   later */
		      POKE_UINT32(0) && /* Len + Checksum */
		      POKE_UINT16(seq) &&
		      POKE_UINT16(0) &&
		      POKE_BYTES(chassis->c_id, ETH_ALEN)))
			goto toobig;
		seq++;

#ifdef ENABLE_DOT1
		switch (state) {
		case 0:
#endif
			/* Display TLV */
			if (!(
			      POKE_START_EDP_TLV(EDP_TLV_DISPLAY) &&
			      POKE_BYTES(chassis->c_name, strlen(chassis->c_name)) &&
			      POKE_UINT8(0) && /* Add a NULL character
						  for better
						  compatibility */
			      POKE_END_EDP_TLV))
				goto toobig;

			/* Info TLV */
			if (!(
			      POKE_START_EDP_TLV(EDP_TLV_INFO)))
				goto toobig;
			/* We try to emulate the slot thing */
			for (i=0; deviceslot[i] != NULL; i++) {
				if (strncmp(hardware->h_ifname, deviceslot[i],
					strlen(deviceslot[i])) == 0) {
					if (!(
					      POKE_UINT16(i) &&
					      POKE_UINT16(atoi(hardware->h_ifname +
							       strlen(deviceslot[i])))))
						goto toobig;
					break;
				}
			}
			/* If we don't find a "slot", we say that the
			   interface is in slot 8 */
			if (deviceslot[i] == NULL) {
				if (!(
				      POKE_UINT16(8) &&
				      POKE_UINT16(if_nametoindex(hardware->h_ifname))))
					goto toobig;
			}
			if (!(
			      POKE_UINT16(0) && /* vchassis */
			      POKE_UINT32(0) && POKE_UINT16(0) && /* Reserved */
			      /* Version */
			      POKE_BYTES(edp_fakeversion, sizeof(edp_fakeversion)) &&
			      /* Connections, we say that we won't
				 have more interfaces than this
				 mask. */
			      POKE_UINT32(0xffffffff) &&
			      POKE_UINT32(0) && POKE_UINT32(0) && POKE_UINT32(0) &&
			      POKE_END_EDP_TLV))
				goto toobig;

#ifdef ENABLE_DOT1
			break;
		case 1:
			TAILQ_FOREACH(vlan, &hardware->h_lport.p_vlans,
			    v_entries) {
				v++;
				if (!(
				      POKE_START_EDP_TLV(EDP_TLV_VLAN) &&
				      POKE_UINT8(0) && /* Flags: no IP address */
				      POKE_UINT8(0) && /* Reserved */
				      POKE_UINT16(vlan->v_vid) &&
				      POKE_UINT32(0) && /* Reserved */
				      POKE_UINT32(0) && /* IP address */
				      /* VLAN name */
				      POKE_BYTES(vlan->v_name, strlen(vlan->v_name)) &&
				      POKE_UINT8(0) &&
				      POKE_END_EDP_TLV))
					goto toobig;
			}
			break;
		}

		if ((state == 1) && (v == 0))	/* No VLAN, no need to send another TLV */
			break;
#endif
			
		/* Null TLV */
		if (!(
		      POKE_START_EDP_TLV(EDP_TLV_NULL) &&
		      POKE_END_EDP_TLV &&
		      POKE_SAVE(end)))
			goto toobig;

		/* Compute len and checksum */
		i = end - pos_llc; /* Ethernet length */
		v = end - pos_edp; /* EDP length */
		POKE_RESTORE(pos_len_eh);
		if (!(POKE_UINT16(i))) goto toobig;
		POKE_RESTORE(pos_len_edp);
		if (!(POKE_UINT16(v))) goto toobig;
		checksum = frame_checksum(pos_edp, v, 0);
		if (!(POKE_UINT16(ntohs(checksum)))) goto toobig;

		if (write(hardware->h_raw, packet, end - packet) == -1) {
			LLOG_WARN("unable to send packet on real device for %s",
			    hardware->h_ifname);
			free(packet);
			return ENETDOWN;
		}
		free(packet);

#ifdef ENABLE_DOT1		
		state++;
	}
#endif

	hardware->h_tx_cnt++;
	return 0;
 toobig:
	free(packet);
	return E2BIG;
}

#define CHECK_TLV_SIZE(x, name)				   \
	do { if (tlv_len < (x)) {			   \
	   LLOG_WARNX(name " EDP TLV too short received on %s",\
	       hardware->h_ifname);			   \
	   goto malformed;				   \
	} } while (0)

int
edp_decode(struct lldpd *cfg, char *frame, int s,
    struct lldpd_hardware *hardware,
    struct lldpd_chassis **newchassis, struct lldpd_port **newport)
{
	struct lldpd_chassis *chassis;
	struct lldpd_port *port;
#ifdef ENABLE_DOT1
	struct lldpd_vlan *lvlan = NULL, *lvlan_next;
#endif
	const unsigned char edpaddr[] = EDP_MULTICAST_ADDR;
	int length, gotend = 0, gotvlans = 0, edp_len, tlv_len, tlv_type;
	int edp_port, edp_slot;
	u_int8_t *pos, *pos_edp, *tlv;
	u_int8_t version[4];
	struct in_addr address;

	if ((chassis = calloc(1, sizeof(struct lldpd_chassis))) == NULL) {
		LLOG_WARN("failed to allocate remote chassis");
		return -1;
	}
	if ((port = calloc(1, sizeof(struct lldpd_port))) == NULL) {
		LLOG_WARN("failed to allocate remote port");
		free(chassis);
		return -1;
	}
#ifdef ENABLE_DOT1
	TAILQ_INIT(&port->p_vlans);
#endif

	length = s;
	pos = (u_int8_t*)frame;

	if (length < 2*ETH_ALEN + sizeof(u_int16_t) + 8 /* LLC */ +
	    10 + ETH_ALEN /* EDP header */) {
		LLOG_WARNX("too short EDP frame received on %s", hardware->h_ifname);
		goto malformed;
	}

	if (PEEK_CMP(edpaddr, sizeof(edpaddr)) != 0) {
		LLOG_INFO("frame not targeted at EDP multicast address received on %s",
		    hardware->h_ifname);
		goto malformed;
	}
	PEEK_DISCARD(ETH_ALEN); PEEK_DISCARD_UINT16;
	PEEK_DISCARD(6);	/* LLC: DSAP + SSAP + control + org */
	if (PEEK_UINT16 != LLC_PID_EDP) {
		LLOG_DEBUG("incorrect LLC protocol ID received on %s",
		    hardware->h_ifname);
		goto malformed;
	}

	PEEK_SAVE(pos_edp);	/* Save the start of EDP packet */
	if (PEEK_UINT8 != 1) {
		LLOG_WARNX("incorrect EDP version for frame received on %s",
		    hardware->h_ifname);
		goto malformed;
	}
	PEEK_DISCARD_UINT8;	/* Reserved */
	edp_len = PEEK_UINT16;
	PEEK_DISCARD_UINT16;	/* Checksum */
	PEEK_DISCARD_UINT16;	/* Sequence */
	if (PEEK_UINT16 != 0) {	/* ID Type = 0 = MAC */
		LLOG_WARNX("incorrect device id type for frame received on %s",
		    hardware->h_ifname);
		goto malformed;
	}
	if (edp_len > length + 10) {
		LLOG_WARNX("incorrect size for EDP frame received on %s",
		    hardware->h_ifname);
		goto malformed;
	}
	chassis->c_ttl = LLDPD_TTL;
	chassis->c_id_subtype = LLDP_CHASSISID_SUBTYPE_LLADDR;
	chassis->c_id_len = ETH_ALEN;
	if ((chassis->c_id = (char *)malloc(ETH_ALEN)) == NULL) {
		LLOG_WARN("unable to allocate memory for chassis ID");
		goto malformed;
	}
	PEEK_BYTES(chassis->c_id, ETH_ALEN);

	/* Let's check checksum */
	if (frame_checksum(pos_edp, edp_len, 0) != 0) {
		LLOG_WARNX("incorrect EDP checksum for frame received on %s",
		    hardware->h_ifname);
		goto malformed;
	}

	while (length && !gotend) {
		if (length < 4) {
			LLOG_WARNX("EDP TLV header is too large for "
			    "frame received on %s",
			    hardware->h_ifname);
			goto malformed;
		}
		if (PEEK_UINT8 != EDP_TLV_MARKER) {
			LLOG_WARNX("incorrect marker starting EDP TLV header for frame "
			    "received on %s",
			    hardware->h_ifname);
			goto malformed;
		}
		tlv_type = PEEK_UINT8;
		tlv_len = PEEK_UINT16 - 4;
		PEEK_SAVE(tlv);
		if ((tlv_len < 0) || (tlv_len > length)) {
			LLOG_DEBUG("incorrect size in EDP TLV header for frame "
			    "received on %s",
			    hardware->h_ifname);
			/* Some poor old Extreme Summit are quite bogus */
			gotend = 1;
			break;
		}
		switch (tlv_type) {
		case EDP_TLV_INFO:
			CHECK_TLV_SIZE(32, "Info");
			port->p_id_subtype = LLDP_PORTID_SUBTYPE_IFNAME;
			edp_slot = PEEK_UINT16; edp_port = PEEK_UINT16;
			if (asprintf(&port->p_id, "%d/%d",
				edp_slot + 1, edp_port + 1) == -1) {
				LLOG_WARN("unable to allocate memory for "
				    "port ID");
				goto malformed;
			}
			port->p_id_len = strlen(port->p_id);
			if (asprintf(&port->p_descr, "Slot %d / Port %d",
				edp_slot + 1, edp_port + 1) == -1) {
				LLOG_WARN("unable to allocate memory for "
				    "port description");
				goto malformed;
			}
			PEEK_DISCARD_UINT16; /* vchassis */
			PEEK_DISCARD(6);     /* Reserved */
			PEEK_BYTES(version, 4);
			if (asprintf(&chassis->c_descr,
				"EDP enabled device, version %d.%d.%d.%d",
				version[0], version[1],
				version[2], version[3]) == -1) {
				LLOG_WARN("unable to allocate memory for "
				    "chassis description");
				goto malformed;
			}
			break;
		case EDP_TLV_DISPLAY:
			if ((chassis->c_name = (char *)calloc(1, tlv_len + 1)) == NULL) {
				LLOG_WARN("unable to allocate memory for chassis "
				    "name");
				goto malformed;
			}
			/* TLV display contains a lot of garbage */
			PEEK_BYTES(chassis->c_name, tlv_len);
			break;
		case EDP_TLV_NULL:
			if (tlv_len != 0) {
				LLOG_WARNX("null tlv with incorrect size in frame "
				    "received on %s",
				    hardware->h_ifname);
				goto malformed;
			}
			if (length)
				LLOG_DEBUG("extra data after edp frame on %s",
				    hardware->h_ifname);
			gotend = 1;
			break;
		case EDP_TLV_VLAN:
#ifdef ENABLE_DOT1
			CHECK_TLV_SIZE(12, "VLAN");
			if ((lvlan = (struct lldpd_vlan *)calloc(1,
				    sizeof(struct lldpd_vlan))) == NULL) {
				LLOG_WARN("unable to allocate vlan");
				goto malformed;
			}
			PEEK_DISCARD_UINT16; /* Flags + reserved */
			lvlan->v_vid = PEEK_UINT16; /* VID */
			PEEK_DISCARD(4);	    /* Reserved */
			PEEK_BYTES(&address, sizeof(address));

			if ((lvlan->v_name = (char *)calloc(1,
				    tlv_len + 1 - 12)) == NULL) {
				LLOG_WARN("unable to allocate vlan name");
				free(lvlan);
				goto malformed;
			}
			PEEK_BYTES(lvlan->v_name, tlv_len - 12);

			if (address.s_addr != INADDR_ANY) {
				if (chassis->c_mgmt.s_addr == INADDR_ANY)
					chassis->c_mgmt.s_addr = address.s_addr;
				else
					/* We need to guess the good one */
					if (cfg->g_mgmt_pattern != NULL) {
						/* We can try to use this to prefer an address */
						char *ip;
						ip = inet_ntoa(address);
						if (fnmatch(cfg->g_mgmt_pattern,
							ip, 0) == 0)
							chassis->c_mgmt.s_addr = address.s_addr;
					}
			}
			TAILQ_INSERT_TAIL(&port->p_vlans,
			    lvlan, v_entries);
#endif
			gotvlans = 1;
			break;
		default:
			LLOG_DEBUG("unknown EDP TLV type (%d) received on %s",
			    tlv_type, hardware->h_ifname);
			hardware->h_rx_unrecognized_cnt++;
		}
		PEEK_DISCARD(tlv + tlv_len - pos);
	}
	if ((chassis->c_id == NULL) ||
	    (port->p_id == NULL) ||
	    (chassis->c_name == NULL) ||
	    (chassis->c_descr == NULL) ||
	    (port->p_descr == NULL) ||
	    (gotend == 0)) {
#ifdef ENABLE_DOT1
		if (gotvlans && gotend) {
			/* VLAN can be sent in a separate frames. We need to add
			 * those vlans to an existing chassis */
			if (hardware->h_rchassis &&
			    (hardware->h_rchassis->c_id_subtype == chassis->c_id_subtype) &&
			    (hardware->h_rchassis->c_id_len == chassis->c_id_len) &&
			    (memcmp(hardware->h_rchassis->c_id, chassis->c_id,
				chassis->c_id_len) == 0)) {
				/* We attach the VLANs to current hardware */
				lldpd_vlan_cleanup(hardware->h_rport);
				for (lvlan = TAILQ_FIRST(&port->p_vlans);
				     lvlan != NULL;
				     lvlan = lvlan_next) {
					lvlan_next = TAILQ_NEXT(lvlan, v_entries);
					TAILQ_REMOVE(&port->p_vlans, lvlan, v_entries);
					TAILQ_INSERT_TAIL(&hardware->h_rport->p_vlans,
					    lvlan, v_entries);
				}
				/* And the IP address */
				hardware->h_rchassis->c_mgmt.s_addr =
				    chassis->c_mgmt.s_addr;
			}
			/* We discard the remaining frame */
			goto malformed;
		}
#else
		if (gotvlans)
			goto malformed;
#endif
		LLOG_WARNX("some mandatory tlv are missing for frame received on %s",
		    hardware->h_ifname);
		goto malformed;
	}
	*newchassis = chassis;
	*newport = port;
	return 1;

malformed:
	lldpd_chassis_cleanup(chassis);
	lldpd_port_cleanup(port, 1);
	return -1;
}

#endif /* ENABLE_EDP */
