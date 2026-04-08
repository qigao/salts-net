#undef i_static
#include <stc/cstr.h>
#include "tinytest.h"

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

spec("STC Tests") {
  it("should handle IntVec operations") {
    IntVec vec = IntVec_init();

    IntVec_push(&vec, 10);
    IntVec_push(&vec, 20);
    IntVec_push(&vec, 30);

    check_int_eq((int)IntVec_size(&vec), 3);
    check_int_eq(*IntVec_at(&vec, 0), 10);
    check_int_eq(*IntVec_at(&vec, 1), 20);
    check_int_eq(*IntVec_at(&vec, 2), 30);

    IntVec_drop(&vec);
  }

  it("should handle StrIntMap operations") {
    StrIntMap map = StrIntMap_init();

    StrIntMap_insert(&map, cstr_from("apple"), 5);
    StrIntMap_insert(&map, cstr_from("banana"), 10);

    check_int_eq((int)StrIntMap_size(&map), 2);

    // get takes i_keyraw (const char*)
    const StrIntMap_value *v1 = StrIntMap_get(&map, "apple");
    check_not_null(v1);
    check_int_eq(v1->second, 5);

    const StrIntMap_value *v2 = StrIntMap_get(&map, "banana");
    check_not_null(v2);
    check_int_eq(v2->second, 10);

    StrIntMap_drop(&map);
  }
}
