/*
 * radius.c	Functions to send/receive radius packets.
 *
 * Version:	$Id$
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA
 *
 * Copyright 2000-2003  The FreeRADIUS server project
 */

static const char rcsid[] = "$Id$";

#include	"autoconf.h"
#include	"md5.h"

#include	<stdlib.h>

#ifdef HAVE_UNISTD_H
#include	<unistd.h>
#endif

#include	<fcntl.h>
#include	<string.h>
#include	<ctype.h>

#ifdef WITH_UDPFROMTO
#include	"udpfromto.h"
#endif

#include	<sys/socket.h>

#ifdef HAVE_ARPA_INET_H
#include	<arpa/inet.h>
#endif

#ifdef HAVE_MALLOC_H
#include	<malloc.h>
#endif

#ifdef WIN32
#include	<process.h>
#endif

#include	"missing.h"
#include	"libradius.h"

/*
 *  The RFC says 4096 octets max, and most packets are less than 256.
 */
#define MAX_PACKET_LEN 4096

/*
 *	The maximum number of attributes which we allow in an incoming
 *	request.  If there are more attributes than this, the request
 *	is rejected.
 *
 *	This helps to minimize the potential for a DoS, when an
 *	attacker spoofs Access-Request packets, which don't have a
 *	Message-Authenticator attribute.  This means that the packet
 *	is unsigned, and the attacker can use resources on the server,
 *	even if the end request is rejected.
 */
int librad_max_attributes = 0;

typedef struct radius_packet_t {
  uint8_t	code;
  uint8_t	id;
  uint8_t	length[2];
  uint8_t	vector[AUTH_VECTOR_LEN];
  uint8_t	data[1];
} radius_packet_t;

static lrad_randctx lrad_rand_pool;	/* across multiple calls */
static volatile int lrad_rand_index = -1;

static const char *packet_codes[] = {
  "",
  "Access-Request",
  "Access-Accept",
  "Access-Reject",
  "Accounting-Request",
  "Accounting-Response",
  "Accounting-Status",
  "Password-Request",
  "Password-Accept",
  "Password-Reject",
  "Accounting-Message",
  "Access-Challenge",
  "Status-Server",
  "Status-Client",
  "14",
  "15",
  "16",
  "17",
  "18",
  "19",
  "20",
  "Resource-Free-Request",
  "Resource-Free-Response",
  "Resource-Query-Request",
  "Resource-Query-Response",
  "Alternate-Resource-Reclaim-Request",
  "NAS-Reboot-Request",
  "NAS-Reboot-Response",
  "28",
  "Next-Passcode",
  "New-Pin",
  "Terminate-Session",
  "Password-Expired",
  "Event-Request",
  "Event-Response",
  "35",
  "36",
  "37",
  "38",
  "39",
  "Disconnect-Request",
  "Disconnect-ACK",
  "Disconnect-NAK",
  "CoF-Request",
  "CoF-ACK",
  "CoF-NAK",
  "46",
  "47",
  "48",
  "49",
  "IP-Address-Allocate",
  "IP-Address-Release"
};

/*
 *  Internal prototypes
 */
static void make_secret(unsigned char *digest, uint8_t *vector,
			const char *secret, char *value);


/*
 *	Wrapper for sendto which handles sendfromto, IPv6, and all
 *	possible combinations.
 */
static int rad_sendto(int sockfd, void *data, size_t data_len, int flags,
		      lrad_ipaddr_t *src_ipaddr, lrad_ipaddr_t *dst_ipaddr,
		      int dst_port)
{
	struct sockaddr_storage	dst;
	socklen_t		sizeof_dst = sizeof(dst);

#ifdef WITH_UDPFROMTO
	struct sockaddr_storage	src;
	socklen_t		sizeof_src = sizeof(src);

	memset(&src, 0, sizeof(src));
#endif
	memset(&dst, 0, sizeof(dst));

	/*
	 *	IPv4 is supported.
	 */
	if (dst_ipaddr->af == AF_INET) {
		struct sockaddr_in	*s4;

		s4 = (struct sockaddr_in *)&dst;
		sizeof_dst = sizeof(struct sockaddr_in);

		s4->sin_family = AF_INET;
		s4->sin_addr = dst_ipaddr->ipaddr.ip4addr;
		s4->sin_port = htons(dst_port);

#ifdef WITH_UDPFROMTO
		s4 = (struct sockaddr_in *)&src;
		sizeof_src = sizeof(struct sockaddr_in);

		s4->sin_family = AF_INET;
		s4->sin_addr = src_ipaddr->ipaddr.ip4addr;
#endif

	/*
	 *	IPv6 MAY be supported.
	 */
#ifdef HAVE_STRUCT_SOCKADDR_IN6
	} else if (dst_ipaddr->af == AF_INET6) {
		struct sockaddr_in6	*s6;

		s6 = (struct sockaddr_in6 *)&dst;
		sizeof_dst = sizeof(struct sockaddr_in6);
		
		s6->sin6_family = AF_INET6;
		s6->sin6_addr = dst_ipaddr->ipaddr.ip6addr;
		s6->sin6_port = htons(dst_port);

#ifdef WITH_UDPFROMTO
		return -1;	/* UDPFROMTO && IPv6 are not supported */
#if 0
		s6 = (struct sockaddr_in6 *)&src;
		sizeof_src = sizeof(struct sockaddr_in6);

		s6->sin6_family = AF_INET6;
		s6->sin6_addr = src_ipaddr->ipaddr.ip6addr;
#endif /* #if 0 */
#endif /* WITH_UDPFROMTO */
#endif /* HAVE_STRUCT_SOCKADDR_IN6 */
	} else return -1;   /* Unknown address family, Die Die Die! */

#ifdef WITH_UDPFROMTO
	/*
	 *	Only IPv4 is supported for udpfromto.
	 *
	 *	And if they don't specify a source IP address, don't
	 *	use udpfromto.
	 */
	if ((dst_ipaddr->af == AF_INET) ||
	    (src_ipaddr->af != AF_UNSPEC)) {
		return sendfromto(sockfd, data, data_len, flags,
				  (struct sockaddr *)&src, sizeof_src, 
				  (struct sockaddr *)&dst, sizeof_dst);
	}
#else
	src_ipaddr = src_ipaddr; /* -Wunused */
#endif

	/*
	 *	No udpfromto, OR an IPv6 socket, fail gracefully.
	 */
	return sendto(sockfd, data, data_len, flags, 
		(struct sockaddr *)&dst, sizeof_dst);
}


/*
 *	Wrapper for recvfrom, which handles recvfromto, IPv6, and all
 *	possible combinations.
 */
static ssize_t rad_recvfrom(int sockfd, uint8_t **pbuf, int flags,
			    lrad_ipaddr_t *src_ipaddr, uint16_t *src_port,
			    lrad_ipaddr_t *dst_ipaddr, uint16_t *dst_port)
{
	struct sockaddr_storage	src;
	struct sockaddr_storage	dst;
	socklen_t		sizeof_src = sizeof(src);
	socklen_t	        sizeof_dst = sizeof(dst);
	ssize_t			data_len;
	uint8_t			header[4];
	void			*buf;
	size_t			len;

	memset(&src, 0, sizeof_src);
	memset(&dst, 0, sizeof_dst);

	/*
	 *	Get address family, etc. first, so we know if we
	 *	need to do udpfromto.
	 *
	 *	FIXME: udpfromto also does this, but it's not
	 *	a critical problem.
	 */
	if (getsockname(sockfd, (struct sockaddr *)&dst,
			&sizeof_dst) < 0) return -1;

	/*
	 *	Read the length of the packet, from the packet.
	 *	This lets us allocate the buffer to use for
	 *	reading the rest of the packet.
	 */
	data_len = recvfrom(sockfd, header, sizeof(header), MSG_PEEK,
			    (struct sockaddr *)&src, &sizeof_src);
	if (data_len < 0) return -1;

	/*
	 *	Too little data is available, round it up to 4 bytes.
	 */
	if (data_len < 4) {
		len = 4;
	} else {		/* we got 4 bytes of data. */
		/*
		 *	See how long the packet says it is.
		 */
		len = (header[2] * 256) + header[3];

		/*
		 *	Too short: read 4 bytes, and discard the rest.
		 */
		if (len < 4) {
			len = 4;

			/*
			 *	Enforce RFC requirements, for sanity.
			 *	Anything after 4k will be discarded.
			 */
		} else if (len > MAX_PACKET_LEN) {
			len = MAX_PACKET_LEN;
		}
	}

	buf = malloc(len);
	if (!buf) return -1;

	/*
	 *	Receive the packet.  The OS will discard any data in the
	 *	packet after "len" bytes.
	 */
#ifdef WITH_UDPFROMTO
	if (dst.ss_family == AF_INET) {
		data_len = recvfromto(sockfd, buf, len, flags,
				      (struct sockaddr *)&src, &sizeof_src, 
				      (struct sockaddr *)&dst, &sizeof_dst);
	} else
#endif
		/*
		 *	No udpfromto, OR an IPv6 socket.  Fail gracefully.
		 */
		data_len = recvfrom(sockfd, buf, len, flags, 
				    (struct sockaddr *)&src, &sizeof_src);
	if (data_len < 0) {
		free(buf);
		return data_len;
	}

	/*
	 *	Check address families, and update src/dst ports, etc.
	 */
	if (src.ss_family == AF_INET) {
		struct sockaddr_in	*s4;

		s4 = (struct sockaddr_in *)&src;
		src_ipaddr->af = AF_INET;
		src_ipaddr->ipaddr.ip4addr = s4->sin_addr;
		*src_port = ntohs(s4->sin_port);

		s4 = (struct sockaddr_in *)&dst;
		dst_ipaddr->af = AF_INET;
		dst_ipaddr->ipaddr.ip4addr = s4->sin_addr;
		*dst_port = ntohs(s4->sin_port);

#ifdef HAVE_STRUCT_SOCKADDR_IN6
	} else if (src.ss_family == AF_INET6) {
		struct sockaddr_in6	*s6;

		s6 = (struct sockaddr_in6 *)&src;
		src_ipaddr->af = AF_INET6;
		src_ipaddr->ipaddr.ip6addr = s6->sin6_addr;
		*src_port = ntohs(s6->sin6_port);

		s6 = (struct sockaddr_in6 *)&dst;
		dst_ipaddr->af = AF_INET6;
		dst_ipaddr->ipaddr.ip6addr = s6->sin6_addr;
		*dst_port = ntohs(s6->sin6_port);
#endif
	} else {
		free(buf);
		return -1;	/* Unknown address family, Die Die Die! */
	}
	
	/*
	 *	Different address families should never happen.
	 */
	if (src.ss_family != dst.ss_family) {
		free(buf);
		return -1;
	}

	/*
	 *	Tell the caller about the data
	 */
	*pbuf = buf;

	return data_len;
}


