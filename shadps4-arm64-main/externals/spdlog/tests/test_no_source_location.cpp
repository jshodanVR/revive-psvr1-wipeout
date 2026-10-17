#ifndef SPDLOG_NO_SOURCE_LOC
    #define SPDLOG_NO_SOURCE_LOC
#endif

#include "includes.h"
#include "test_sink.h"

// no source location should appear in the log message
TEST_CASE("test_no_source_location", "[source_location]") {
    auto test_sink = std::make_shared<spdlog::sinks::test_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("test", test_sink);
    logger->set_pattern("%s:%#:%! %v");

    // test with no source location with parameters
    SPDLOG_LOGGER_CALL(logger, spdlog::level::info, "Hello {}", "no source location");
    REQUIRE(test_sink->lines().size() == 1);
    REQUIRE(test_sink->lines()[0] == ":: Hello no source location");
    // test with no source location without parameters
    SPDLOG_LOGGER_CALL(logger, spdlog::level::info, "Hello");
    REQUIRE(test_sink->lines().size() == 2);
    REQUIRE(test_sink->lines()[1] == ":: Hello");
}
