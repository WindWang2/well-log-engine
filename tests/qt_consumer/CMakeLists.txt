cmake_minimum_required(VERSION 3.24)

project(WellLogQtExternalConsumer LANGUAGES CXX)

find_package(WellLog 0.1 CONFIG REQUIRED)

add_executable(welllog_qt_external_consumer main.cpp)
target_link_libraries(
    welllog_qt_external_consumer
    PRIVATE WellLog::QtWidgets Qt6::Widgets
)
target_compile_features(welllog_qt_external_consumer PRIVATE cxx_std_20)

enable_testing()
add_test(NAME external.qt.consumer.runs COMMAND welllog_qt_external_consumer)