/*
 *	Encode a packet.
 */
int rad_encode(RADIUS_PACKET *packet, const RADIUS_PACKET *original,
	       const char *secret)
{
	radius_packet_t	*hdr;
	uint32_t	lvalue;
	uint8_t	        *ptr, *length_ptr, *vsa_length_ptr;
	uint8_t		digest[16];
	int		vendorcode, vendorpec;
	uint16_t	total_length;
	int		len, allowed;
	VALUE_PAIR	*reply;
	
	/*
	 *	For simplicity in the following logic, we allow
	 *	the attributes to "overflow" the 4k maximum
	 *	RADIUS packet size, by one attribute.
	 *
	 *	It's uint32_t, for alignment purposes.
	 */
	uint32_t	data[(MAX_PACKET_LEN + 256) / 4];

	/*
	 *	Double-check some things based on packet code.
	 */
	switch (packet->code) {
	case PW_AUTHENTICATION_ACK:
	case PW_AUTHENTICATION_REJECT:
	case PW_ACCESS_CHALLENGE:
		if (!original) {
			librad_log("ERROR: Cannot sign response packet without a request packet.");
			return -1;
		}
		break;
		
		/*
		 *	These packet vectors start off as all zero.
		 */
	case PW_ACCOUNTING_REQUEST:
	case PW_DISCONNECT_REQUEST:
		memset(packet->vector, 0, sizeof(packet->vector));
		break;
		
	default:
		break;
	}
		
	/*
	 *	Use memory on the stack, until we know how
	 *	large the packet will be.
	 */
	hdr = (radius_packet_t *) data;
	
	/*
	 *	Build standard header
	 */
	hdr->code = packet->code;
	hdr->id = packet->id;
	
	memcpy(hdr->vector, packet->vector, sizeof(hdr->vector));

	total_length = AUTH_HDR_LEN;
	packet->verified = 0;
	
	/*
	 *	Load up the configuration values for the user
	 */
	ptr = hdr->data;
	vendorcode = 0;
	vendorpec = 0;
	vsa_length_ptr = NULL;

	/*
	 *	FIXME: Loop twice over the reply list.  The first time,
	 *	calculate the total length of data.  The second time,
	 *	allocate the memory, and fill in the VP's.
	 *
	 *	Hmm... this may be slower than just doing a small
	 *	memcpy.
	 */
	
	/*
	 *	Loop over the reply attributes for the packet.
	 */
	for (reply = packet->vps; reply; reply = reply->next) {
		/*
		 *	Ignore non-wire attributes
		 */
		if ((VENDOR(reply->attribute) == 0) &&
		    ((reply->attribute & 0xFFFF) > 0xff)) {
			continue;
		}
		
		/*
		 *	Check that the packet is no more than 4k in
		 *	size, AFTER over-flowing the 4k boundary.
		 *	Note that the 'data' buffer, above, is one
		 *	attribute longer than necessary, in order to
		 *	permit this overflow.
		 */
		if (total_length > MAX_PACKET_LEN) {
			librad_log("ERROR: Too many attributes for packet, result is larger than RFC maximum of 4k");
			return -1;
		}
		
		/*
		 *	Set the Message-Authenticator to the correct
		 *	length and initial value.
		 */
		if (reply->attribute == PW_MESSAGE_AUTHENTICATOR) {
			reply->length = AUTH_VECTOR_LEN;
			memset(reply->strvalue, 0, AUTH_VECTOR_LEN);
			packet->verified = total_length; /* HACK! */
		}
		
		/*
		 *	Print out ONLY the attributes which
		 *	we're sending over the wire, and print
		 *	them out BEFORE they're encrypted.
		 */
		debug_pair(reply);
		
		/*
		 *	We have a different vendor.  Re-set
		 *	the vendor codes.
		 */
		if (vendorcode != VENDOR(reply->attribute)) {
			vendorcode = 0;
			vendorpec = 0;
			vsa_length_ptr = NULL;
		}
		
		/*
		 *	If the Vendor-Specific attribute is getting
		 *	full, then create a new VSA attribute
		 *
		 *	FIXME: Multiple VSA's per Vendor-Specific
		 *	SHOULD be configurable.  When that's done, the
		 *	(1), below, can be changed to point to a
		 *	configuration variable which is set TRUE if
		 *	the NAS cannot understand multiple VSA's per
		 *	Vendor-Specific
		 */
		if ((1) || /* ALWAYS create a new Vendor-Specific */
		    (vsa_length_ptr &&
		     (reply->length + *vsa_length_ptr) >= MAX_STRING_LEN)) {
			vendorcode = 0;
			vendorpec = 0;
			vsa_length_ptr = NULL;
		}
		
		/*
		 *	Maybe we have the start of a set of
		 *	(possibly many) VSA attributes from
		 *	one vendor.  Set a global VSA wrapper
		 */
		if ((vendorcode == 0) &&
		    ((vendorcode = VENDOR(reply->attribute)) != 0)) {
			vendorpec  = dict_vendorpec(vendorcode);
			
			/*
			 *	This is a potentially bad error...
			 *	we can't find the vendor ID!
			 */
			if (vendorpec == 0) {
				/* FIXME: log an error */
				continue;
			}
			
			/*
			 *	Build a VSA header.
			 */
			*ptr++ = PW_VENDOR_SPECIFIC;
			vsa_length_ptr = ptr;
			*ptr++ = 6;
			lvalue = htonl(vendorpec);
			memcpy(ptr, &lvalue, 4);
			ptr += 4;
			total_length += 6;
		}
		
		if (vendorpec == VENDORPEC_USR) {
			lvalue = htonl(reply->attribute & 0xFFFF);
			memcpy(ptr, &lvalue, 4);
			
			length_ptr = vsa_length_ptr;
			
			total_length += 4;
			*length_ptr  += 4;
			ptr          += 4;
			
			/*
			 *	Each USR-style attribute gets it's own
			 *	VSA wrapper, so we re-set the vendor
			 *	specific information.
			 */
			vendorcode = 0;
			vendorpec = 0;
			vsa_length_ptr = NULL;
			
		} else if (vendorpec == VENDORPEC_LUCENT) {
			/*
			 *	16-bit attribute, 8-bit length
			 */
			*ptr++ = ((reply->attribute >> 8) & 0xFF);
			*ptr++ = (reply->attribute & 0xFF);
			length_ptr = ptr;
			if (vsa_length_ptr) *vsa_length_ptr += 3;
			*ptr++ = 3;
			total_length += 3;
		} else {
			/*
			 *	All other attributes are encoded as
			 *	per the RFC.
			 */
			*ptr++ = (reply->attribute & 0xFF);
			length_ptr = ptr;
			if (vsa_length_ptr) *vsa_length_ptr += 2;
			*ptr++ = 2;
			total_length += 2;
		}

		switch(reply->type) {
		case PW_TYPE_STRING:
		case PW_TYPE_OCTETS:
			/*
			 *  Encrypt the various password styles
			 */
			switch (reply->flags.encrypt) {
			default:
				break;
				
			case FLAG_ENCRYPT_USER_PASSWORD:
				rad_pwencode((char *)reply->strvalue,
					     &(reply->length),
					     (const char *)secret,
					     (const char *)packet->vector);
				break;
				
			case FLAG_ENCRYPT_TUNNEL_PASSWORD:
				if (!original) {
					librad_log("ERROR: No request packet, cannot encrypt Tunnel-Password attribute in the reply.");
					return -1;
				}
				rad_tunnel_pwencode(reply->strvalue,
						    &(reply->length),
						    secret,
						    original->vector);
				break;
				
				
			case FLAG_ENCRYPT_ASCEND_SECRET:
				make_secret(digest, packet->vector,
					    secret, reply->strvalue);
				memcpy(reply->strvalue, digest, AUTH_VECTOR_LEN );
				reply->length = AUTH_VECTOR_LEN;
				break;
			} /* switch over encryption flags */
			/* FALL-THROUGH */
			
			/*
			 *	None of these can have "encrypt"
			 *	flags, so we skip them.
			 */				  
		case PW_TYPE_IFID:
		case PW_TYPE_IPV6ADDR:
		case PW_TYPE_IPV6PREFIX:
			
			/*
			 *	Ascend binary attributes are
			 *	stored internally in binary form.
			 */
		case PW_TYPE_ABINARY:
			len = reply->length;
			
			/*
			 *    Set the TAG at the beginning of the
			 *    string if tagged.  If tag value is not
			 *    valid for tagged attribute, make it 0x00
			 *    per RFC 2868.  -cparker
			 */
			if (reply->flags.has_tag) {
				if (TAG_VALID(reply->flags.tag)) {
					len++;
					*ptr++ = reply->flags.tag;
					
				} else if (reply->flags.encrypt == FLAG_ENCRYPT_TUNNEL_PASSWORD) {
					/*
					 *  Tunnel passwords REQUIRE a
					 *  tag, even if we don't have
					 *  a valid tag.
					 */
					len++;
					*ptr++ = 0x00;
				} /* else don't write a tag */
			} /* else the attribute doesn't have a tag */
			
			/*
			 *	Ensure we don't go too far.  The
			 *	'length' of the attribute may be
			 *	0..255, minus whatever octets are used
			 *	in the attribute header.
			 */
			allowed = 255;
			if (vsa_length_ptr) {
				allowed -= *vsa_length_ptr;
			} else {
				allowed -= *length_ptr;
			}
			
			if (len > allowed) {
				len = allowed;
			}
			
			*length_ptr += len;
			if (vsa_length_ptr) *vsa_length_ptr += len;
			/*
			 *  If we have tagged attributes we can't
			 *  assume that len == reply->length.  Use
			 *  reply->length for copying the string data
			 *  into the packet.  Use len for the true
			 *  length of the string+tags.
			 */
			memcpy(ptr, reply->strvalue, reply->length);
			ptr += reply->length;
			total_length += len;
			break;
			
		case PW_TYPE_INTEGER:
		case PW_TYPE_IPADDR:
			*length_ptr += 4;
			if (vsa_length_ptr) *vsa_length_ptr += 4;
			
			if (reply->type == PW_TYPE_INTEGER ) {
				/*  If tagged, the tag becomes the MSB of the value */
				if(reply->flags.has_tag) {
					/*  Tag must be ( 0x01 -> 0x1F ) OR 0x00  */
					if(!TAG_VALID(reply->flags.tag)) {
						reply->flags.tag = 0x00;
					}
					lvalue = htonl((reply->lvalue & 0xffffff) |
						       ((reply->flags.tag & 0xff) << 24));
				} else {
					lvalue = htonl(reply->lvalue);
				}
			} else {
				/*
				 *	IP address is already in
				 *	network byte order.
				 */
				lvalue = reply->lvalue;
			}
			memcpy(ptr, &lvalue, 4);
			ptr += 4;
			total_length += 4;
			break;
			
			/*
			 *  There are no tagged date attributes.
			 */
		case PW_TYPE_DATE:
			*length_ptr += 4;
			if (vsa_length_ptr) *vsa_length_ptr += 4;
			
			lvalue = htonl(reply->lvalue);
			memcpy(ptr, &lvalue, 4);
			ptr += 4;
			total_length += 4;
			break;
		default:
			break;
		}
	} /* done looping over all attributes */
	
	/*
	 *	Fill in the rest of the fields, and copy the data over
	 *	from the local stack to the newly allocated memory.
	 *
	 *	Yes, all this 'memcpy' is slow, but it means
	 *	that we only allocate the minimum amount of
	 *	memory for a request.
	 */
	packet->data_len = total_length;
	packet->data = (uint8_t *) malloc(packet->data_len);
	if (!packet->data) {
		librad_log("Out of memory");
		return -1;
	}

	memcpy(packet->data, data, packet->data_len);
	hdr = (radius_packet_t *) packet->data;
	
	total_length = htons(total_length);
	memcpy(hdr->length, &total_length, sizeof(total_length));

	return 0;
}


