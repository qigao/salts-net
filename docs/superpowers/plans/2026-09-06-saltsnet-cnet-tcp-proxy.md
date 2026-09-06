# SaltsNet CNet TCP Proxy Implementation Plan

1. Define the CMeta protocol enum, bounded configuration, and lifecycle API.
2. Add loopback HTTP CONNECT and SOCKS5 CONNECT tests before enabling TProxy.
3. Implement fixed-session handshake accumulation and CNet upstream connect.
4. Implement copied-admission bidirectional pumping and access/auth failures.
5. Separate and rename the rule engine; connect it through the route boundary.
6. Replace the old example and remove coroutine proxy sources/headers/tests.
7. Enable `SaltsNet::TProxy`, run adjacent LB tests, and scan for CoroNet.
