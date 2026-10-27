# SNMP MIB textual schema tool

This directory contains only the SNMP-specific MIB schema-to-C **command-line
compiler**. It does not implement or export any binary ASN.1 BER/DER codec,
X.509 parser, cryptographic primitive, independent runtime or SaltsNet library.

- `mib_compiler`: single native executable; its scanner and Lemon grammar
  are compiled privately into that executable, with no separately installed
  parser/AST libraries or public headers.
- Parser sources under `parser/` process textual SNMP MIB schema syntax;
  they do **not** parse BER/DER on the wire.
- General BER/DER ASN.1 and OID/Value Tree belong only to
  `qigao/crypto/asn1/` (`Salts::ASN1`).
- X.509 DER inspection, key formats and cryptography belong only to
  `qigao/crypto/crypto/` (`Salts::Crypto`).

The CLI is intentionally not a complete ASN.1 schema implementation.
It does not install or export `SaltsNet::SNMPSupport` or
`SaltsNet::Asn1Parser`. Output directory must exist before execution;
the tool never invokes a shell to create paths. The codegen behavior is
unqualified for production until real MIB corpus and strict generated
C11 tests pass. Do not present CLI availability as proof of full SNMP MIB
schema support.
