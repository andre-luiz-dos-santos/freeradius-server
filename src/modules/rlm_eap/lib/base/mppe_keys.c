/*
 * mppe_keys.c
 *
 * Version:     $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2002  Axis Communications AB
 * Copyright 2006  The FreeRADIUS server project
 * Authors: Henrik Eriksson <henriken@axis.com> & Lars Viklund <larsv@axis.com>
 */

RCSID("$Id$")
USES_APPLE_DEPRECATED_API	/* OpenSSL API has been deprecated by Apple */

#include "eap_tls.h"
#include <openssl/hmac.h>
#include <freeradius-devel/sha1.h>


/*
 * TLS PRF from RFC 2246
 */
static void P_hash(EVP_MD const *evp_md,
		   unsigned char const *secret, unsigned int secret_len,
		   unsigned char const *seed,   unsigned int seed_len,
		   unsigned char *out, unsigned int out_len)
{
	HMAC_CTX ctx_a, ctx_out;
	unsigned char a[HMAC_MAX_MD_CBLOCK];
	unsigned int size;

	HMAC_CTX_init(&ctx_a);
	HMAC_CTX_init(&ctx_out);
#ifdef EVP_MD_CTX_FLAG_NON_FIPS_ALLOW
	HMAC_CTX_set_flags(&ctx_a, EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
	HMAC_CTX_set_flags(&ctx_out, EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
#endif
	HMAC_Init_ex(&ctx_a, secret, secret_len, evp_md, NULL);
	HMAC_Init_ex(&ctx_out, secret, secret_len, evp_md, NULL);

	size = HMAC_size(&ctx_out);

	/* Calculate A(1) */
	HMAC_Update(&ctx_a, seed, seed_len);
	HMAC_Final(&ctx_a, a, NULL);

	while (1) {
		/* Calculate next part of output */
		HMAC_Update(&ctx_out, a, size);
		HMAC_Update(&ctx_out, seed, seed_len);

		/* Check if last part */
		if (out_len < size) {
			HMAC_Final(&ctx_out, a, NULL);
			memcpy(out, a, out_len);
			break;
		}

		/* Place digest in output buffer */
		HMAC_Final(&ctx_out, out, NULL);
		HMAC_Init_ex(&ctx_out, NULL, 0, NULL, NULL);
		out += size;
		out_len -= size;

		/* Calculate next A(i) */
		HMAC_Init_ex(&ctx_a, NULL, 0, NULL, NULL);
		HMAC_Update(&ctx_a, a, size);
		HMAC_Final(&ctx_a, a, NULL);
	}

	HMAC_CTX_cleanup(&ctx_a);
	HMAC_CTX_cleanup(&ctx_out);
	memset(a, 0, sizeof(a));
}

/*  EAP-FAST Pseudo-Random Function (T-PRF): RFC 4851, Section 5.5 */
void T_PRF(unsigned char const *secret, unsigned int secret_len,
	   char const *prf_label,
	   unsigned char const *seed,  unsigned int seed_len,
	   unsigned char *out, unsigned int out_len)
{
	size_t prf_size = strlen(prf_label);
	size_t pos;
	uint8_t	*buf;

	if (prf_size > 128) prf_size = 128;
	prf_size++;	/* include trailing zero */

	buf = talloc_size(NULL, SHA1_DIGEST_LENGTH + prf_size + seed_len + 2 + 1);

	memcpy(buf + SHA1_DIGEST_LENGTH, prf_label, prf_size);
	if (seed) memcpy(buf + SHA1_DIGEST_LENGTH + prf_size, seed, seed_len);
	*(uint16_t *)&buf[SHA1_DIGEST_LENGTH + prf_size + seed_len] = htons(out_len);
	buf[SHA1_DIGEST_LENGTH + prf_size + seed_len + 2] = 1;

	// T1 is just the seed
	fr_hmac_sha1(buf, buf + SHA1_DIGEST_LENGTH, prf_size + seed_len + 2 + 1, secret, secret_len);

#define MIN(a,b) (((a)>(b)) ? (b) : (a))
	memcpy(out, buf, MIN(out_len, SHA1_DIGEST_LENGTH));

	pos = SHA1_DIGEST_LENGTH;
	while (pos < out_len) {
		buf[SHA1_DIGEST_LENGTH + prf_size + seed_len + 2]++;

		fr_hmac_sha1(buf, buf, SHA1_DIGEST_LENGTH + prf_size + seed_len + 2 + 1, secret, secret_len);
		memcpy(&out[pos], buf, MIN(out_len - pos, SHA1_DIGEST_LENGTH));

		if (out_len - pos <= SHA1_DIGEST_LENGTH)
			break;

		pos += SHA1_DIGEST_LENGTH;
	}

	memset(buf, 0, SHA1_DIGEST_LENGTH + prf_size + seed_len + 2 + 1);
	talloc_free(buf);
}

static void PRF(unsigned char const *secret, unsigned int secret_len,
		unsigned char const *seed,   unsigned int seed_len,
		unsigned char *out, unsigned char *buf, unsigned int out_len)
{
	unsigned int i;
	unsigned int len = (secret_len + 1) / 2;
	uint8_t const *s1 = secret;
	uint8_t const *s2 = secret + (secret_len - len);

	P_hash(EVP_md5(),  s1, len, seed, seed_len, out, out_len);
	P_hash(EVP_sha1(), s2, len, seed, seed_len, buf, out_len);

	for (i=0; i < out_len; i++) {
		out[i] ^= buf[i];
	}
}

#define EAP_TLS_MPPE_KEY_LEN     32

/** Generate keys according to RFC 2716 and add to the reply
 *
 */
void eap_tls_gen_mppe_keys(REQUEST *request, SSL *s, char const *prf_label)
{
	uint8_t out[4 * EAP_TLS_MPPE_KEY_LEN];
	uint8_t *p;
	size_t prf_size;

	prf_size = strlen(prf_label);

#if OPENSSL_VERSION_NUMBER >= 0x10001000L
	if (SSL_export_keying_material(s, out, sizeof(out), prf_label, prf_size, NULL, 0, 0) != 1) /* Fallback */
#endif

	{
		uint8_t seed[64 + (2 * SSL3_RANDOM_SIZE)];
		uint8_t buf[4 * EAP_TLS_MPPE_KEY_LEN];

		p = seed;

		memcpy(p, prf_label, prf_size);
		p += prf_size;

		memcpy(p, s->s3->client_random, SSL3_RANDOM_SIZE);
		p += SSL3_RANDOM_SIZE;
		prf_size += SSL3_RANDOM_SIZE;

		memcpy(p, s->s3->server_random, SSL3_RANDOM_SIZE);
		prf_size += SSL3_RANDOM_SIZE;

		PRF(s->session->master_key, s->session->master_key_length,
		    seed, prf_size, out, buf, sizeof(out));
	}

	RDEBUG2("Adding session keys");
	p = out;
	eap_add_reply(request, "MS-MPPE-Recv-Key", p, EAP_TLS_MPPE_KEY_LEN);
	p += EAP_TLS_MPPE_KEY_LEN;
	eap_add_reply(request, "MS-MPPE-Send-Key", p, EAP_TLS_MPPE_KEY_LEN);

	eap_add_reply(request, "EAP-MSK", out, 64);
	eap_add_reply(request, "EAP-EMSK", out + 64, 64);
}


/*
 *	Generate the challenge using a PRF label.
 *
 *	It's in the TLS module simply because it's only a few lines
 *	of code, and it needs access to the TLS PRF functions.
 */
void eap_tls_gen_challenge(SSL *s, uint8_t *buffer, uint8_t *scratch, size_t size, char const *prf_label)
{
	uint8_t seed[128 + 2*SSL3_RANDOM_SIZE];
	uint8_t *p = seed;
	size_t len;

#if OPENSSL_VERSION_NUMBER >= 0x10001000L
	if (SSL_export_keying_material(s, buffer, size, prf_label,
				       strlen(prf_label), NULL, 0, 0) == 1) return;

#endif

	len = strlen(prf_label);
	if (len > 128) len = 128;

	memcpy(p, prf_label, len);
	p += len;
	memcpy(p, s->s3->client_random, SSL3_RANDOM_SIZE);
	p += SSL3_RANDOM_SIZE;
	memcpy(p, s->s3->server_random, SSL3_RANDOM_SIZE);
	p += SSL3_RANDOM_SIZE;

	PRF(s->session->master_key, s->session->master_key_length,
	    seed, p - seed, buffer, scratch, size);
}


/*
 *	Same as before, but for EAP-FAST the order of {server,client}_random is flipped
 */
void eap_fast_tls_gen_challenge(SSL *s, uint8_t *buffer, uint8_t *scratch, size_t size, char const *prf_label)
{
	uint8_t seed[128 + 2*SSL3_RANDOM_SIZE];
	uint8_t *p = seed;
	size_t len;

	len = strlen(prf_label);
	if (len > 128) len = 128;

	memcpy(p, prf_label, len);
	p += len;
	memcpy(p, s->s3->server_random, SSL3_RANDOM_SIZE);
	p += SSL3_RANDOM_SIZE;
	memcpy(p, s->s3->client_random, SSL3_RANDOM_SIZE);
	p += SSL3_RANDOM_SIZE;

	PRF(s->session->master_key, s->session->master_key_length,
	    seed, p - seed, buffer, scratch, size);
}

/*
 *	Actually generates EAP-Session-Id, which is an internal server
 *	attribute.  Not all systems want to send EAP-Key-Name
 */
void eap_tls_gen_eap_key(RADIUS_PACKET *packet, SSL *s, uint32_t header)
{
	VALUE_PAIR *vp;
	uint8_t *p;

	vp = fr_pair_afrom_num(packet, 0, PW_EAP_SESSION_ID);
	if (!vp) return;

	p = talloc_array(vp, uint8_t, 1 + 2 * SSL3_RANDOM_SIZE);
	p[0] = header & 0xff;

#ifdef HAVE_SSL_GET_CLIENT_RANDOM
	SSL_get_client_random(s, p + 1, SSL3_RANDOM_SIZE);
	SSL_get_server_random(s, p + 1 + SSL3_RANDOM_SIZE, SSL3_RANDOM_SIZE);
#else
	memcpy(p + 1, s->s3->client_random, SSL3_RANDOM_SIZE);
	memcpy(p + 1 + SSL3_RANDOM_SIZE,
	       s->s3->server_random, SSL3_RANDOM_SIZE);
#endif
	fr_pair_value_memsteal(vp, p);
	fr_pair_add(&packet->vps, vp);
}
