cmake_add_test(
  SOURCES test_turbo_crypto.c
  LIBS turbo_crypto Rocida::TinyTest
  FOLDER "vendor/turbo_crypto/tests")
