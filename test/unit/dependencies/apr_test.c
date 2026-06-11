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
#include <apr_general.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_version.h>
#include <string.h>

static void test_apr_version(void)
{
    sput_fail_unless(APR_MAJOR_VERSION == 1, "APR major version >= 1");
    sput_fail_unless(APR_MINOR_VERSION == 7, "APR minor version >= 7");
}

static void test_apr_pool_create_destroy(void)
{
    apr_pool_t* pool = NULL;
    apr_status_t rv = apr_pool_create(&pool, NULL);
    sput_fail_unless(rv == APR_SUCCESS, "apr_pool_create succeeds");
    sput_fail_unless(pool != NULL, "pool is not NULL");
    apr_pool_destroy(pool);
}

static void test_apr_string_dup(void)
{
    apr_pool_t* pool = NULL;
    apr_pool_create(&pool, NULL);
    const char* original = "mod_http3 test string";
    char* copy = apr_pstrdup(pool, original);
    sput_fail_unless(copy != NULL, "copy is not NULL");
    sput_fail_unless(strcmp(copy, original) == 0, "copy equals original");
    sput_fail_unless(copy != original, "copy is different pointer");
    apr_pool_destroy(pool);
}

void run_apr_tests(void)
{
    sput_run_test(test_apr_version);
    sput_run_test(test_apr_pool_create_destroy);
    sput_run_test(test_apr_string_dup);
}
