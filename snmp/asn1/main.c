#include <stdio.h>
#include <stdlib.h>

// Prototype from scanner.re
void scan(const char *cursor);

int main() {
    // Simple ASN.1 Test Case
    const char *asn1_input = 
        "MyModule DEFINITIONS ::= BEGIN "
        "  MyPacket ::= SEQUENCE { "
        "    id INTEGER, "
        "    valid BOOLEAN "
        "  } "
        "END";

    printf("Input: \n%s\n\n", asn1_input);
    scan(asn1_input);

    return 0;
}