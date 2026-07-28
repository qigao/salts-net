# TurboNet embeds c-ares in CoroNet and must not acquire a runtime c-ares DLL.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO c-ares/c-ares
    REF "v${VERSION}"
    SHA512 5c6b4422e158cef2943f7066fb8c738d9ac6f470cdb3ca5cf2b9fa26494f4fb1d7fef25a73d59d9f12aa8eaadc1da358c889d84ac8703b7e430134310bda45ba
    HEAD_REF main
    PATCHES
        avoid-docs.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tool CARES_BUILD_TOOLS
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DCARES_STATIC=ON
        -DCARES_SHARED=OFF
        -DCARES_STATIC_PIC=ON
        -DCARES_BUILD_TESTS=OFF
        -DCARES_BUILD_CONTAINER_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/c-ares)
vcpkg_fixup_pkgconfig()

vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/include/ares.h"
    "#  ifdef CARES_STATICLIB" "#if 1"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")

if("tool" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES adig ahost AUTO_CLEAN)
endif()
