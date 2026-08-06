cmake_minimum_required(VERSION 3.24)

project(WellLogSvgExternalConsumer LANGUAGES CXX)

find_package(WellLog 0.1 CONFIG REQUIRED)

add_executable(welllog_svg_external_consumer main.cpp)
target_link_libraries(
    welllog_svg_external_consumer
    PRIVATE WellLog::ExportVector WellLog::Session
)
target_compile_features(welllog_svg_external_consumer PRIVATE cxx_std_20)

enable_testing()
add_test(NAME external.svg.consumer.runs COMMAND welllog_svg_external_consumer)
