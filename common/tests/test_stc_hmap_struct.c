#include <stdio.h>
#include <stc/common.h>

typedef struct { int x, y, z; } Vec3i;

#define i_type hmap_vi
#define i_key Vec3i
#define i_val int
#define i_eq c_memcmp_eq // bitwise equal
#define i_static
#include <stc/hmap.h>

int main(void)
{
    // Initialize the map
    hmap_vi vecs = hmap_vi_init();

    hmap_vi_insert(&vecs, (Vec3i){100,   0,   0}, 1);
    hmap_vi_insert(&vecs, (Vec3i){  0, 100,   0}, 2);
    hmap_vi_insert(&vecs, (Vec3i){  0,   0, 100}, 3);
    hmap_vi_insert(&vecs, (Vec3i){100, 100, 100}, 4);

    // Using standard c_foreach_kv for iteration in newer STC
    // key is a pointer to Vec3i, val is a pointer to int
    c_foreach_kv (key, val, hmap_vi, vecs) {
        printf("{ %3d, %3d, %3d }: %d\n", key->x, key->y, key->z, *val);
    }

    hmap_vi_drop(&vecs);
    return 0;
}