/*
 *	Sign a previously encoded packet.
 */
int rad_sign(RADIUS_PACKET *packet, const RADIUS_PACKET *original,
	     const char *secret)
{
	radius_packet_t	*hdr = (radius_packet_t *)packet->data;

	/*
	 *	It wasn't assigned an Id, this is bad!
	 */
	if (packet->id < 0) {
		librad_log("ERROR: RADIUS packets must be assigned an Id.");
		return -1;
	}

	if (!packet->data || (packet->data_len < AUTH_HDR_LEN) ||
	    (packet->verified < 0)) {
		librad_log("ERROR: You must call rad_encode() before rad_sign()");
		return -1;
	}

	/*
	 *	If there's a Message-Authenticator, update it
	 *	now, BEFORE updating the authentication vector.
	 *
	 *	This is a hack...
	 */
	if (packet->verified > 0) {
		uint8_t calc_auth_vector[AUTH_VECTOR_LEN];
		
		switch (packet->code) {
		case PW_ACCOUNTING_REQUEST:
		case PW_ACCOUNTING_RESPONSE:
		case PW_DISCONNECT_REQUEST:
		case PW_DISCONNECT_ACK:
		case PW_DISCONNECT_NAK:
		case PW_COF_REQUEST:
		case PW_COF_ACK:
		case PW_COF_NAK:
			memset(hdr->vector, 0, AUTH_VECTOR_LEN);
			break;

		case PW_AUTHENTICATION_ACK:
		case PW_AUTHENTICATION_REJECT:
		case PW_ACCESS_CHALLENGE:
			if (!original) {
				librad_log("ERROR: Cannot sign response packet without a request packet.");
				return -1;
			}
			memcpy(hdr->vector, original->vector,
			       AUTH_VECTOR_LEN);
			break;

		default:	/* others have vector already set to zero */
			break;
			
		}
		
		/*
		 *	Set the authentication vector to zero,
		 *	calculate the signature, and put it
		 *	into the Message-Authenticator
		 *	attribute.
		 */
		lrad_hmac_md5(packet->data, packet->data_len,
			      secret, strlen(secret),
			      calc_auth_vector);
		memcpy(packet->data + packet->verified + 2,
		       calc_auth_vector, AUTH_VECTOR_LEN);
		
		/*
		 *	Copy the original request vector back
		 *	to the raw packet.
		 */
		memcpy(hdr->vector, packet->vector, AUTH_VECTOR_LEN);
	}
	
	/*
	 *	Switch over the packet code, deciding how to
	 *	sign the packet.
	 */
	switch (packet->code) {
		/*
		 *	Request packets are not signed, bur
		 *	have a random authentication vector.
		 */
	case PW_AUTHENTICATION_REQUEST:
	case PW_STATUS_SERVER:
		break;
		
		/*
		 *	Reply packets are signed with the
		 *	authentication vector of the request.
		 */
	default:
		{
			uint8_t digest[16];
			
			MD5_CTX	context;
			MD5Init(&context);
			MD5Update(&context, packet->data, packet->data_len);
			MD5Update(&context, secret, strlen(secret));
			MD5Final(digest, &context);
			
			memcpy(hdr->vector, digest, AUTH_VECTOR_LEN);
			memcpy(packet->vector, digest, AUTH_VECTOR_LEN);
			break;
		}
	}/* switch over packet codes */

	return 0;
}

/*
 *	Reply to the request.  Also attach
 *	reply attribute value pairs and any user message provided.
 */
int rad_send(RADIUS_PACKET *packet, const RADIUS_PACKET *original,
	     const char *secret)
{
	VALUE_PAIR		*reply;
	const char		*what;
	char			ip_buffer[128];

	/*
	 *	Maybe it's a fake packet.  Don't send it.
	 */
	if (!packet || (packet->sockfd < 0)) {
		return 0;
	}

	if ((packet->code > 0) && (packet->code < 52)) {
		what = packet_codes[packet->code];
	} else {
		what = "Reply";
	}

	/*
	 *  First time through, allocate room for the packet
	 */
	if (!packet->data) {
		DEBUG("Sending %s of id %d to %s port %d\n",
		      what, packet->id,
		      inet_ntop(packet->dst_ipaddr.af,
				&packet->dst_ipaddr.ipaddr,
				ip_buffer, sizeof(ip_buffer)),
			packet->dst_port);
		
		/*
		 *	Encode the packet.
		 */
		if (rad_encode(packet, original, secret) < 0) {
			return -1;
		}
		
		/*
		 *	Re-sign it, including updating the
		 *	Message-Authenticator.
		 */
		if (rad_sign(packet, original, secret) < 0) {
			return -1;
		}

		/*
		 *	If packet->data points to data, then we print out
		 *	the VP list again only for debugging.
		 */
	} else if (librad_debug) {
	  	DEBUG("Re-sending %s of id %d to %s port %d\n", what, packet->id,
		      inet_ntop(packet->dst_ipaddr.af,
				&packet->dst_ipaddr.ipaddr,
				ip_buffer, sizeof(ip_buffer)),
		      packet->dst_port);

		for (reply = packet->vps; reply; reply = reply->next) {
			/* FIXME: ignore attributes > 0xff */
			debug_pair(reply);
		}
	}

	/*
	 *	And send it on it's way.
	 */
	return rad_sendto(packet->sockfd, packet->data, packet->data_len, 0,
			  &packet->src_ipaddr, &packet->dst_ipaddr,
			  packet->dst_port);
}


/*
 *	Validates the requesting client NAS.  Calculates the
 *	signature based on the clients private key.
 */
