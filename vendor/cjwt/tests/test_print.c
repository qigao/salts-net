/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjwt.h"

void setUp(void) {}
void tearDown(void) {}

void test_print(void)
{
    // clang-format off
    cjwt_t jwt = {
        .header.alg = alg_rs512,
        .iss = (char*) "Issuer Claim",
        .sub = (char*) "Sub Claim",
        .jti = (char*) "JTI Claim",
        .aud.count = 2,
        .aud.names = (char*[2]) { "Aud Item", "foo" },
        .exp = (int64_t[1]) {   100 },
        .nbf = (int64_t[1]) {  2000 },
        .iat = (int64_t[1]) { 40000 },
        .private_claims = cJSON_CreateObject(),
    };
    // clang-format on

    /* This is generally a debugging tool & validating the output is
     * less of a priority than not crashing. */

    cjwt_print(stdout, NULL);

    cjwt_print(stdout, &jwt);

    cJSON_Delete(jwt.private_claims);
    memset(&jwt, 0, sizeof(cjwt_t));

    cjwt_print(stdout, &jwt);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_print);
    return UNITY_END();
}
