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
#include <httpd.h>

static void test_httpd_version(void)
{
    sput_fail_unless(AP_SERVER_MAJORVERSION_NUMBER == 2, "httpd major version == 2");
    sput_fail_unless(AP_SERVER_MINORVERSION_NUMBER == 5, "httpd minor version == 5");
}

static void test_httpd_status_codes(void)
{
    sput_fail_unless(HTTP_OK == 200, "HTTP_OK is 200");
    sput_fail_unless(HTTP_NOT_FOUND == 404, "HTTP_NOT_FOUND is 404");
    sput_fail_unless(HTTP_INTERNAL_SERVER_ERROR == 500, "HTTP_INTERNAL_SERVER_ERROR is 500");
}

void run_httpd_tests(void)
{
    sput_run_test(test_httpd_version);
    sput_run_test(test_httpd_status_codes);
}
