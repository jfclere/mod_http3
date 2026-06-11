/*
 * Copyright (c) 2026 The mod_http3 Project Authors. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sput.h"
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>

static void test_openssl_version(void)
{
    sput_fail_unless(OPENSSL_VERSION_MAJOR == 3, "OpenSSL major version == 3");
    sput_fail_unless(OPENSSL_VERSION_MINOR == 5, "OpenSSL minor version == 5");
}

static void test_openssl_quic_client_method(void)
{
    const SSL_METHOD* method = OSSL_QUIC_client_method();
    sput_fail_unless(method != NULL, "QUIC client method not NULL");
}

static void test_openssl_quic_ssl_context_creation(void)
{
    SSL_CTX* ctx = SSL_CTX_new(OSSL_QUIC_client_method());
    sput_fail_unless(ctx != NULL, "QUIC SSL_CTX created");
    SSL_CTX_free(ctx);
}

static void test_openssl_evp_digest_sha256(void)
{
    const EVP_MD* sha256 = EVP_sha256();
    sput_fail_unless(sha256 != NULL, "EVP_sha256 not NULL");
    sput_fail_unless(EVP_MD_size(sha256) == 32, "SHA256 digest size is 32");
}

void run_openssl_tests(void)
{
    sput_run_test(test_openssl_version);
    sput_run_test(test_openssl_quic_client_method);
    sput_run_test(test_openssl_quic_ssl_context_creation);
    sput_run_test(test_openssl_evp_digest_sha256);
}
