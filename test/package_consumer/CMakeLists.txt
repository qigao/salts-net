cmake_minimum_required(VERSION 3.20)
project(SaltsNetPackageConsumerTest C)

if(NOT DEFINED SALTSNET_PACKAGE_ROOT OR
   NOT IS_DIRECTORY "${SALTSNET_PACKAGE_ROOT}")
  message(FATAL_ERROR
          "SALTSNET_PACKAGE_ROOT must name the staged SaltsNet install prefix")
endif()
if(DEFINED SALTSNET_EXPECT_PREFIX_COUNT)
  set(saltsnet_expected_prefix_path)
  math(EXPR saltsnet_last_prefix "${SALTSNET_EXPECT_PREFIX_COUNT} - 1")
  foreach(saltsnet_prefix_index RANGE 0 ${saltsnet_last_prefix})
    set(saltsnet_expected_name
        "SALTSNET_EXPECT_PREFIX_${saltsnet_prefix_index}")
    if(NOT DEFINED ${saltsnet_expected_name})
      message(FATAL_ERROR
              "Missing expected prefix item: ${saltsnet_expected_name}")
    endif()
    set(saltsnet_expected_prefix "${${saltsnet_expected_name}}")
    list(APPEND saltsnet_expected_prefix_path "${saltsnet_expected_prefix}")
  endforeach()
  get_property(saltsnet_cached_prefix_path
               CACHE CMAKE_PREFIX_PATH PROPERTY VALUE)
  if(NOT "${saltsnet_cached_prefix_path}" STREQUAL
         "${saltsnet_expected_prefix_path}")
    message(FATAL_ERROR
            "CMAKE_PREFIX_PATH changed in transit: expected "
            "${saltsnet_expected_prefix_path}, got ${saltsnet_cached_prefix_path}")
  endif()
endif()

find_package(SaltsNet CONFIG REQUIRED
             PATHS "${SALTSNET_PACKAGE_ROOT}" NO_DEFAULT_PATH)

add_executable(saltsnet_package_consumer_test package_consumer_test.c)
set_target_properties(saltsnet_package_consumer_test
                      PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(
  saltsnet_package_consumer_test
  PRIVATE SaltsNet::Crypto
          SaltsNet::Asn1Parser
          SaltsNet::SNMP
          SaltsNet::ICE
          SaltsNet::LDAP
          SaltsNet::UriParser
          SaltsNet::MimeParser
          SaltsNet::Email
          SaltsNet::LB
          SaltsNet::TCPProxy)

if(SALTSNET_EXPECT_LSQUIC)
  foreach(saltsnet_lsquic_target IN ITEMS SaltsNet::LSQUIC SaltsNet::LSQUICCore)
    if(NOT TARGET ${saltsnet_lsquic_target})
      message(FATAL_ERROR
              "Enabled LSQUIC export is missing target: ${saltsnet_lsquic_target}")
    endif()
  endforeach()
  target_compile_definitions(saltsnet_package_consumer_test
                             PRIVATE SALTSNET_PACKAGE_HAS_LSQUIC=1)
  target_link_libraries(saltsnet_package_consumer_test
                        PRIVATE SaltsNet::LSQUIC SaltsNet::LSQUICCore)
elseif(TARGET SaltsNet::LSQUIC OR TARGET SaltsNet::LSQUICCore)
  message(FATAL_ERROR "Disabled LSQUIC unexpectedly exported a package target")
endif()
