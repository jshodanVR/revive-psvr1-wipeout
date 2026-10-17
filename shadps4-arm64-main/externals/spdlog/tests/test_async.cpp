#include <tuple>

#include "includes.h"
#include "spdlog/sinks/async_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "test_sink.h"

#define TEST_FILENAME "test_logs/async_test.log"

using spdlog::sinks::async_sink;
using spdlog::sinks::sink;
using spdlog::sinks::test_sink_mt;
using namespace std::chrono_literals;

auto creat_async_logger(size_t queue_size, std::shared_ptr<sink> backend_sink) {
    async_sink::config cfg;
    cfg.queue_size = queue_size;
    cfg.sinks.push_back(std::move(backend_sink));
    auto s = std::make_shared<async_sink>(cfg);
    auto logger = std::make_shared<spdlog::logger>("async_logger", s);
    return std::make_tuple(logger, s);
}

TEST_CASE("basic async test ", "[async]") {
    const auto test_sink = std::make_shared<test_sink_mt>();
    size_t overrun_counter = 0;
    size_t messages = 256;
    {
        constexpr size_t queue_size = 16;
        auto [logger, async_sink] = creat_async_logger(queue_size, test_sink);
        for (size_t i = 0; i < messages; i++) {
            logger->info("Hello message #{}", i);
        }
        logger->flush();
        overrun_counter = async_sink->get_overrun_counter();
    }
    // logger and async_sink are destroyed here so the queue should be emptied
    REQUIRE(test_sink->msg_counter() == messages);
    REQUIRE(test_sink->flush_counter() == 1);
    REQUIRE(overrun_counter == 0);
}

TEST_CASE("discard policy ", "[async]") {
    auto test_sink = std::make_shared<test_sink_mt>();
    test_sink->set_delay(1ms);
    async_sink::config config;
    config.queue_size = 4;
    config.policy = async_sink::overflow_policy::overrun_oldest;
    config.sinks.push_back(test_sink);
    size_t messages = 1024;
    auto as = std::make_shared<async_sink>(config);
    auto logger = std::make_shared<spdlog::logger>("async_logger", as);
    REQUIRE(as->get_discard_counter() == 0);
    REQUIRE(as->get_overrun_counter() == 0);
    for (size_t i = 0; i < messages; i++) {
        logger->info("Hello message");
    }
    REQUIRE(test_sink->msg_counter() < messages);
    REQUIRE(as->get_overrun_counter() > 0);
    as->reset_overrun_counter();
    REQUIRE(as->get_overrun_counter() == 0);
}

TEST_CASE("discard policy discard_new ", "[async]") {
    auto test_sink = std::make_shared<test_sink_mt>();
    test_sink->set_delay(1ms);
    async_sink::config config;
    config.queue_size = 4;
    config.policy = async_sink::overflow_policy::discard_new;
    config.sinks.push_back(test_sink);
    size_t messages = 1024;
    auto as = std::make_shared<async_sink>(config);
    auto logger = std::make_shared<spdlog::logger>("async_logger", as);

    REQUIRE(as->get_config().policy == async_sink::overflow_policy::discard_new);
    REQUIRE(as->get_discard_counter() == 0);
    REQUIRE(as->get_overrun_counter() == 0);
    for (size_t i = 0; i < messages; i++) {
        logger->info("Hello message");
    }
    REQUIRE(test_sink->msg_counter() < messages);
    REQUIRE(as->get_discard_counter() > 0);
    as->reset_discard_counter();
    REQUIRE(as->get_discard_counter() == 0);
}

TEST_CASE("flush", "[async]") {
    auto test_sink = std::make_shared<test_sink_mt>();
    size_t messages = 256;
    {
        constexpr size_t queue_size = 256;
        auto [logger, async_sink] = creat_async_logger(queue_size, test_sink);
        for (size_t i = 0; i < messages; i++) {
            logger->info("Hello message #{}", i);
        }
        logger->flush();
    }
    REQUIRE(test_sink->msg_counter() == messages);
    REQUIRE(test_sink->flush_counter() == 1);
}

TEST_CASE("wait_dtor ", "[async]") {
    auto test_sink = std::make_shared<test_sink_mt>();
    test_sink->set_delay(5ms);
    async_sink::config config;
    config.sinks.push_back(test_sink);
    config.queue_size = 4;
    config.policy = async_sink::overflow_policy::block;
    size_t messages = 100;
    {
        auto as = std::make_shared<async_sink>(config);
        auto logger = std::make_shared<spdlog::logger>("async_logger", as);
        for (size_t i = 0; i < messages; i++) {
            logger->info("Hello message #{}", i);
        }
        logger->flush();
        REQUIRE(as->get_overrun_counter() == 0);
        REQUIRE(as->get_discard_counter() == 0);
    }

    REQUIRE(test_sink->msg_counter() == messages);
    REQUIRE(test_sink->flush_counter() == 1);
}

