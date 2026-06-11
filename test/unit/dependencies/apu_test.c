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
#include <apr_base64.h>
#include <apr_general.h>
#include <apr_md5.h>
#include <apr_pools.h>
#include <apu.h>
#include <apu_version.h>
#include <stdlib.h>
#include <string.h>

static void test_apu_version(void)
{
    sput_fail_unless(APU_MAJOR_VERSION == 1, "APU major version == 1");
    sput_fail_unless(APU_MINOR_VERSION == 6, "APU minor version == 6");
}

static void test_apu_base64_encode_decode(void)
{
    const char input[] = "mod_http3 apr-util test";
    int input_len = (int)(sizeof(input) - 1);

    int enc_buf_len = apr_base64_encode_len(input_len);
    sput_fail_unless(enc_buf_len > 0, "encode buffer length > 0");

    char* encoded = (char*)malloc(enc_buf_len);
    sput_fail_unless(encoded != NULL, "encoded buffer allocated");

    int enc_len = apr_base64_encode(encoded, input, input_len);
    sput_fail_unless(enc_len > 0, "encoded length > 0");
    sput_fail_unless(enc_len <= enc_buf_len, "encoded length <= buffer length");

    encoded[enc_len] = '\0';

    int dec_buf_len = apr_base64_decode_len(encoded);
    sput_fail_unless(dec_buf_len > 0, "decode buffer length > 0");

    char* decoded = (char*)malloc(dec_buf_len);
    sput_fail_unless(decoded != NULL, "decoded buffer allocated");

    int dec_len = apr_base64_decode(decoded, encoded);
    sput_fail_unless(dec_len == input_len, "decoded length equals input length");
    sput_fail_unless(memcmp(decoded, input, input_len) == 0, "decoded equals input");

    free(encoded);
    free(decoded);
}

static void test_apu_md5_digest(void)
{
    apr_pool_t* pool = NULL;
    apr_pool_create(&pool, NULL);

    const unsigned char input[] = "mod_http3 apr-util md5";
    int input_len = (int)(sizeof(input) - 1);

    unsigned char digest[APR_MD5_DIGESTSIZE];
    apr_status_t rv = apr_md5(digest, input, input_len);
    sput_fail_unless(rv == APR_SUCCESS, "apr_md5 succeeds");

    int any_non_zero = 0;
    for (int i = 0; i < APR_MD5_DIGESTSIZE; i++)
    {
        if (digest[i] != 0)
        {
            any_non_zero = 1;
            break;
        }
    }
    sput_fail_unless(any_non_zero, "MD5 digest is not all zeros");

    apr_pool_destroy(pool);
}

void run_apu_tests(void)
{
    sput_run_test(test_apu_version);
    sput_run_test(test_apu_base64_encode_decode);
    sput_run_test(test_apu_md5_digest);
}