static int calc_acctdigest(RADIUS_PACKET *packet, const char *secret)
{
	u_char		digest[AUTH_VECTOR_LEN];
	MD5_CTX		context;

	/*
	 *	Older clients have the authentication vector set to
	 *	all zeros. Return `1' in that case.
	 */
	memset(digest, 0, sizeof(digest));
	if (memcmp(packet->vector, digest, AUTH_VECTOR_LEN) == 0) {
		packet->verified = 1;
		return 1;
	}

	/*
	 *	Zero out the auth_vector in the received packet.
	 *	Then append the shared secret to the received packet,
	 *	and calculate the MD5 sum. This must be the same
	 *	as the original MD5 sum (packet->vector).
	 */
	memset(packet->data + 4, 0, AUTH_VECTOR_LEN);

	/*
	 *  MD5(packet + secret);
	 */
	MD5Init(&context);
	MD5Update(&context, packet->data, packet->data_len);
	MD5Update(&context, secret, strlen(secret));
	MD5Final(digest, &context);

	/*
	 *	Return 0 if OK, 2 if not OK.
	 */
	packet->verified =
	memcmp(digest, packet->vector, AUTH_VECTOR_LEN) ? 2 : 0;

	return packet->verified;
}

/*
 *	Validates the requesting client NAS.  Calculates the
 *	signature based on the clients private key.
 */
static int calc_replydigest(RADIUS_PACKET *packet, RADIUS_PACKET *original,
			    const char *secret)
{
	uint8_t		calc_digest[AUTH_VECTOR_LEN];
	MD5_CTX		context;

	/*
	 *	Very bad!
	 */
	if (original == NULL) {
		return 3;
	}

	/*
	 *  Copy the original vector in place.
	 */
	memcpy(packet->data + 4, original->vector, AUTH_VECTOR_LEN);

	/*
	 *  MD5(packet + secret);
	 */
	MD5Init(&context);
	MD5Update(&context, packet->data, packet->data_len);
	MD5Update(&context, secret, strlen(secret));
	MD5Final(calc_digest, &context);

	/*
	 *  Copy the packet's vector back to the packet.
	 */
	memcpy(packet->data + 4, packet->vector, AUTH_VECTOR_LEN);

	/*
	 *	Return 0 if OK, 2 if not OK.
	 */
	packet->verified =
		memcmp(packet->vector, calc_digest, AUTH_VECTOR_LEN) ? 2 : 0;
	return packet->verified;
}

/*
 *	Receive UDP client requests, and fill in
 *	the basics of a RADIUS_PACKET structure.
 */
RADIUS_PACKET *rad_recv(int fd)
{
	RADIUS_PACKET		*packet;
	uint8_t			*attr;
	int			totallen;
	int			count;
	radius_packet_t		*hdr;
	char			host_ipaddr[128];
	int			seen_eap;
	int			num_attributes;

	/*
	 *	Allocate the new request data structure
	 */
	if ((packet = malloc(sizeof(*packet))) == NULL) {
		librad_log("out of memory");
		return NULL;
	}
	memset(packet, 0, sizeof(*packet));

	packet->data_len = rad_recvfrom(fd, &packet->data, 0,
					&packet->src_ipaddr, &packet->src_port,
					&packet->dst_ipaddr, &packet->dst_port);

	/*
	 *	Check for socket errors.
	 */
	if (packet->data_len < 0) {
		librad_log("Error receiving packet: %s", strerror(errno));
		/* packet->data is NULL */
		free(packet);
		return NULL;
	}

	/*
	 *	Fill IP header fields.  We need these for the error
	 *	messages which may come later.
	 */
	packet->sockfd = fd;

	/*
	 *	FIXME: Do even more filtering by only permitting
	 *	certain IP's.  The problem is that we don't know
	 *	how to do this properly for all possible clients...
	 */

	/*
	 *	Explicitely set the VP list to empty.
	 */
	packet->vps = NULL;

	/*
	 *	Check for packets smaller than the packet header.
	 *
	 *	RFC 2865, Section 3., subsection 'length' says:
	 *
	 *	"The minimum length is 20 ..."
	 */
	if (packet->data_len < AUTH_HDR_LEN) {
		librad_log("WARNING: Malformed RADIUS packet from host %s: too short (received %d < minimum %d)",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)),
			   packet->data_len, AUTH_HDR_LEN);
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	RFC 2865, Section 3., subsection 'length' says:
	 *
	 *	" ... and maximum length is 4096."
	 */
	if (packet->data_len > MAX_PACKET_LEN) {
		librad_log("WARNING: Malformed RADIUS packet from host %s: too long (received %d > maximum %d)",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)),
			   packet->data_len, MAX_PACKET_LEN);
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	Check for packets with mismatched size.
	 *	i.e. We've received 128 bytes, and the packet header
	 *	says it's 256 bytes long.
	 */
	totallen = (packet->data[2] << 8) | packet->data[3];
	hdr = (radius_packet_t *)packet->data;

	/*
	 *	Code of 0 is not understood.
	 *	Code of 16 or greate is not understood.
	 */
	if ((hdr->code == 0) ||
	    (hdr->code >= 52)) {
		librad_log("WARNING: Bad RADIUS packet from host %s: unknown packet code %d",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)),
			   hdr->code);
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	Repeat the length checks.  This time, instead of
	 *	looking at the data we received, look at the value
	 *	of the 'length' field inside of the packet.
	 *
	 *	Check for packets smaller than the packet header.
	 *
	 *	RFC 2865, Section 3., subsection 'length' says:
	 *
	 *	"The minimum length is 20 ..."
	 */
	if (totallen < AUTH_HDR_LEN) {
		librad_log("WARNING: Malformed RADIUS packet from host %s: too short (length %d < minimum %d)",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)),
			   totallen, AUTH_HDR_LEN);
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	And again, for the value of the 'length' field.
	 *
	 *	RFC 2865, Section 3., subsection 'length' says:
	 *
	 *	" ... and maximum length is 4096."
	 */
	if (totallen > MAX_PACKET_LEN) {
		librad_log("WARNING: Malformed RADIUS packet from host %s: too long (length %d > maximum %d)",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)),
			   totallen, MAX_PACKET_LEN);
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	RFC 2865, Section 3., subsection 'length' says:
	 *
	 *	"If the packet is shorter than the Length field
	 *	indicates, it MUST be silently discarded."
	 *
	 *	i.e. No response to the NAS.
	 */
	if (packet->data_len < totallen) {
		librad_log("WARNING: Malformed RADIUS packet from host %s: received %d octets, packet length says %d",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)),
			   packet->data_len, totallen);
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	RFC 2865, Section 3., subsection 'length' says:
	 *
	 *	"Octets outside the range of the Length field MUST be
	 *	treated as padding and ignored on reception."
	 */
	if (packet->data_len > totallen) {
		/*
		 *	We're shortening the packet below, but just
		 *	to be paranoid, zero out the extra data.
		 */
		memset(packet->data + totallen, 0, packet->data_len - totallen);
		packet->data_len = totallen;
	}

	/*
	 *	Walk through the packet's attributes, ensuring that
	 *	they add up EXACTLY to the size of the packet.
	 *
	 *	If they don't, then the attributes either under-fill
	 *	or over-fill the packet.  Any parsing of the packet
	 *	is impossible, and will result in unknown side effects.
	 *
	 *	This would ONLY happen with buggy RADIUS implementations,
	 *	or with an intentional attack.  Either way, we do NOT want
	 *	to be vulnerable to this problem.
	 */
	attr = hdr->data;
	count = totallen - AUTH_HDR_LEN;
	seen_eap = 0;
	num_attributes = 0;

	while (count > 0) {
		/*
		 *	Attribute number zero is NOT defined.
		 */
		if (attr[0] == 0) {
			librad_log("WARNING: Malformed RADIUS packet from host %s: Invalid attribute 0",
				   inet_ntop(packet->src_ipaddr.af,
					     &packet->src_ipaddr.ipaddr,
					     host_ipaddr, sizeof(host_ipaddr)));
			rad_free(&packet);
			return NULL;
		}

		/*
		 *	Attributes are at LEAST as long as the ID & length
		 *	fields.  Anything shorter is an invalid attribute.
		 */
       		if (attr[1] < 2) {
			librad_log("WARNING: Malformed RADIUS packet from host %s: attribute %d too short",
				   inet_ntop(packet->src_ipaddr.af,
					     &packet->src_ipaddr.ipaddr,
					     host_ipaddr, sizeof(host_ipaddr)),
				   attr[0]);
			rad_free(&packet);
			return NULL;
		}

		/*
		 *	Sanity check the attributes for length.
		 */
		switch (attr[0]) {
		default:	/* don't do anything by default */
			break;

		case PW_EAP_MESSAGE:
			seen_eap |= PW_EAP_MESSAGE;
			break;

		case PW_MESSAGE_AUTHENTICATOR:
			if (attr[1] != 2 + AUTH_VECTOR_LEN) {
				librad_log("WARNING: Malformed RADIUS packet from host %s: Message-Authenticator has invalid length %d",
					   inet_ntop(packet->src_ipaddr.af,
						     &packet->src_ipaddr.ipaddr,
						     host_ipaddr, sizeof(host_ipaddr)),
					   attr[1] - 2);
				rad_free(&packet);
				return NULL;
			}
			seen_eap |= PW_MESSAGE_AUTHENTICATOR;
			break;

		case PW_VENDOR_SPECIFIC:
			if (attr[1] <= 6) {
				librad_log("WARNING: Malformed RADIUS packet from host %s: Vendor-Specific has invalid length %d",
					   inet_ntop(packet->src_ipaddr.af,
						     &packet->src_ipaddr.ipaddr,
						     host_ipaddr, sizeof(host_ipaddr)),
					   attr[1] - 2);
				rad_free(&packet);
				return NULL;
			}

			/*
			 *	Don't allow VSA's with vendor zero.
			 */
			if ((attr[2] == 0) && (attr[3] == 0) &&
			    (attr[4] == 0) && (attr[5] == 0)) {
				librad_log("WARNING: Malformed RADIUS packet from host %s: Vendor-Specific has vendor ID of zero",
					   inet_ntop(packet->src_ipaddr.af,
						     &packet->src_ipaddr.ipaddr,
						     host_ipaddr, sizeof(host_ipaddr)));
				rad_free(&packet);
				return NULL;
			}

			/*
			 *	Don't look at the contents of VSA's,
			 *	too many vendors have non-standard
			 *	formats.
			 */
			break;
		}

		/*
		 *	FIXME: Look up the base 255 attributes in the
		 *	dictionary, and switch over their type.  For
		 *	integer/date/ip, the attribute length SHOULD
		 *	be 6.
		 */
		count -= attr[1];	/* grab the attribute length */
		attr += attr[1];
		num_attributes++;	/* seen one more attribute */
	}

	/*
	 *	If the attributes add up to a packet, it's allowed.
	 *
	 *	If not, we complain, and throw the packet away.
	 */
	if (count != 0) {
		librad_log("WARNING: Malformed RADIUS packet from host %s: packet attributes do NOT exactly fill the packet",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)));
		rad_free(&packet);
		return NULL;
	}

	/*
	 *	If we're configured to look for a maximum number of
	 *	attributes, and we've seen more than that maximum,
	 *	then throw the packet away, as a possible DoS.
	 */
	if ((librad_max_attributes > 0) &&
	    (num_attributes > librad_max_attributes)) {
		librad_log("WARNING: Possible DoS attack from host %s: Too many attributes in request (received %d, max %d are allowed).",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)),
			   num_attributes, librad_max_attributes);
		rad_free(&packet);
		return NULL;
	}

	/*
	 * 	http://www.freeradius.org/rfc/rfc2869.html#EAP-Message
	 *
	 *	A packet with an EAP-Message attribute MUST also have
	 *	a Message-Authenticator attribute.
	 *
	 *	A Message-Authenticator all by itself is OK, though.
	 */
	if (seen_eap &&
	    (seen_eap != PW_MESSAGE_AUTHENTICATOR) &&
	    (seen_eap != (PW_EAP_MESSAGE | PW_MESSAGE_AUTHENTICATOR))) {
		librad_log("WARNING: Insecure packet from host %s:  Received EAP-Message with no Message-Authenticator.",
			   inet_ntop(packet->src_ipaddr.af,
				     &packet->src_ipaddr.ipaddr,
				     host_ipaddr, sizeof(host_ipaddr)));
		rad_free(&packet);
		return NULL;
	}

	if (librad_debug) {
		if ((hdr->code > 0) && (hdr->code < 52)) {
			printf("rad_recv: %s packet from host %s port %d",
			       packet_codes[hdr->code],
			       inet_ntop(packet->src_ipaddr.af,
					 &packet->src_ipaddr.ipaddr,
					 host_ipaddr, sizeof(host_ipaddr)),
			       packet->src_port);
		} else {
			printf("rad_recv: Packet from host %s port %d code=%d",
			       inet_ntop(packet->src_ipaddr.af,
					 &packet->src_ipaddr.ipaddr,
					 host_ipaddr, sizeof(host_ipaddr)),
			       packet->src_port,
			       hdr->code);
		}
		printf(", id=%d, length=%d\n", hdr->id, totallen);
	}

	/*
	 *	Fill RADIUS header fields
	 */
	packet->code = hdr->code;
	packet->id = hdr->id;
	memcpy(packet->vector, hdr->vector, AUTH_VECTOR_LEN);

	return packet;
}