TEST_CASE("multi threads", "[async]") {
    auto test_sink = std::make_shared<spdlog::sinks::test_sink_mt>();
    size_t messages = 256;
    size_t n_threads = 10;
    {
        constexpr size_t queue_size = 128;
        auto [logger, async_sink] = creat_async_logger(queue_size, test_sink);

        std::vector<std::thread> threads;
        for (size_t i = 0; i < n_threads; i++) {
            threads.emplace_back([l = logger, msgs = messages] {
                for (size_t j = 0; j < msgs; j++) {
                    l->info("Hello message #{}", j);
                }
            });
            logger->flush();
        }

        for (auto &t : threads) {
            t.join();
        }
    }
    REQUIRE(test_sink->msg_counter() == messages * n_threads);
    REQUIRE(test_sink->flush_counter() == n_threads);
}

TEST_CASE("to_file", "[async]") {
    prepare_logdir();
    size_t messages = 1024;
    {
        spdlog::filename_t filename = SPDLOG_FILENAME_T(TEST_FILENAME);
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, true);
        auto [logger, async_sink] = creat_async_logger(messages, file_sink);

        for (size_t j = 0; j < messages; j++) {
            logger->info("Hello message #{}", j);
        }
    }
    require_message_count(TEST_FILENAME, messages);
    auto contents = file_contents(TEST_FILENAME);
    using spdlog::details::os::default_eol;
    REQUIRE(ends_with(contents, spdlog::fmt_lib::format("Hello message #1023{}", default_eol)));
}

TEST_CASE("bad_ctor", "[async]") {
    async_sink::config cfg;
    cfg.queue_size = 0;
    REQUIRE_THROWS_AS(std::make_shared<async_sink>(cfg), spdlog::spdlog_ex);
}

TEST_CASE("bad_ctor2", "[async]") {
    async_sink::config cfg;
    cfg.queue_size = async_sink::max_queue_size + 1;
    REQUIRE_THROWS_AS(std::make_shared<async_sink>(cfg), spdlog::spdlog_ex);
}

TEST_CASE("start_stop_clbks", "[async]") {
    bool start_called = false;
    bool stop_called = false;
    {
        async_sink::config cfg;
        cfg.on_thread_start = [&] { start_called = true; };
        cfg.on_thread_stop = [&] { stop_called = true; };
        auto sink = std::make_shared<async_sink>(cfg);
    }
    REQUIRE(start_called);
    REQUIRE(stop_called);
}

TEST_CASE("start_stop_clbks2", "[async]") {
    bool start_called = false;
    bool stop_called = false;
    {
        async_sink::config cfg;
        cfg.on_thread_start = [&] { start_called = true; };
        auto sink = std::make_shared<async_sink>(cfg);
    }
    REQUIRE(start_called);
    REQUIRE_FALSE(stop_called);
}

TEST_CASE("start_stop_clbks3", "[async]") {
    bool start_called = false;
    bool stop_called = false;
    {
        async_sink::config cfg;
        cfg.on_thread_start = nullptr;
        cfg.on_thread_stop = [&] { stop_called = true; };
        auto sink = std::make_shared<async_sink>(cfg);
    }
    REQUIRE_FALSE(start_called);
    REQUIRE(stop_called);
}

TEST_CASE("start_stop_clbks4", "[async]") {
    bool start_called = false;
    bool stop_called = false;
    {
        async_sink::config cfg;
        cfg.on_thread_start = [&] { start_called = true; };
        cfg.on_thread_stop = [&] { stop_called = true; };
        cfg.queue_size = 128;
        auto sink = std::make_shared<async_sink>(cfg);
    }
    REQUIRE(start_called);
    REQUIRE(stop_called);
}

// should not start threads if queue size is invalid
TEST_CASE("start_stop_clbks5", "[async]") {
    bool start_called = false;
    bool stop_called = false;
    {
        async_sink::config cfg;
        cfg.on_thread_start = [&] { start_called = true; };
        cfg.on_thread_stop = [&] { stop_called = true; };
        cfg.queue_size = 0;
        REQUIRE_THROWS_AS(std::make_shared<async_sink>(cfg), spdlog::spdlog_ex);
    }
    REQUIRE_FALSE(start_called);
    REQUIRE_FALSE(stop_called);
}

