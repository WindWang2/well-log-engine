cmake_minimum_required(VERSION 3.24)

project(WellLogExternalConsumer LANGUAGES CXX)

find_package(WellLog 0.1 CONFIG REQUIRED)

add_executable(welllog_external_consumer main.cpp)
target_link_libraries(
    welllog_external_consumer
    PRIVATE WellLog::Session WellLog::IO
)
target_compile_features(welllog_external_consumer PRIVATE cxx_std_20)

enable_testing()
add_test(NAME external.consumer.runs COMMAND welllog_external_consumer)