/*
 *	Verify the signature of a packet.
 */
int rad_verify(RADIUS_PACKET *packet, RADIUS_PACKET *original,
	       const char *secret)
{
	uint8_t			*ptr;
	int			length;
	int			attrlen;

	if (!packet || !packet->data) return -1;

	/*
	 *	Before we allocate memory for the attributes, do more
	 *	sanity checking.
	 */
	ptr = packet->data + AUTH_HDR_LEN;
	length = packet->data_len - AUTH_HDR_LEN;
	while (length > 0) {
		uint8_t	msg_auth_vector[AUTH_VECTOR_LEN];
		uint8_t calc_auth_vector[AUTH_VECTOR_LEN];

		attrlen = ptr[1];

		switch (ptr[0]) {
		default:	/* don't do anything. */
			break;

			/*
			 *	Note that more than one Message-Authenticator
			 *	attribute is invalid.
			 */
		case PW_MESSAGE_AUTHENTICATOR:
			memcpy(msg_auth_vector, &ptr[2], sizeof(msg_auth_vector));
			memset(&ptr[2], 0, AUTH_VECTOR_LEN);

			switch (packet->code) {
			default:
				break;

			case PW_ACCOUNTING_REQUEST:
			case PW_ACCOUNTING_RESPONSE:
			case PW_DISCONNECT_REQUEST:
			case PW_DISCONNECT_ACK:
			case PW_DISCONNECT_NAK:
			case PW_COF_REQUEST:
			case PW_COF_ACK:
			case PW_COF_NAK:
			  	memset(packet->data + 4, 0, AUTH_VECTOR_LEN);
				break;

			case PW_AUTHENTICATION_ACK:
			case PW_AUTHENTICATION_REJECT:
			case PW_ACCESS_CHALLENGE:
				if (!original) {
					librad_log("ERROR: Cannot validate Message-Authenticator in response packet without a request packet.");
					return -1;
				}
				memcpy(packet->data + 4, original->vector, AUTH_VECTOR_LEN);
				break;
			}

			lrad_hmac_md5(packet->data, packet->data_len,
				      secret, strlen(secret), calc_auth_vector);
			if (memcmp(calc_auth_vector, msg_auth_vector,
				   sizeof(calc_auth_vector)) != 0) {
				char buffer[32];
				librad_log("Received packet from %s with invalid Message-Authenticator!  (Shared secret is incorrect.)",
					   inet_ntop(packet->src_ipaddr.af,
						     &packet->src_ipaddr.ipaddr,
						     buffer, sizeof(buffer)));
				/* Silently drop packet, according to RFC 3579 */
				return -2;
			} /* else the message authenticator was good */

			/*
			 *	Reinitialize Authenticators.
			 */
			memcpy(&ptr[2], msg_auth_vector, AUTH_VECTOR_LEN);
			memcpy(packet->data + 4, packet->vector, AUTH_VECTOR_LEN);
			break;
		} /* switch over the attributes */

		ptr += attrlen;
		length -= attrlen;
	} /* loop over the packet, sanity checking the attributes */

	/*
	 *	Calculate and/or verify digest.
	 */
	switch(packet->code) {
		int rcode;

		case PW_AUTHENTICATION_REQUEST:
		case PW_STATUS_SERVER:
		case PW_DISCONNECT_REQUEST:
			/*
			 *	The authentication vector is random
			 *	nonsense, invented by the client.
			 */
			break;

		case PW_ACCOUNTING_REQUEST:
			if (calc_acctdigest(packet, secret) > 1) {
				char buffer[32];
				librad_log("Received Accounting-Request packet "
					   "from %s with invalid signature!  (Shared secret is incorrect.)",
					   inet_ntop(packet->src_ipaddr.af,
						     &packet->src_ipaddr.ipaddr,
						     buffer, sizeof(buffer)));
				return -1;
			}
			break;

			/* Verify the reply digest */
		case PW_AUTHENTICATION_ACK:
		case PW_AUTHENTICATION_REJECT:
		case PW_ACCOUNTING_RESPONSE:
			rcode = calc_replydigest(packet, original, secret);
			if (rcode > 1) {
				char buffer[32];
				librad_log("Received %s packet "
					   "from client %s port %d with invalid signature (err=%d)!  (Shared secret is incorrect.)",
					   packet_codes[packet->code],
					   inet_ntop(packet->src_ipaddr.af,
						     &packet->src_ipaddr.ipaddr,
						     buffer, sizeof(buffer)),
					   packet->src_port,
					   rcode);
				return -1;
			}
		  break;
	}

	return 0;
}


/*
 *	Calculate/check digest, and decode radius attributes.
 *	Returns:
 *	-1 on decoding error
 *	-2 if decoding error implies the message should be silently dropped
 *	0 on success
 */
