# SaltsNet cryptographic compatibility matrix

This matrix describes the implemented SaltsNet `1.1` source contract. A row is
**verified** only when a protocol test exercises the wire operation; availability
of a digest or cipher primitive alone is not sufficient. TLS certificate support
is inherited from the exact installed Salts SDK and is not promoted to a
SaltsNet product claim without a SaltsNet protocol test.

The public dependency boundary is provider-neutral:

```text
SaltsNet protocol -> Salts CMeta crypto / CNet TLS -> private provider
```

`vcpkg.json` has no OpenSSL, BoringSSL, or GmSSL dependency. ICE links
`Salts::CNet`/`Salts::CMeta`; SNMP links `Salts::CNet`/`Salts::CMeta`; Email and
LDAP use `Salts::CNet`. No provider structure is present in an installed
SaltsNet header.

## Protocol matrix

| Protocol path | Implemented algorithm/profile | Status | Executable evidence and limits |
| --- | --- | --- | --- |
| STUN / ICE short-term credentials | RFC 5389/8445 `MESSAGE-INTEGRITY` HMAC-SHA-1; CRC-32 fingerprint; CSPRNG transaction IDs/tie breakers | **verified** | `stun`, `stun_cnet`, `stun_request`, `ice`, and `ice_stun_gather` cover construction, validation, mutation, transaction matching, retries, and real loopback UDP. This path does not use X.509 or TLS. |
| TURN long-term credentials | MD5 of `username:realm:password`, then HMAC-SHA-1 `MESSAGE-INTEGRITY`; constant-time comparison | **verified** | `turn` covers authenticated allocation, missing integrity, mutated integrity, bounded attributes, and loopback request/response. This path does not use X.509 or TLS. |
| STUN/TURN SHA-256 integrity and password algorithms from newer extensions | none | **unsupported** | No SHA-256 `MESSAGE-INTEGRITY` attribute or negotiation is exposed. Unsupported input must not be reported as authenticated. |
| SNMPv3 USM authentication | HMAC-MD5-96 and HMAC-SHA-96; RFC 3414 password-to-key and localization | **verified** | `test_snmp_v3` contains published-style deterministic key/localization/authentication vectors and mutation checks; `test_snmp_cnet` performs authenticated UDP exchanges and downgrade rejection. |
| SNMPv3 USM privacy | DES-CBC and AES-128-CFB with provider-neutral CSPRNG salts | **verified** | `test_snmp_v3` covers encrypt/decrypt, capacity, invalid input, unique salts and tampering; `test_snmp_cnet` performs an `authPriv` AES-128 exchange. |
| SNMP USM SHA-2 authentication; AES-192/AES-256 privacy | none | **unsupported** | Public enums admit only MD5/SHA-1 and DES/AES-128. Unknown enum values fail with `USM_ERROR_UNSUPPORTED` or protocol validation failure. |
| SMTP/SMTPS, POP3/POP3S, IMAP/IMAPS | CNet direct TLS and same-connection STARTTLS/STLS, platform trust, peer verification and hostname from the configured host | **implemented; certificate matrix not yet verified** | `test_email_cnet` proves plaintext product exchanges and that invalid STARTTLS is rejected after a real ClientHello. It does not prove a valid root/intermediate/leaf chain, P-384/SHA-384, expiry, constraints, EKU, or mTLS. |
| LDAP/LDAPS | CNet direct TLS, platform trust, peer verification and hostname from the LDAP URL | **implemented; certificate matrix not yet verified** | `test_ldap_cnet` proves plaintext Bind/search and fail-closed invalid LDAPS after a real ClientHello. The positive/negative X.509 profile matrix is not yet a SaltsNet claim. |
| TCP proxy TLS upstream | `tls://` is passed to CNet; no SaltsNet provider or certificate implementation | **not yet verified** | Current proxy tests validate policy/admission only. No valid TLS upstream chain or hostname-negative product test exists. |
| S/MIME | Content-Type detection for enveloped-data, signed-data and `multipart/signed` | **verified for detection only** | `test_mime_smime` verifies bounded MIME classification. PKCS#7 parsing, X.509 validation, signing, signature verification, encryption, and decryption are **unsupported**; detection must not be described as cryptographic verification. |

## TLS and certificate profiles

For SMTP, POP3, IMAP, LDAP, and TLS proxy upstreams, the following remain
**not yet verified in SaltsNet**, even if the same exact Salts SDK has a green
CNet test: RSA-2048/3072, RSA-PSS, P-256/SHA-256, P-384/SHA-384 (including a
mixed P-384 issuer/P-256 leaf), SHA-512 signatures, SM2/SM3, TLS 1.2/1.3
signature/cipher combinations, Ed25519, root/intermediate chain handling,
hostname and IP SAN, validity periods, basic constraints, key usage/EKU,
tampered DER/signatures, and mutual TLS.

This distinction is intentional. SaltsNet protocol clients currently select
CNet's platform trust store and do not expose an independent provider, runtime
provider switch, insecure verification mode, or OpenSSL/BoringSSL fallback.

## Platform and provenance interpretation

The native SDK workflow builds the same public graph on Linux x64/arm64,
Windows x64, macOS arm64, Android arm64, and iOS arm64. Native desktop jobs run
the executable tests; Android/iOS jobs are compile/link/install qualification,
not device-runtime protocol evidence. Each installed SDK contains
`saltsnet-sdk-manifest.txt` with the SaltsNet commit, RID/triplet, and resolved
Salts/SaltsUtils versions. A compatibility report must quote that manifest and
the provider revision recorded by the matching Salts SDK; the semantic version
range alone is not exact provenance.

The unresolved TLS rows are owned by
[qigao/salts#1222](https://github.com/qigao/salts/issues/1222) and
[qigao/salts-net#56](https://github.com/qigao/salts-net/issues/56). They must
stay marked **not yet verified** until real protocol-path positive and negative
tests pass on one coherent installed dependency graph.
