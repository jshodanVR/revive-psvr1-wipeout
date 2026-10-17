#ifdef SPDLOG_NO_SOURCE_LOC
    #undef SPDLOG_NO_SOURCE_LOC
#endif

#include "includes.h"
#include "test_sink.h"

// test with source location
TEST_CASE("test_source_location", "[source_location]") {
    auto test_sink = std::make_shared<spdlog::sinks::test_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("test", test_sink);
    logger->set_pattern("%s:%# %v");
    // test with source location with parameters
    SPDLOG_LOGGER_CALL(logger, spdlog::level::info, "Hello {}", "source location");
    REQUIRE(test_sink->lines().size() == 1);
    REQUIRE(test_sink->lines()[0] == "test_source_location.cpp:14 Hello source location");
    // test with source location without parameters
    SPDLOG_LOGGER_CALL(logger, spdlog::level::info, "Hello");
    REQUIRE(test_sink->lines().size() == 2);
    REQUIRE(test_sink->lines()[1] == "test_source_location.cpp:18 Hello");
}