int rad_decode(RADIUS_PACKET *packet, RADIUS_PACKET *original,
	       const char *secret)
{
	uint32_t		lvalue;
	uint32_t		vendorcode;
	VALUE_PAIR		**tail;
	VALUE_PAIR		*pair;
	uint8_t			*ptr;
	int			length;
	int			attribute;
	int			attrlen;
	int			vendorlen;
	radius_packet_t		*hdr;

	if (rad_verify(packet, original, secret) < 0) return -1;

	/*
	 *	Extract attribute-value pairs
	 */
	hdr = (radius_packet_t *)packet->data;
	ptr = hdr->data;
	length = packet->data_len - AUTH_HDR_LEN;

	/*
	 *	There may be VP's already in the packet.  Don't
	 *	destroy them.
	 */
	for (tail = &packet->vps; *tail != NULL; tail = &((*tail)->next)) {
		/* nothing */
	}

	vendorcode = 0;
	vendorlen  = 0;

	while (length > 0) {
		if (vendorlen > 0) {
			attribute = *ptr++ | (vendorcode << 16);
			attrlen   = *ptr++;
		} else {
			attribute = *ptr++;
			attrlen   = *ptr++;
		}

		attrlen -= 2;
		length  -= 2;

		/*
		 *	This could be a Vendor-Specific attribute.
		 */
		if ((vendorlen <= 0) &&
		    (attribute == PW_VENDOR_SPECIFIC)) {
			/*
			 *	attrlen was checked to be >= 6, in rad_recv
			 */
			memcpy(&lvalue, ptr, 4);
			vendorcode = ntohl(lvalue);

			/*
			 *	This is an implementation issue.
			 *	We currently pack vendor into the upper
			 *	16 bits of a 32-bit attribute number,
			 *	so we can't handle vendor numbers larger
			 *	than 16 bits.
			 */
			if (vendorcode > 65535) goto create_pair;

			/*
			 *	vendorcode was checked to be non-zero
			 *	above, in rad_recv.
			 */

			/*
			 *	USR & Lucent are special, so everything
			 *	else is normal.
			 */
			if ((vendorcode != VENDORPEC_USR) &&
			    (vendorcode != VENDORPEC_LUCENT)) {
				int	sublen;
				uint8_t	*subptr;

				/*
				 *	First, check to see if the
				 *	sub-attributes fill the VSA,
				 *	as defined by the RFC.  If
				 *	not, then it's a vendor who
				 *	packs all of the information
				 *	into one nonsense attribute
				 */
				subptr = ptr + 4;
				sublen = attrlen - 4;
				
				while (sublen >= 2) {
					if (subptr[1] < 2) { /* too short */
						break;
					}
					
					if (subptr[1] > sublen) { /* too long */
						break;
					}
					
					sublen -= subptr[1]; /* just right */
					subptr += subptr[1];
				}

				/*
				 *	VSA's don't exactly fill the
				 *	attribute.  Make a nonsense
				 *	VSA.
				 */
				if (sublen != 0) goto create_pair;
				
				/*
				 *	If the attribute is RFC compatible,
				 *	then allow it as an RFC style VSA.
				 */
				ptr += 4;
				vendorlen = attrlen - 4;
				attribute = *ptr++ | (vendorcode << 16);
				attrlen   = *ptr++;
				attrlen -= 2;
				length -= 6;

				/*
				 *	USR-style attributes are 4 octets,
				 *	with the upper 2 octets being zero.
				 *
				 *	The upper octets may not be zero,
				 *	but that then means we won't be
				 *	able to pack the vendor & attribute
				 *	into a 32-bit number, so we can't
				 *	handle it.
				 *
				 *
				 *	FIXME: Update the dictionaries so
				 *	that we key off of per-attribute
				 *	flags "4-octet", instead of hard
				 *	coding USR here.  This will also
				 *	let us send packets with other
				 *	vendors having 4-octet attributes.
				 */
			} else if ((vendorcode == VENDORPEC_USR) &&
				   ((ptr[4] == 0) && (ptr[5] == 0)) &&
				   (attrlen >= 8)) {
				DICT_ATTR *da;

				da = dict_attrbyvalue((vendorcode << 16) |
						      (ptr[6] << 8) |
						      ptr[7]);

				/*
				 *	See if it's in the dictionary.
				 *	If so, it's a valid USR style
				 *	attribute.  If not, it's not...
				 *
				 *	Don't touch 'attribute' until
				 *	we know what to do!
				 */
				if (da != NULL) {
					attribute = ((vendorcode << 16) |
						     (ptr[6] << 8) |
						     ptr[7]);
					ptr += 8;
					attrlen -= 8;
					length -= 8;
				}
			} else if ((vendorcode == VENDORPEC_LUCENT) &&
				   (attrlen >= 7) &&
				   ((ptr[6] + 4) == attrlen)) {
				attribute = ((vendorcode << 16) |
					     (ptr[4] << 8) |
					     ptr[5]);
				ptr += 7;
				attrlen -= 7;
				length -= 7;
			} /* else something went catastrophically wrong */
		} /* else it wasn't a VSA */

		/*
		 *	Create the attribute, setting the default type
		 *	to 'octects'.  If the type in the dictionary
		 *	is different, then the dictionary type will
		 *	over-ride this one.
		 */
	create_pair:
		if ((pair = paircreate(attribute, PW_TYPE_OCTETS)) == NULL) {
			pairfree(&packet->vps);
			librad_log("out of memory");
			return -1;
		}

		pair->length = attrlen;
		pair->operator = T_OP_EQ;
		pair->next = NULL;

		switch (pair->type) {

			/*
			 *	The attribute may be zero length,
			 *	or it may have a tag, and then no data...
			 */
		case PW_TYPE_STRING:
			if (pair->flags.has_tag) {
				int offset = 0;

				/*
				 *	If there's sufficient room for
				 *	a tag, and the tag looks valid,
				 *	then use it.
				 */
				if ((pair->length > 0) &&
				    TAG_VALID_ZERO(*ptr)) {
					pair->flags.tag = *ptr;
					pair->length--;
					offset = 1;

					/*
					 *	If the leading tag
					 *	isn't valid, then it's
					 *	ignored for the tunnel
					 *	password attribute.
					 */
				} else if (pair->flags.encrypt == FLAG_ENCRYPT_TUNNEL_PASSWORD) {
					/*
					 * from RFC2868 - 3.5.  Tunnel-Password
					 * If the value of the Tag field is greater than
					 * 0x00 and less than or equal to 0x1F, it SHOULD
					 * be interpreted as indicating which tunnel
					 * (of several alternatives) this attribute pertains;
					 * otherwise, the Tag field SHOULD be ignored.
					 */
					pair->flags.tag = 0x00;
					if (pair->length > 0) pair->length--;
					offset = 1;
				} else {
				       pair->flags.tag = 0x00;
				}

				/*
				 *	pair->length MAY be zero here.
				 */
				memcpy(pair->strvalue, ptr + offset,
				       pair->length);
			} else {
			  /*
			   *	Ascend binary attributes never have a
			   *	tag, and neither do the 'octets' type.
			   */
			case PW_TYPE_ABINARY:
			case PW_TYPE_OCTETS:
				/* attrlen always < MAX_STRING_LEN */
				memcpy(pair->strvalue, ptr, attrlen);
			        pair->flags.tag = 0;
			}

			/*
			 *	Decrypt passwords here.
			 */
			switch (pair->flags.encrypt) {
			default:
				break;

				/*
				 *  User-Password
				 */
			case FLAG_ENCRYPT_USER_PASSWORD:
				if (original) {
					rad_pwdecode((char *)pair->strvalue,
						     pair->length, secret,
						     (char *)original->vector);
				} else {
					rad_pwdecode((char *)pair->strvalue,
						     pair->length, secret,
						     (char *)packet->vector);
				}
				if (pair->attribute == PW_USER_PASSWORD) {
					pair->length = strlen(pair->strvalue);
				}
				break;

				/*
				 *	Tunnel-Password's may go ONLY
				 *	in response packets.
				 */
			case FLAG_ENCRYPT_TUNNEL_PASSWORD:
				if (!original) {
					librad_log("ERROR: Tunnel-Password attribute in request: Cannot decrypt it.");
					free(pair);
					return -1;
				}
				if (rad_tunnel_pwdecode(pair->strvalue,
							&pair->length,
							secret,
							(char *)original->vector) < 0) {
					free(pair);
					return -1;
				}
				break;

				/*
				 *  Ascend-Send-Secret
				 *  Ascend-Receive-Secret
				 */
			case FLAG_ENCRYPT_ASCEND_SECRET:
				if (!original) {
					librad_log("ERROR: Ascend-Send-Secret attribute in request: Cannot decrypt it.");
					free(pair);
					return -1;
				} else {
					uint8_t my_digest[AUTH_VECTOR_LEN];
					make_secret(my_digest,
						    original->vector,
						    secret, ptr);
					memcpy(pair->strvalue, my_digest,
					       AUTH_VECTOR_LEN );
					pair->strvalue[AUTH_VECTOR_LEN] = '\0';
					pair->length = strlen(pair->strvalue);
				}
				break;
			} /* switch over encryption flags */
			break;	/* from octets/string/abinary */

		case PW_TYPE_INTEGER:
		case PW_TYPE_DATE:
		case PW_TYPE_IPADDR:
			/*
			 *	Check for RFC compliance.  If the
			 *	attribute isn't compliant, turn it
			 *	into a string of raw octets.
			 *
			 *	Also set the lvalue to something
			 *	which should never match anything.
			 */
			if (attrlen != 4) {
				pair->type = PW_TYPE_OCTETS;
				memcpy(pair->strvalue, ptr, attrlen);
				pair->lvalue = 0xbad1bad1;
				break;
			}

      			memcpy(&lvalue, ptr, 4);

			if (pair->type != PW_TYPE_IPADDR) {
				pair->lvalue = ntohl(lvalue);
			} else {
				lrad_ipaddr_t ipaddr;

				 /*
				  *  It's an IP address, keep it in network
				  *  byte order, and put the ASCII IP
				  *  address or host name into the string
				  *  value.
				  */
				pair->lvalue = lvalue;
				ipaddr.af = AF_INET;
				ipaddr.ipaddr.ip4addr.s_addr = lvalue;
				inet_ntop(AF_INET, &ipaddr.ipaddr,
					  pair->strvalue,
					  sizeof(pair->strvalue) - 1);
				
				
			}

			/*
			 *	Tagged attributes of type integer have
			 *	special treatment.
			 */
			if (pair->flags.has_tag &&
			    pair->type == PW_TYPE_INTEGER) {
			        pair->flags.tag = (pair->lvalue >> 24) & 0xff;
				pair->lvalue &= 0x00ffffff;
			}

			/*
			 *	Try to get the name for integer
			 *	attributes.
			 */
			if (pair->type == PW_TYPE_INTEGER) {
				DICT_VALUE *dval;
				dval = dict_valbyattr(pair->attribute,
						      pair->lvalue);
				if (dval) {
					strNcpy(pair->strvalue,
						dval->name,
						sizeof(pair->strvalue));
				}
			}
			break;

			/*
			 *	IPv6 interface ID is 8 octets long.
			 */
		case PW_TYPE_IFID:
			if (attrlen != 8)
				pair->type = PW_TYPE_OCTETS;
			memcpy(pair->strvalue, ptr, attrlen);
			break;

			/*
			 *	IPv6 addresses are 16 octets long
			 */
		case PW_TYPE_IPV6ADDR:
			if (attrlen != 16)
				pair->type = PW_TYPE_OCTETS;
			memcpy(pair->strvalue, ptr, attrlen);
			break;

			/*
			 *	IPv6 prefixes are 2 to 18 octets long.
			 *
			 *	RFC 3162: The first octet is unused.
			 *	The second is the length of the prefix
			 *	the rest are the prefix data.
			 *
			 *	The prefix length can have value 0 to 128.
			 */
		case PW_TYPE_IPV6PREFIX:
			if (attrlen < 2 || attrlen > 18)
				pair->type = PW_TYPE_OCTETS;
			if (attrlen >= 2) {
				if (ptr[1] > 128) {
					pair->type = PW_TYPE_OCTETS;
				}

				/*
				 *	FIXME: double-check that
				 *	(ptr[1] >> 3) matches attrlen + 2
				 */
			}
			memcpy(pair->strvalue, ptr, attrlen);
			if (attrlen < 18) {
				memset(pair->strvalue + attrlen, 0,
				       18 - attrlen);
			}
			break;

		default:
			DEBUG("    %s (Unknown Type %d)\n",
			      pair->name, pair->type);
			free(pair);
			pair = NULL;
			break;
		}

		if (pair) {
			debug_pair(pair);
			*tail = pair;
			tail = &pair->next;
		}

		ptr += attrlen;
		length -= attrlen;
		if (vendorlen > 0) vendorlen -= (attrlen + 2);
	}

	/*
	 *	Merge information from the outside world into our
	 *	random pool
	 */
	lrad_rand_seed(packet->data, AUTH_HDR_LEN);

	return 0;
}


