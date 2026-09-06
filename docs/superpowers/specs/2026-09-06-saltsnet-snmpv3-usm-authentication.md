# SaltsNet SNMPv3 USM authentication hardening

## Context

The SNMPv3 parser currently decodes authenticated messages without verifying
`msgAuthenticationParameters`. The builder can also emit an authenticated
security level without usable credentials and silently succeeds if the digest
cannot be inserted.

RFC 3414 requires the receiver to save the received digest, replace its exact
wire field with twelve zero octets, authenticate the complete serialized
message, and reject a mismatch.

## Data and ownership protocol

- The input packet is a borrowed immutable byte range for `snmp_parse_v3`.
- Authenticated parsing accepts at most 65,535 bytes. It creates one owned
  verification copy, clears only the located authentication value, computes
  the HMAC, and releases the copy before returning.
- The decoded `snmp_message_t` remains owned by the caller or its supplied
  `MemoryPool` under the existing parser contract.
- Builder output remains caller-owned. Authentication is committed only after
  the complete message is encoded and the exact field is located.
- No buffer or decoded ASN.1 view crosses a callback, thread, or CNet poll.

## Wire location and bounds

The locator walks the fixed SNMPv3/USM shape:

1. outer message `SEQUENCE`;
2. its third child, `msgSecurityParameters OCTET STRING`;
3. the embedded USM `SEQUENCE`;
4. its fifth child, `msgAuthenticationParameters OCTET STRING`.

Every TLV uses checked length arithmetic and must be contained by its parent.
Indefinite-length authenticated input is rejected because an in-place exact
wire locator is required; it is never accepted without authentication.

## Errors and compatibility

- `SNMP_PARSE_ERROR_AUTH` distinguishes missing credentials, user mismatch,
  malformed authentication parameters, unsupported authentication protocol,
  and digest mismatch from ordinary syntax errors.
- `authPriv` without authentication is rejected.
- HMAC-MD5-96 and HMAC-SHA-96 retain their twelve-octet wire contract.
- The existing `SNMP_AUTH_SHA256` value cannot implement RFC 7860 with the
  current twelve-byte public buffer. Authenticated message construction rejects
  it until a size-versioned USM credential/message API is introduced.

## Verification

- A valid `authNoPriv` message round-trips.
- Digest mutation, missing credentials, and user mismatch fail authentication.
- Authenticated construction without usable credentials fails.
- Existing SNMP, ASN.1, CNet transport, and full repository tests remain green.
