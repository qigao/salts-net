#include "tinytest.h"

#ifndef _WIN32
#include <unistd.h>
#endif

static char *tt_join_path__(const char *dir, const char *name) {
  size_t len = strlen(dir) + strlen(name) + 2;
  char *path = (char *)malloc(len);
  if (!path) return NULL;
#ifdef _WIN32
  snprintf(path, len, "%s\\%s", dir, name);
#else
  snprintf(path, len, "%s/%s", dir, name);
#endif
  return path;
}

spec("tinytest runtime regression") {
  it("check_int_eq_warn should evaluate operands once") {
    int value = 1;
    check_int_eq_warn(value++, 2);
    check_int_eq(value, 2);
  }

  it("check_mem_eq_warn should evaluate length once") {
    unsigned char actual[2] = {0, 0};
    unsigned char expected[2] = {1, 0};
    size_t len = 1;
    check_mem_eq_warn(actual, expected, len++);
    check_size_eq(len, 2);
  }

  it("check_warn should behave as a single statement") {
    if (1)
      check_warn(1);
    else
      check_warn(0);
    check_true(1);
  }

#ifndef _WIN32
  it("tt_remove_tree should not follow directory symlinks") {
    char *root = tt_make_temp_dir("ttroot");
    char *outside = tt_make_temp_dir("ttout");
    char *link = NULL;
    char *sentinel = NULL;
    size_t size = 0;
    char *content = NULL;

    check_not_null(root);
    check_not_null(outside);

    link = tt_join_path__(root, "linked");
    sentinel = tt_join_path__(outside, "sentinel.txt");

    check_not_null(link);
    check_not_null(sentinel);
    check_int_eq(tt_write_file(sentinel, "x", 1), 0);
    check_int_eq(symlink(outside, link), 0);
    check_int_eq(tt_remove_tree(root), 0);

    content = tt_read_file(sentinel, &size);
    check_not_null(content);
    check_size_eq(size, 1);

    free(content);
    check_int_eq(tt_remove_tree(outside), 0);
    free(root);
    free(outside);
    free(link);
    free(sentinel);
  }
#else
  it_skip("tt_remove_tree should not follow directory symlinks") {
    check_true(1);
  }
#endif
}
