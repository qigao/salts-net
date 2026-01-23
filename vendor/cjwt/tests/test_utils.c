/* SPDX-FileCopyrightText: 2021-2022 Comcast Cable Communications Management, LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/utils.h"

void setUp(void) {}
void tearDown(void) {}

struct test_vector {
    const char *full;
    size_t len;
    int rv;
    struct split_jwt goal;
};

void test_split(void)
{
    // clang-format off
    struct test_vector tests[] = {
        {   .full = "abcdefghijkl",
            .len  = 12,
            .rv   = -1,
        },
        {   .full = "a.b.c.d.e.fghijkl",
            .len  = 17,
            .rv   = -1,
        },
        {   .full = "abcd.efg",
            .len  = 8,
            .rv   = 0,
            .goal = {
                .count = 2,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = NULL,   .len = 0 },
                    { .data = NULL,   .len = 0 },
                    { .data = NULL,   .len = 0 },
                },
            },
        },
        {   .full = "abcd.efg.hij",
            .len  = 12,
            .rv   = 0,
            .goal = {
                .count = 3,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = "hij",  .len = 3 },
                    { .data = NULL,   .len = 0 },
                    { .data = NULL,   .len = 0 },
                },
            },
        },
        {   .full = "abcd.efg.hij.klm",
            .len  = 16,
            .rv   = 0,
            .goal = {
                .count = 4,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = "hij",  .len = 3 },
                    { .data = "klm",  .len = 3 },
                    { .data = NULL,   .len = 0 },
                },
            },
        },
        {   .full = "abcd.efg.hij.klm.op",
            .len  = 19,
            .rv   = 0,
            .goal = {
                .count = 5,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = "hij",  .len = 3 },
                    { .data = "klm",  .len = 3 },
                    { .data = "op",   .len = 2 },
                },
            },
        },
        {   .full = "abcd.efg..klm.op",
            .len  = 16,
            .rv   = 0,
            .goal = {
                .count = 5,
                .sections = {
                    { .data = "abcd", .len = 4 },
                    { .data = "efg",  .len = 3 },
                    { .data = "",     .len = 0 },
                    { .data = "klm",  .len = 3 },
                    { .data = "op",   .len = 2 },
                },
            },
        },
        {   .full = "....",
            .len  = 4,
            .rv   = 0,
            .goal = {
                .count = 5,
                .sections = {
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                },
            },
        },
        {   .full = "d....g",
            .len  = 6,
            .rv   = 0,
            .goal = {
                .count = 5,
                .sections = {
                    { .data = "d",    .len = 1 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "g",    .len = 1 },
                },
            },
        },
        {   .full = "dog.",
            .len  = 4,
            .rv   = 0,
            .goal = {
                .count = 2,
                .sections = {
                    { .data = "dog",  .len = 3 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                    { .data = "",     .len = 0 },
                },
            },
        },
    };
    // clang-format on

    for (size_t i = 0; i < sizeof(tests) / sizeof(struct test_vector); i++) {
        struct split_jwt got;
        int rv;

        rv = split(tests[i].full, tests[i].len, &got);

        TEST_ASSERT_EQUAL_INT(tests[i].rv, rv);
        if (0 == tests[i].rv) {
            TEST_ASSERT_EQUAL_UINT(tests[i].goal.count, got.count);

            for (size_t j = 0; j < got.count; j++) {
                TEST_ASSERT_EQUAL_UINT(tests[i].goal.sections[j].len, got.sections[j].len);

                for (size_t k = 0; k < tests[i].goal.sections[j].len; k++) {
                    TEST_ASSERT_EQUAL_INT(tests[i].goal.sections[j].data[k], got.sections[j].data[k]);
                }
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_split);
    return UNITY_END();
}
