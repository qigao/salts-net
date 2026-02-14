#include "fmt.h"
#include <stdio.h>

// Helper macro to get enum name as string
#define ENUM_NAME(x) #x

int main() {
    char buf[256];
    int val = 65;
    
    // Test basic integer as char
    fmt(buf, sizeof(buf), "Integer as char: '{:c}'", val);
    printf("%s\n", buf);

    // Test enum as char with name
    enum Color { RED = 65, GREEN = 66 };
    Color c = GREEN;
    fmt(buf, sizeof(buf), "Enum as char: '{:c}' -- {}", c, ENUM_NAME(GREEN));
    printf("%s\n", buf);

    // Test mixing multiple types
    fmt(buf, sizeof(buf), "Mixed: {} is '{:c}'", val, val);
    printf("%s\n", buf);

    return 0;
}
