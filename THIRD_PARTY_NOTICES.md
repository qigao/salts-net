# Third-Party Notices

SaltsNet first-party code is licensed under the Apache License 2.0.
The components below retain their upstream license terms.

| Component | Repository path | Upstream license | Notes |
| --- | --- | --- | --- |
| LSQUIC | `lsquic/` | MIT, with BSD-3-Clause portions | See `lsquic/LICENSE` and `lsquic/LICENSE.chrome`. |
| CRoaring | `vendor/croar/` | Apache-2.0 OR MIT | License notices are embedded in the amalgamated source. |
| reed/gf256 | `vendor/reed/` | MIT | See `vendor/reed/LICENSE`. |
| libecc | `vendor/turbo_crypto/libecc/` | BSD-2-Clause OR GPL-2.0-or-later | SaltsNet selects the BSD license for redistribution. |
| Monocypher | `vendor/turbo_crypto/monocypher/` | BSD-2-Clause OR CC0-1.0 | License notices are embedded in the upstream source. |
| sha-2 (SHA-256) | `vendor/turbo_crypto/sha2/` | Unlicense OR 0BSD | See `vendor/turbo_crypto/sha2/LICENSE.md`. |
| xxtea-c | `vendor/turbo_crypto/xxtea/` | MIT | See `vendor/turbo_crypto/xxtea/LICENSE.md`. |

Dependencies downloaded by vcpkg or another package manager are not relicensed
by SaltsNet and remain governed by their respective upstream licenses.
