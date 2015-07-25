/* Copyright (C) 2009-2015, Martin Johansson <martin@fatbob.nu>
   Copyright (C) 2005-2015, Thorvald Natvig <thorvald@natvig.com>
   Copyright (C) 2015-2015, Szymon Pusz <szymon@pusz.net>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Developers nor the names of its contributors may
     be used to endorse or promote products derived from this software without
     specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "conf.h"
#include "log.h"
#include "ssl.h"

#include <stdlib.h>
#include <fcntl.h>

#include <mbedtls/config.h>
#include <mbedtls/havege.h>
#include <mbedtls/certs.h>
#include <mbedtls/x509.h>
#include <mbedtls/ssl.h>
#include <mbedtls/net.h>

const int ciphers[] =
{
    MBEDTLS_TLS_DHE_RSA_WITH_AES_256_CBC_SHA,
    MBEDTLS_TLS_RSA_WITH_AES_256_CBC_SHA,
    MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA,
    0
};

static mbedtls_x509_crt certificate;
static inline int x509parse_keyfile(mbedtls_pk_context *pk, const char *path,
                                    const char *pwd)
{
    int ret;

    mbedtls_pk_init(pk);
    ret = mbedtls_pk_parse_keyfile(pk, path, pwd);
    if (ret == 0 && !mbedtls_pk_can_do(pk, MBEDTLS_PK_RSA))
        ret = MBEDTLS_ERR_PK_TYPE_MISMATCH;

    return ret;
}

static mbedtls_pk_context key;
bool_t builtInTestCertificate;

#ifdef USE_MBEDTLS_HAVEGE
havege_state hs;
#else
int urandom_fd;
#endif

/* DH prime */
char *my_dhm_P =
	"9CE85640903BF123906947FEDE767261" \
	"D9B4A973EB8F7D984A8C656E2BCC161C" \
	"183D4CA471BA78225F940F16D1D99CA3" \
	"E66152CC68EDCE1311A390F307741835" \
	"44FF6AB553EC7073AD0CB608F2A3B480" \
	"19E6C02BCED40BD30E91BB2469089670" \
	"DEF409C08E8AC24D1732A6128D2220DC53";
char *my_dhm_G = "4";

#ifdef USE_MBEDTLS_TESTCERT
static void initTestCert()
{
	int rc;
	builtInTestCertificate = true;
	rc = mbedtls_x509_crt_parse_rsa(&certificate, (unsigned char *)test_srv_crt,
		strlen(test_srv_crt));

	if (rc != 0)
		Log_fatal("Could not parse built-in test certificate");
}

static void initTestKey()
{
	int rc;

	rc = mbedtls_x509parse_key_rsa(&key, (unsigned char *)test_srv_key,
	                       strlen(test_srv_key), NULL, 0);
	if (rc != 0)
		Log_fatal("Could not parse built-in test RSA key");
}
#endif

/*
 * How to generate a self-signed cert with openssl:
 * openssl genrsa 1024 > host.key
 * openssl req -new -x509 -nodes -sha1 -days 365 -key host.key > host.cert
 */
static void initCert()
{
	int rc;
	char *crtfile = (char *)getStrConf(CERTIFICATE);

	if (crtfile == NULL) {
#ifdef USE_MBEDTLS_TESTCERT
		Log_warn("No certificate file specified. Falling back to test certificate.");
		initTestCert();
#else
		Log_fatal("No certificate file specified");
#endif
		return;
	}

	rc = mbedtls_x509_crt_parse_file(&certificate, crtfile);

	if (rc != 0) {
#ifdef USE_MBEDTLS_TESTCERT
		Log_warn("Could not read certificate file '%s'. Falling back to test certificate.", crtfile);
		initTestCert();
#else
		Log_fatal("Could not read certificate file '%s'", crtfile);
#endif
		return;
	}
}

static void initKey()
{
	int rc;
	char *keyfile = (char *)getStrConf(KEY);

	if (keyfile == NULL)
		Log_fatal("No key file specified");
	rc = x509parse_keyfile(&key, keyfile, NULL);
	if (rc != 0)
		Log_fatal("Could not read RSA key file %s", keyfile);
}

#ifndef USE_MBEDTLS_HAVEGE
int urandom_bytes(void *ctx, unsigned char *dest, size_t len)
{
	int cur;

	while (len) {
		cur = read(urandom_fd, dest, len);
		if (cur < 0)
			continue;
		len -= cur;
	}
	return 0;
}
#endif

#define DEBUG_LEVEL 0
static void pssl_debug(void *ctx, int level, const char *file, int line, const char *str)
{
    if (level <= DEBUG_LEVEL)
		Log_info("mbedTLS [level %d]: %s", level, str);
}

void SSLi_init(void)
{
	char verstring[12];

	initCert();
#ifdef USE_MBEDTLS_TESTCERT
	if (builtInTestCertificate) {
		Log_warn("*** Using built-in test certificate and RSA key ***");
		Log_warn("*** This is not secure! Please use a CA-signed certificate or create a key and self-signed certificate ***");
		initTestKey();
	}
	else
		initKey();
#else
	initKey();
#endif

	/* Initialize random number generator */
#ifdef USE_MBEDTLS_HAVEGE
    mbedtls_havege_init(&hs);
#else
    urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd < 0)
	    Log_fatal("Cannot open /dev/urandom");
