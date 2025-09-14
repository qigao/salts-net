#include <stc/cstr.h>
#include <stdio.h>

int main(void) {
    cstr s0, s1, full_path;
    c_defer(
        cstr_drop(&s0), cstr_drop(&s1), cstr_drop(&full_path)
    ){
        s0 = cstr_lit("Initialization without using strlen().");
        printf("%s\nLength: %" c_ZI "\n\n", cstr_str(&s0), cstr_size(&s0));

        s1 = cstr_lit("one-nine-three-seven-five.");
        printf("%s\n", cstr_str(&s1));

        cstr_insert(&s1, 3, "-two");
        printf("%s\n", cstr_str(&s1));

        cstr_erase(&s1, 7, 5); // -nine
        printf("%s\n", cstr_str(&s1));

        cstr_replace_nfirst(&s1, "seven", "four", 1);
        printf("%s\n", cstr_str(&s1));

        // reassign:
        cstr_assign(&s1, "one two three four five six seven");
        cstr_append(&s1, " eight");
        printf("append: %s\n", cstr_str(&s1));

        full_path = cstr_from_fmt("%s/%s.%s", "directory", "filename", "ext");
        printf("%s\n", cstr_str(&full_path));
    }
    return 0;
}
