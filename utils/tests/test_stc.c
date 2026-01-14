#undef i_static
#include <stc/cstr.h>
#include "unity.h"

// Define a vector of integers using i_type to explicitly name the struct
#define i_static
#define i_type IntVec
#define i_key int
#include <stc/vec.h>

// Define a hash map of string to int
#define i_static
#define i_type StrIntMap
#define i_key_str
#define i_val int
#include <stc/hmap.h>

void setUp(void) {
    // Set up before each test
}

void tearDown(void) {
    // Clean up after each test
}

void test_stc_vec(void) {
    IntVec vec = IntVec_init();
    
    IntVec_push(&vec, 10);
    IntVec_push(&vec, 20);
    IntVec_push(&vec, 30);
    
    TEST_ASSERT_EQUAL(3, IntVec_size(&vec));
    TEST_ASSERT_EQUAL(10, *IntVec_at(&vec, 0));
    TEST_ASSERT_EQUAL(20, *IntVec_at(&vec, 1));
    TEST_ASSERT_EQUAL(30, *IntVec_at(&vec, 2));
    
    IntVec_drop(&vec);
}

void test_stc_hmap(void) {
    StrIntMap map = StrIntMap_init();
    
    StrIntMap_insert(&map, cstr_from("apple"), 5);
    StrIntMap_insert(&map, cstr_from("banana"), 10);
    
    TEST_ASSERT_EQUAL(2, StrIntMap_size(&map));
    
    // get takes i_keyraw (const char*)
    const StrIntMap_value* v1 = StrIntMap_get(&map, "apple");
    TEST_ASSERT_NOT_NULL(v1);
    TEST_ASSERT_EQUAL(5, v1->second);
    
    const StrIntMap_value* v2 = StrIntMap_get(&map, "banana");
    TEST_ASSERT_NOT_NULL(v2);
    TEST_ASSERT_EQUAL(10, v2->second);
    
    StrIntMap_drop(&map);
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_stc_vec);
    RUN_TEST(test_stc_hmap);
    
    return UNITY_END();
}