#endif

#ifdef MBEDTLS_VERSION_FEATURES
    mbedtls_version_get_string(verstring);
    Log_info("mbedTLS library version %s initialized", verstring);
#else
    Log_info("mbedTLS library initialized");
#endif
}

void SSLi_deinit(void)
{
	mbedtls_x509_crt_free(&certificate);
	mbedtls_pk_free(&key);
}

/* Create SHA1 of last certificate in the peer's chain. */
bool_t SSLi_getSHA1Hash(SSL_handle_t *ssl, uint8_t *hash)
{
	mbedtls_x509_crt const *cert;
	cert = mbedtls_ssl_get_peer_cert(ssl);

	if (!cert) {
		return false;
	}
	mbedtls_sha1(cert->raw.p, cert->raw.len, hash);
	return true;
}

SSL_handle_t *SSLi_newconnection(int *fd, bool_t *SSLready)
{
	mbedtls_ssl_context *ssl;
	mbedtls_ssl_session *ssn;
	mbedtls_ssl_config *conf;
	int rc;

	ssl = calloc(1, sizeof(mbedtls_ssl_context));
	ssn = calloc(1, sizeof(mbedtls_ssl_session));
	conf = calloc(1, sizeof(mbedtls_ssl_config));

	if (!ssl || !ssn || !conf)
		Log_fatal("Out of memory");

	mbedtls_ssl_init(ssl);
	mbedtls_ssl_config_init(conf);

	if((rc = mbedtls_ssl_config_defaults(conf,
			MBEDTLS_SSL_IS_SERVER,
			MBEDTLS_SSL_TRANSPORT_STREAM,
			MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
		Log_fatal("mbedtls_ssl_config_defaults returned %d", rc);

	mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
#ifdef USE_MBEDTLS_HAVEGE
	mbedtls_ssl_conf_rng(conf, HAVEGE_RAND, &hs);
#else
	mbedtls_ssl_conf_rng(conf, urandom_bytes, NULL);
#endif
	mbedtls_ssl_conf_dbg(conf, pssl_debug, NULL);
	mbedtls_ssl_set_bio(ssl, fd, mbedtls_net_send, mbedtls_net_recv, NULL);
	mbedtls_ssl_conf_ciphersuites(conf, (const int*)&ciphers);
	mbedtls_ssl_set_session(ssl, ssn);
	mbedtls_ssl_conf_ca_chain(conf, &certificate, NULL);

	if((rc = mbedtls_ssl_conf_own_cert(conf, &certificate, &key)) != 0)
		Log_fatal("mbedtls_ssl_conf_own_cert returned %d", rc);

	if((rc = mbedtls_ssl_conf_dh_param(conf, my_dhm_P, my_dhm_G)) != 0)
		Log_fatal("mbedtls_ssl_conf_dh_param returned %d", rc);

	if((rc = mbedtls_ssl_setup(ssl, conf)) != 0)
		Log_fatal("mbedtls_ssl_setup returned %d", rc);

	return ssl;
}

int SSLi_nonblockaccept(SSL_handle_t *ssl, bool_t *SSLready)
{
	int rc;

	rc = mbedtls_ssl_handshake(ssl);
	if (rc != 0) {
		if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
			return 0;
		} else if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) { /* Allow this (selfsigned etc) */
			return 0;
		} else {
			Log_warn("SSL handshake failed: %d", rc);
			return -1;
		}
	}
	*SSLready = true;
	return 0;
}

int SSLi_read(SSL_handle_t *ssl, uint8_t *buf, int len)
{
	int rc;

	rc = mbedtls_ssl_read(ssl, buf, len);
	if (rc == MBEDTLS_ERR_SSL_WANT_READ)
		return SSLI_ERROR_WANT_READ;
	return rc;
}

int SSLi_write(SSL_handle_t *ssl, uint8_t *buf, int len)
{
	int rc;

	rc = mbedtls_ssl_write(ssl, buf, len);
	if (rc == MBEDTLS_ERR_SSL_WANT_WRITE)
		return SSLI_ERROR_WANT_WRITE;
	return rc;
}

int SSLi_get_error(SSL_handle_t *ssl, int code)
{
	return code;
}

bool_t SSLi_data_pending(SSL_handle_t *ssl)
{
	return mbedtls_ssl_get_bytes_avail(ssl) > 0;
}

void SSLi_shutdown(SSL_handle_t *ssl)
{
	mbedtls_ssl_close_notify(ssl);
}

void SSLi_free(SSL_handle_t *ssl)
{
	Log_debug("SSLi_free");
	mbedtls_ssl_config_free((mbedtls_ssl_config*)ssl->conf);
	mbedtls_ssl_free(ssl);
	free((mbedtls_ssl_config*)ssl->conf);
	free(ssl);
}

