"""Validate the native package before publishing an already-built artifact."""

import hashlib
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify(path, version, commit):
    rids = {"linux-x64", "linux-arm64", "windows-x64", "macos-arm64",
            "android-arm64-v8a", "ios-arm64"}
    require(path.name == f"SaltsNet.Native.{version}.nupkg", "Package filename/version mismatch")
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "Expected a full source commit")
    with zipfile.ZipFile(path) as package:
        names = package.namelist()
        require(len(names) == len(set(names)), "Duplicate package entries")
        metadata = ET.fromstring(package.read("SaltsNet.Native.nuspec")).find("{*}metadata")
        require(metadata is not None, "Missing NuGet metadata")
        require(metadata.findtext("{*}id") == "SaltsNet.Native", "Wrong package identity")
        require(metadata.findtext("{*}version") == version, "NuGet version mismatch")
        repository = metadata.find("{*}repository")
        require(repository is not None and repository.get("commit") == commit,
                "NuGet source commit mismatch")
        require(not metadata.findall(".//{*}dependency"), "Unexpected package dependencies")
        expected = {f"sdk/{rid}/saltsnet-sdk-manifest.txt" for rid in rids}
        actual = {name for name in names if name.endswith("saltsnet-sdk-manifest.txt")}
        require(actual == expected, "Expected exactly the six qualified SDK manifests")
        dependency_pairs = set()
        for rid in sorted(rids):
            content = package.read(f"sdk/{rid}/saltsnet-sdk-manifest.txt").decode("utf-8-sig")
            fields = {}
            for line in content.splitlines():
                key, value = line.split("=", 1)
                require(key not in fields, f"Duplicate {rid} manifest key: {key}")
                fields[key] = value
            for key, value in (("version", version), ("commit", commit),
                               ("rid", rid), ("build_type", "Release")):
                require(fields.get(key) == value, f"{rid}: {key} mismatch")
            require(re.fullmatch(r"2\.3\.0(?:-rc\.[1-9][0-9]*)?", fields.get("salts", "")),
                    f"{rid}: unsupported Salts release")
            require(re.fullmatch(r"4\.3\.0(?:-rc\.[1-9][0-9]*)?", fields.get("salts_utils", "")),
                    f"{rid}: unsupported SaltsUtils release")
            dependency_pairs.add((fields["salts"], fields["salts_utils"]))
            require(f"sdk/{rid}/lib/cmake/SaltsNet/SaltsNetConfig.cmake" in names,
                    f"{rid}: missing installed CMake package")
        require(len(dependency_pairs) == 1, "SDKs use different dependency pairs")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    print(f"Verified {version}, commit {commit}, six Release SDKs, dependencies {dependency_pairs.pop()}")
    print(f"SHA256 {digest}  {path.name}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: verify-release-package.py PACKAGE VERSION COMMIT")
    try:
        verify(Path(sys.argv[1]), sys.argv[2], sys.argv[3])
    except (OSError, ValueError, KeyError, ET.ParseError, zipfile.BadZipFile) as error:
        sys.exit(f"Release package rejected: {error}")