/*
 *	Encode password.
 *
 *	We assume that the passwd buffer passed is big enough.
 *	RFC2138 says the password is max 128 chars, so the size
 *	of the passwd buffer must be at least 129 characters.
 *	Preferably it's just MAX_STRING_LEN.
 *
 *	int *pwlen is updated to the new length of the encrypted
 *	password - a multiple of 16 bytes.
 */
#define AUTH_PASS_LEN (AUTH_VECTOR_LEN)
int rad_pwencode(char *passwd, int *pwlen, const char *secret,
		 const char *vector)
{
	librad_MD5_CTX context, old;
	uint8_t	digest[AUTH_VECTOR_LEN];
	int	i, n, secretlen;
	int	len;

	/*
	 *	RFC maximum is 128 bytes.
	 *
	 *	If length is zero, pad it out with zeros.
	 *
	 *	If the length isn't aligned to 16 bytes,
	 *	zero out the extra data.
	 */
	len = *pwlen;

	if (len > 128) len = 128;

	if (len == 0) {
		memset(passwd, 0, AUTH_PASS_LEN);
		len = AUTH_PASS_LEN;
	} else if ((len % AUTH_PASS_LEN) != 0) {
		memset(&passwd[len], 0, AUTH_PASS_LEN - (len % AUTH_PASS_LEN));
		len += AUTH_PASS_LEN - (len % AUTH_PASS_LEN);
	}
	*pwlen = len;

	/*
	 *	Use the secret to setup the decryption digest
	 */
	secretlen = strlen(secret);
	
	librad_MD5Init(&context);
	librad_MD5Update(&context, secret, secretlen);
	old = context;		/* save intermediate work */

	/*
	 *	Encrypt it in place.  Don't bother checking
	 *	len, as we've ensured above that it's OK.
	 */
	for (n = 0; n < len; n += AUTH_PASS_LEN) {
		if (n == 0) {
			librad_MD5Update(&context, vector, AUTH_PASS_LEN);
			librad_MD5Final(digest, &context);
		} else {
			context = old;
			librad_MD5Update(&context,
					 passwd + n - AUTH_PASS_LEN,
					 AUTH_PASS_LEN);
			librad_MD5Final(digest, &context);
		}
		
		for (i = 0; i < AUTH_PASS_LEN; i++) {
			passwd[i + n] ^= digest[i];
		}
	}

	return 0;
}

/*
 *	Decode password.
 */
int rad_pwdecode(char *passwd, int pwlen, const char *secret,
		 const char *vector)
{
	librad_MD5_CTX context, old;
	uint8_t	digest[AUTH_VECTOR_LEN];
	int	i, n, secretlen;

	/*
	 *	The RFC's say that the maximum is 128.
	 *	The buffer we're putting it into above is 254, so
	 *	we don't need to do any length checking.
	 */
	if (pwlen > 128) pwlen = 128;

	/*
	 *	Catch idiots.
	 */
	if (pwlen == 0) goto done;

	/*
	 *	Use the secret to setup the decryption digest
	 */
	secretlen = strlen(secret);
	
	librad_MD5Init(&context);
	librad_MD5Update(&context, secret, secretlen);
	old = context;		/* save intermediate work */

	/*
	 *	The inverse of the code above.
	 */
	for (n = 0; n < pwlen; n += AUTH_PASS_LEN) {
		if (n == 0) {
			librad_MD5Update(&context, vector, AUTH_VECTOR_LEN);
			librad_MD5Final(digest, &context);

			context = old;
			librad_MD5Update(&context, passwd, AUTH_PASS_LEN);
		} else {
			librad_MD5Final(digest, &context);

			context = old;
			librad_MD5Update(&context, passwd + n, AUTH_PASS_LEN);
		}
		
		for (i = 0; i < AUTH_PASS_LEN; i++) {
			passwd[i + n] ^= digest[i];
		}
	}

 done:
	passwd[pwlen] = '\0';
	return strlen(passwd);
}

static unsigned int salt_offset = 0;

/*
 *	Encode Tunnel-Password attributes when sending them out on the wire.
 *
 *	int *pwlen is updated to the new length of the encrypted
 *	password - a multiple of 16 bytes.
 *
 *      This is per RFC-2868 which adds a two char SALT to the initial intermediate
 *      value MD5 hash.
 */
int rad_tunnel_pwencode(char *passwd, int *pwlen, const char *secret,
			const char *vector)
{
	uint8_t	buffer[AUTH_VECTOR_LEN + MAX_STRING_LEN + 3];
	unsigned char	digest[AUTH_VECTOR_LEN];
	char*   salt;
	int	i, n, secretlen;
	unsigned len, n2;

	len = *pwlen;

	if (len > 127) len = 127;

	/*
	 * Shift the password 3 positions right to place a salt and original
	 * length, tag will be added automatically on packet send
	 */
	for (n=len ; n>=0 ; n--) passwd[n+3] = passwd[n];
	salt = passwd;
	passwd += 2;
	/*
	 * save original password length as first password character;
	 */
	*passwd = len;
	len += 1;


	/*
	 *	Generate salt.  The RFC's say:
	 *
	 *	The high bit of salt[0] must be set, each salt in a
	 *	packet should be unique, and they should be random
	 *
	 *	So, we set the high bit, add in a counter, and then
	 *	add in some CSPRNG data.  should be OK..
	 */
	salt[0] = (0x80 | ( ((salt_offset++) & 0x0f) << 3) |
		   (lrad_rand() & 0x07));
	salt[1] = lrad_rand();

	/*
	 *	Padd password to multiple of AUTH_PASS_LEN bytes.
	 */
	n = len % AUTH_PASS_LEN;
	if (n) {
		n = AUTH_PASS_LEN - n;
		for (; n > 0; n--, len++)
			passwd[len] = 0;
	}
	/* set new password length */
	*pwlen = len + 2;

	/*
	 *	Use the secret to setup the decryption digest
	 */
	secretlen = strlen(secret);
	memcpy(buffer, secret, secretlen);

	for (n2 = 0; n2 < len; n2+=AUTH_PASS_LEN) {
		if (!n2) {
			memcpy(buffer + secretlen, vector, AUTH_VECTOR_LEN);
			memcpy(buffer + secretlen + AUTH_VECTOR_LEN, salt, 2);
			librad_md5_calc(digest, buffer, secretlen + AUTH_VECTOR_LEN + 2);
		} else {
			memcpy(buffer + secretlen, passwd + n2 - AUTH_PASS_LEN, AUTH_PASS_LEN);
			librad_md5_calc(digest, buffer, secretlen + AUTH_PASS_LEN);
		}

		for (i = 0; i < AUTH_PASS_LEN; i++) {
			passwd[i + n2] ^= digest[i];
		}
	}
	passwd[n2] = 0;
	return 0;
}

