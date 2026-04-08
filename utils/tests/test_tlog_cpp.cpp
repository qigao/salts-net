#include "tlog.h"
#include "tinytest.h"
#include <string>
#include <cstdio>

spec("TLog C++ Tests") {

  it("should log const char* with {}") {
    const char* msg = "hello from const char*";
    TLOG_INFO("const char*: {}", msg);
  }

  it("should log string literal with {}") {
    TLOG_INFO("literal: {}", "hello literal");
  }

  it("should log int with {}") {
    int x = 42;
    TLOG_INFO("int: {}", x);
  }

  it("should log double with {}") {
    double d = 3.14159;
    TLOG_INFO("double: {}", d);
  }

  it("should log size_t with {}") {
    size_t sz = 1024;
    TLOG_INFO("size_t: {}", sz);
  }

  it("should log bool with {}") {
    TLOG_INFO("bool true: {}", true);
    TLOG_INFO("bool false: {}", false);
  }

  it("should log std::string with {}") {
    std::string s = "hello from std::string";
    TLOG_INFO("std::string: {}", s);
    // Verify it doesn't print a pointer address
    printf("[VERIFY] std::string value: '%s'\n", s.c_str());
  }

  it("should log std::string .c_str() with {}") {
    std::string s = "hello via c_str";
    TLOG_INFO("c_str: {}", s.c_str());
  }

  it("should log mixed types") {
    std::string name = "Alice";
    int age = 30;
    double score = 95.5;
    TLOG_INFO("name={} age={} score={}", name, age, score);
    // Also with .c_str() for comparison
    TLOG_INFO("name={} age={} score={}", name.c_str(), age, score);
  }

  it("should log pointer with {}") {
    int x = 42;
    TLOG_INFO("pointer: {}", (void*)&x);
  }

  it("should log enum with {}") {
    enum Color { RED = 0, GREEN = 1, BLUE = 2 };
    Color c = GREEN;
    TLOG_INFO("enum: {}", c);
  }
}
