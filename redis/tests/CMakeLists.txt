cmake_minimum_required(VERSION 3.15)

# Redis client unit tests
add_executable(test_redis_client test_redis_client.c)

target_link_libraries(test_redis_client PRIVATE tedis TurboUtils::TinyTest)

target_include_directories(
  test_redis_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
                            ${CMAKE_SOURCE_DIR}/shared/TurboUtils::TinyTest)

set_target_properties(test_redis_client PROPERTIES FOLDER "redis/tests")

add_test(NAME test_redis_client COMMAND test_redis_client)

# Redis pool unit tests
add_executable(test_redis_pool test_redis_pool.c)
target_compile_definitions(test_redis_pool PRIVATE REDIS_TESTING)

target_link_libraries(test_redis_pool PRIVATE tedis TurboUtils::TinyTest)

target_include_directories(
  test_redis_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
                          ${CMAKE_SOURCE_DIR}/shared/TurboUtils::TinyTest)

set_target_properties(test_redis_pool PROPERTIES FOLDER "redis/tests")

add_test(NAME test_redis_pool COMMAND test_redis_pool)

# Redis cluster unit tests
add_executable(test_redis_cluster test_redis_cluster.c)

target_link_libraries(test_redis_cluster PRIVATE tedis TurboUtils::TinyTest)

target_include_directories(
  test_redis_cluster PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
                             ${CMAKE_SOURCE_DIR}/shared/TurboUtils::TinyTest)

set_target_properties(test_redis_cluster PROPERTIES FOLDER "redis/tests")

add_test(NAME test_redis_cluster COMMAND test_redis_cluster)

# Redis Sentinel unit and protocol tests
add_executable(test_redis_sentinel test_redis_sentinel.c)

target_link_libraries(test_redis_sentinel PRIVATE tedis TurboUtils::TinyTest)

target_include_directories(
  test_redis_sentinel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
                              ${CMAKE_SOURCE_DIR}/shared/TurboUtils::TinyTest)

set_target_properties(test_redis_sentinel PROPERTIES FOLDER "redis/tests")

add_test(NAME test_redis_sentinel COMMAND test_redis_sentinel)
