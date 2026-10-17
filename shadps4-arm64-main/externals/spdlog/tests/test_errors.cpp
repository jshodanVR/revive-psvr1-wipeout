/*
 * This content is released under the MIT License as specified in
 * https://raw.githubusercontent.com/gabime/spdlog/v2.x/LICENSE
 */
#include <iostream>

#include "includes.h"
#include "spdlog/sinks/basic_file_sink.h"

static spdlog::filename_t log_filename = SPDLOG_FILENAME_T("test_logs/simple_log.txt");
static std::string log_err_msg = "Error during log";
static std::string flush_err_msg = "Error during flush";

class failing_sink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg &) override { throw std::runtime_error(log_err_msg.c_str()); }
    void flush_() override { throw std::runtime_error(flush_err_msg.c_str()); }
};
struct custom_ex {};

using namespace spdlog::sinks;
TEST_CASE("default_error_handler", "[errors]") {
    prepare_logdir();
    auto logger = spdlog::create<basic_file_sink_mt>("test-bad-format", log_filename);
    logger->set_pattern("%v");
    logger->info(SPDLOG_FMT_RUNTIME("Test message {} {}"), 1);
    logger->info("Test message {}", 2);
    logger->flush();
    using spdlog::details::os::default_eol;
    REQUIRE(file_contents(log_filename) == spdlog::fmt_lib::format("Test message 2{}", default_eol));
    REQUIRE(count_lines(log_filename) == 1);
}

TEST_CASE("custom_error_handler", "[errors]") {
    prepare_logdir();
    auto logger = spdlog::create<basic_file_sink_mt>("test-format-error", log_filename);
    logger->flush_on(spdlog::level::info);
    logger->set_error_handler([=](const std::string &msg) { REQUIRE(msg == "argument not found"); });
    logger->info("Good message #1");
    REQUIRE_NOTHROW(logger->info(SPDLOG_FMT_RUNTIME("Bad format msg {} {}"), "xxx"));
    logger->info("Good message #2");
    require_message_count(log_filename, 2);
}

TEST_CASE("throwing_sink", "[errors]") {
    auto logger = std::make_shared<spdlog::logger>("test-throwing-sink", std::make_shared<failing_sink>());
    REQUIRE_NOTHROW(logger->info("Some message"));
}

TEST_CASE("throwing_flush", "[errors]") {
    auto logger = spdlog::create<failing_sink>("test-throwing-sink");
    REQUIRE_NOTHROW(logger->flush());
}

TEST_CASE("throwing_error_handler", "[errors]") {
    auto logger = std::make_shared<spdlog::logger>("test-throwing-error-handler", std::make_shared<failing_sink>());
    logger->set_error_handler([=](const std::string &msg) {
        REQUIRE(msg == log_err_msg);
        throw std::runtime_error("test throw");
    });
    REQUIRE_NOTHROW(logger->info("Some message"));
}

TEST_CASE("throwing_flush_error_handler", "[errors]") {
    auto logger = spdlog::create<failing_sink>("test-throwing-error-handler");
    logger->set_error_handler([=](const std::string &msg) {
        REQUIRE(msg == flush_err_msg);
        throw std::runtime_error("test throw");
    });
    REQUIRE_NOTHROW(logger->flush());
}

TEST_CASE("unknown_ex_from_err_handler", "[errors]") {
    auto logger = std::make_shared<spdlog::logger>("test-throwing-error-handler", std::make_shared<failing_sink>());
    logger->set_error_handler([=](const std::string &msg) {
        REQUIRE(msg == log_err_msg);
        throw custom_ex();
    });
    REQUIRE_NOTHROW(logger->info("Some message"));
}

TEST_CASE("unknown_ex_from_flush_err_handler", "[errors]") {
    auto logger = spdlog::create<failing_sink>("test-throwing-error-handler");
    logger->set_error_handler([=](const std::string &msg) {
        REQUIRE(msg == flush_err_msg);
        throw custom_ex();
    });
    REQUIRE_NOTHROW(logger->flush());
}