TEST_CASE("multi-sinks", "[async]") {
    prepare_logdir();
    auto test_sink1 = std::make_shared<test_sink_mt>();
    auto test_sink2 = std::make_shared<test_sink_mt>();
    auto test_sink3 = std::make_shared<test_sink_mt>();
    size_t messages = 1024;
    {
        async_sink::config cfg;
        cfg.sinks.push_back(test_sink1);
        cfg.sinks.push_back(test_sink2);
        cfg.sinks.push_back(test_sink3);
        auto as = std::make_shared<async_sink>(cfg);
        spdlog::logger l("async_logger", as);

        for (size_t j = 0; j < messages; j++) {
            l.info("Hello message #{}", j);
        }
    }
    REQUIRE(test_sink1->msg_counter() == messages);
    REQUIRE(test_sink2->msg_counter() == messages);
    REQUIRE(test_sink3->msg_counter() == messages);
}

TEST_CASE("level-off", "[async]") {
    const auto test_sink = std::make_shared<test_sink_mt>();
    test_sink->set_level(spdlog::level::critical);
    {
        constexpr size_t messages = 256;
        constexpr size_t queue_size = 16;
        auto [logger, async_sink] = creat_async_logger(queue_size, test_sink);
        logger->flush_on(spdlog::level::critical);
        for (size_t i = 0; i < messages; i++) {
            logger->info("Hello message #{}", i);
        }
    }
    // logger and async_sink are destroyed here so the queue should be emptied
    REQUIRE(test_sink->msg_counter() == 0);
    REQUIRE(test_sink->flush_counter() == 0);
}

TEST_CASE("backend_ex", "[async]") {
    const auto test_sink = std::make_shared<test_sink_mt>();
    test_sink->set_exception(std::runtime_error("test backend exception"));
    constexpr size_t queue_size = 16;
    auto [logger, async_sink] = creat_async_logger(queue_size, test_sink);
    REQUIRE_NOTHROW(logger->info("Hello message"));
    REQUIRE_NOTHROW(logger->flush());
}

// test async custom error handler. trigger it using a backend exception and make sure it's called
TEST_CASE("custom_err_handler", "[async]") {
    bool error_called = false;
    auto test_sink = std::make_shared<test_sink_mt>();
    test_sink->set_exception(std::runtime_error("test backend exception"));
    async_sink::config config;
    config.sinks.push_back(std::move(test_sink));
    config.custom_err_handler = [&error_called](const std::string &) { error_called = true; };
    auto asink = std::make_shared<async_sink>(config);
    spdlog::logger("async_logger", std::move(asink)).info("Test");
    // lvalue logger so will be destructed here already so all messages were processed
    REQUIRE(error_called);
}

// test wait_all
TEST_CASE("wait_all", "[async]") {
    auto test_sink = std::make_shared<test_sink_mt>();
    auto delay = 10ms;
    test_sink->set_delay(delay);
    async_sink::config config;
    config.sinks.push_back(test_sink);
    size_t messages = 10;
    auto as = std::make_shared<async_sink>(config);
    auto logger = std::make_shared<spdlog::logger>("async_logger", as);
    for (size_t i = 0; i < messages; i++) {
        logger->info("Hello message");
    }
    REQUIRE_FALSE(as->wait_all(-10ms));
    REQUIRE_FALSE(as->wait_all(0ms));
    auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(as->wait_all(delay));

    // should have waited approx 10ms before giving up
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed >= delay);
    REQUIRE(elapsed < delay *  6); // big tolerance, to pass tests in slow virtual machines
    // wait enough time for all messages to be processed
    REQUIRE(as->wait_all(messages * delay + 500ms));
    REQUIRE(as->wait_all(-10ms));  // no more messages
    REQUIRE(as->wait_all(0ms));    // no more messages
    REQUIRE(as->wait_all(10ms));   // no more messages
}

// test wait_all without timeout
TEST_CASE("wait_all2", "[async]") {
    auto test_sink = std::make_shared<test_sink_mt>();
    auto delay = 10ms;
    test_sink->set_delay(delay);
    async_sink::config config;
    config.sinks.push_back(test_sink);
    size_t messages = 10;
    auto as = std::make_shared<async_sink>(config);
    auto logger = std::make_shared<spdlog::logger>("async_logger", as);
    for (size_t i = 0; i < messages; i++) {
        logger->info("Hello message");
    }
    as->wait_all();
    REQUIRE(test_sink->msg_counter() == messages);
}
