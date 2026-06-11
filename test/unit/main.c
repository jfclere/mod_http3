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
#include <stdio.h>

struct sput __sput;

int main(void)
{
    apr_status_t rc = apr_initialize();
    if (rc != APR_SUCCESS)
    {
        fprintf(stderr, "apr_initialize() failed\n");
        return 1;
    }

    sput_start_testing();

    extern void run_dependencies_suite(void);
    run_dependencies_suite();

    sput_finish_testing();

    apr_terminate();

    return sput_get_return_value();
}
