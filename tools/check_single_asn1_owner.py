#!/usr/bin/env python3
"""Fail-closed SaltsNet source boundary: no local ASN.1/X.509 module."""
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
forbidden_paths = [
    "snmp/asn1",
    "snmp/mib/src/x509_cert.c",
    "snmp/mib/include/asn1",
    "snmp/mib/private/x509_cert.h",
]
errors = [p for p in forbidden_paths if (ROOT / p).exists()]
checks = ["snmp/CMakeLists.txt", "snmp/mib/CMakeLists.txt",
          "tests/installed_sdk/CMakeLists.txt"]
contents = []
for filename in checks:
    path = ROOT / filename
    if not path.is_file():
        errors.append("missing " + filename)
    else:
        contents.append(path.read_text(encoding="utf-8"))
if contents:
    snmp, mib, consumer = contents
    if "add_subdirectory(mib)" not in snmp or "add_subdirectory(asn1)" in snmp:
        errors.append("SNMP must compile only its MIB CLI, never local ASN1")
    if "add_library(" in mib or "SaltsNet::SNMPSupport" in mib or "asn1_parser" in mib:
        errors.append("MIB CLI must not export an independent ASN1 support library")
    if "SNMPSupport)" in consumer or "SNMPSupport " in consumer:
        errors.append("Installed SDK must not require retired SNMPSupport")
    if "Salts::ASN1" not in snmp:
        errors.append("SNMP must link the single imported Salts::ASN1")
for error in errors:
    print("FAIL local ASN.1 source-owner leak: " + error, file=sys.stderr)
if errors:
    sys.exit(1)
print("PASS: SaltsNet has no ASN.1 or X.509 native module; MIB is CLI-only")