/*
 *	Decode Tunnel-Password encrypted attributes.
 *
 *      Defined in RFC-2868, this uses a two char SALT along with the
 *      initial intermediate value, to differentiate it from the
 *      above.
 */
int rad_tunnel_pwdecode(uint8_t *passwd, int *pwlen, const char *secret,
			const char *vector)
{
	librad_MD5_CTX  context, old;
	uint8_t		digest[AUTH_VECTOR_LEN];
	int		secretlen;
	unsigned	i, n, len, reallen;

	len = *pwlen;

	/*
	 *	We need at least a salt.
	 */
	if (len < 2) {
		librad_log("tunnel password is too short");
		return -1;
	}

	/*
	 *	There's a salt, but no password.  Or, there's a salt
	 *	and a 'data_len' octet.  It's wrong, but at least we
	 *	can figure out what it means: the password is empty.
	 *
	 *	Note that this means we ignore the 'data_len' field,
	 *	if the attribute length tells us that there's no
	 *	more data.  So the 'data_len' field may be wrong,
	 *	but that's ok...
	 */
	if (len <= 3) {
		passwd[0] = 0;
		*pwlen = 0;
		return 0;
	}

	len -= 2;		/* discount the salt */

	/*
	 *	Mash maximum values, too.
	 */
	if (len > 128) len = 128;

	/*
	 *	Use the secret to setup the decryption digest
	 */
	secretlen = strlen(secret);

	librad_MD5Init(&context);
	librad_MD5Update(&context, secret, secretlen);
	old = context;		/* save intermediate work */

	/*
	 *	Set up the initial key:
	 *
	 *	 b(1) = MD5(secret + vector + salt)
	 */
	librad_MD5Update(&context, vector, AUTH_VECTOR_LEN);
	librad_MD5Update(&context, passwd, 2);

	reallen = 0;
	for (n = 0; n < len; n += AUTH_PASS_LEN) {
		int base = 0;

		if (n == 0) {
			librad_MD5Final(digest, &context);

			context = old;

			/*
			 *	A quick check: decrypt the first octet
			 *	of the password, which is the
			 *	'data_len' field.  Ensure it's sane.
			 */
			reallen = passwd[2] ^ digest[0];
			if (reallen >= len) {
				librad_log("tunnel password is too long for the attribute");
				return -1;
			}

			librad_MD5Update(&context, passwd + 2, AUTH_PASS_LEN);

			base = 1;
		} else {
			librad_MD5Final(digest, &context);

			context = old;
			librad_MD5Update(&context, passwd + n + 2, AUTH_PASS_LEN);
		}

		for (i = base; i < AUTH_PASS_LEN; i++) {
			passwd[n + i - 1] = passwd[n + i + 2] ^ digest[i];
		}
	}

	*pwlen = reallen;
	passwd[reallen] = 0;

	return reallen;
}

/*
 *	Encode a CHAP password
 *
 *	FIXME: might not work with Ascend because
 *	we use vp->length, and Ascend gear likes
 *	to send an extra '\0' in the string!
 */
int rad_chap_encode(RADIUS_PACKET *packet, char *output, int id,
		    VALUE_PAIR *password)
{
	int		i;
	char		*ptr;
	char		string[MAX_STRING_LEN * 2 + 1];
	VALUE_PAIR	*challenge;

	/*
	 *	Sanity check the input parameters
	 */
	if ((packet == NULL) || (password == NULL)) {
		return -1;
	}

	/*
	 *	Note that the password VP can be EITHER
	 *	a User-Password attribute (from a check-item list),
	 *	or a CHAP-Password attribute (the client asking
	 *	the library to encode it).
	 */

	i = 0;
	ptr = string;
	*ptr++ = id;

	i++;
	memcpy(ptr, password->strvalue, password->length);
	ptr += password->length;
	i += password->length;

	/*
	 *	Use Chap-Challenge pair if present,
	 *	Request-Authenticator otherwise.
	 */
	challenge = pairfind(packet->vps, PW_CHAP_CHALLENGE);
	if (challenge) {
		memcpy(ptr, challenge->strvalue, challenge->length);
		i += challenge->length;
	} else {
		memcpy(ptr, packet->vector, AUTH_VECTOR_LEN);
		i += AUTH_VECTOR_LEN;
	}

	*output = id;
	librad_md5_calc((u_char *)output + 1, (u_char *)string, i);

	return 0;
}


/*
 *	Seed the random number generator.
 *
 *	May be called any number of times.
 */
void lrad_rand_seed(const void *data, size_t size)
{
	uint32_t hash;

	/*
	 *	Ensure that the pool is initialized.
	 */
	if (lrad_rand_index < 0) {
		int fd;
		
		memset(&lrad_rand_pool, 0, sizeof(lrad_rand_pool));

		fd = open("/dev/urandom", O_RDONLY);
		if (fd >= 0) {
			size_t total;
			ssize_t this;

			total = this = 0;
			while (total < sizeof(lrad_rand_pool.randrsl)) {
				this = read(fd, lrad_rand_pool.randrsl,
					    sizeof(lrad_rand_pool.randrsl) - total);
				if ((this < 0) && (errno != EINTR)) break;
				if (this > 0) total += this;
 			}
			close(fd);
		} else {
			time_t now = time(NULL);

			lrad_rand_pool.randrsl[0] = fd;
			lrad_rand_pool.randrsl[1] = hash;
			lrad_rand_pool.randrsl[2] = now;
		}

		lrad_randinit(&lrad_rand_pool, 1);
		lrad_rand_index = 0;
	}

	if (!data) return;

	/*
	 *	Seed the hash with data from the random pool, and update
	 *	it with data from the user.
	 */
	hash = lrad_hash(lrad_rand_pool.randrsl, 8);
	hash = lrad_hash_update(data, size, hash);
	
	lrad_rand_pool.randrsl[lrad_rand_index & 0xff] ^= hash;
	lrad_rand_index++;
	lrad_rand_index &= 0xff;

	/*
	 *	Churn the pool every so often after seeding it.
	 */
	if ((hash & 0x03) == (lrad_rand_pool.randrsl[lrad_rand_index & 0xff] & 0x03)) {
		lrad_isaac(&lrad_rand_pool);
	}
}


/*
 *	Return a 32-bit random number.
 */
uint32_t lrad_rand(void)
{
	uint32_t num;

	/*
	 *	Ensure that the pool is initialized.
	 */
	if (lrad_rand_index < 0) {
		lrad_rand_seed(NULL, 0);
	}

	/*
	 *	We don't return data directly from the pool.
	 *	Rather, we return a summary of the data.
	 */
	num = lrad_rand_pool.randrsl[lrad_rand_index & 0xff];
	lrad_rand_index++;

	/*
	 *	Every so often, churn the pool.
	 */
	if ((num & 0x0f) == (lrad_rand_pool.randrsl[lrad_rand_index & 0xff] & 0x0f)) {
		lrad_isaac(&lrad_rand_pool);
	}

	lrad_rand_index &= 0xff;
	return num;
}


/*
 *	Allocate a new RADIUS_PACKET
 */
RADIUS_PACKET *rad_alloc(int newvector)
{
	RADIUS_PACKET	*rp;

	if ((rp = malloc(sizeof(RADIUS_PACKET))) == NULL) {
		librad_log("out of memory");
		return NULL;
	}
	memset(rp, 0, sizeof(*rp));
	rp->id = -1;
	rp->verified = -1;

	if (newvector) {
		int i;
		uint32_t hash, base;

		/*
		 *	Don't expose the actual contents of the random
		 *	pool.
		 */
		base = lrad_rand();
		for (i = 0; i < AUTH_VECTOR_LEN; i += sizeof(uint32_t)) {
			hash = lrad_rand() ^ base;
			memcpy(rp->vector + i, &hash, sizeof(hash));
		}
	}
	lrad_rand();		/* stir the pool again */

	return rp;
}

/*
 *	Free a RADIUS_PACKET
 */
void rad_free(RADIUS_PACKET **radius_packet_ptr)
{
	RADIUS_PACKET *radius_packet;

	if (!radius_packet_ptr) return;
	radius_packet = *radius_packet_ptr;

	free(radius_packet->data);
	pairfree(&radius_packet->vps);

	free(radius_packet);

	*radius_packet_ptr = NULL;
}

/*************************************************************************
 *
 *      Function: make_secret
 *
 *      Purpose: Build an encrypted secret value to return in a reply
 *               packet.  The secret is hidden by xoring with a MD5 digest
 *               created from the shared secret and the authentication
 *               vector.  We put them into MD5 in the reverse order from
 *               that used when encrypting passwords to RADIUS.
 *
 *************************************************************************/
static void make_secret(unsigned char *digest, uint8_t *vector,
			const char *secret, char *value)
{
        u_char  buffer[256 + AUTH_VECTOR_LEN];
        int             secretLen = strlen(secret);
        int             i;

        memcpy(buffer, vector, AUTH_VECTOR_LEN );
        memcpy(buffer + AUTH_VECTOR_LEN, secret, secretLen );

        librad_md5_calc(digest, buffer, AUTH_VECTOR_LEN + secretLen );
        memset(buffer, 0, sizeof(buffer));

        for ( i = 0; i < AUTH_VECTOR_LEN; i++ ) {
		digest[i] ^= value[i];
        }
}
