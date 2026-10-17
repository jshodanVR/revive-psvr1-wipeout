//
// Copyright(c) 2018 Gabi Melman.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)
//

//
// latency.cpp : spdlog latency benchmarks
//

#include "benchmark/benchmark.h"
#include "spdlog/sinks/async_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/null_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/spdlog.h"

using namespace spdlog::sinks;
void bench_c_string(benchmark::State &state, std::shared_ptr<spdlog::logger> logger) {
    const char *msg =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Vestibulum pharetra metus cursus "
        "lacus placerat congue. Nulla egestas, mauris a tincidunt tempus, enim lectus volutpat mi, "
        "eu consequat sem "
        "libero nec massa. In dapibus ipsum a diam rhoncus gravida. Etiam non dapibus eros. Donec "
        "fringilla dui sed "
        "augue pretium, nec scelerisque est maximus. Nullam convallis, sem nec blandit maximus, "
        "nisi turpis ornare "
        "nisl, sit amet volutpat neque massa eu odio. Maecenas malesuada quam ex, posuere congue "
        "nibh turpis duis.";

    for (auto _ : state) {
        logger->info(msg);
    }
}

void bench_logger(benchmark::State &state, std::shared_ptr<spdlog::logger> logger) {
    int i = 0;
    for (auto _ : state) {
        logger->info("Hello logger: msg number {}...............", ++i);
    }
}

void bench_global_logger(benchmark::State &state, std::shared_ptr<spdlog::logger> logger) {
    spdlog::set_global_logger(std::move(logger));
    int i = 0;
    for (auto _ : state) {
        spdlog::info("Hello logger: msg number {}...............", ++i);
    }
}

void bench_disabled_macro(benchmark::State &state, std::shared_ptr<spdlog::logger> logger) {
    int i = 0;
    benchmark::DoNotOptimize(i);       // prevent unused warnings
    benchmark::DoNotOptimize(logger);  // prevent unused warnings
    for (auto _ : state) {
        SPDLOG_LOGGER_DEBUG(logger, "Hello logger: msg number {}...............", i++);
    }
}

void bench_disabled_macro_global_logger(benchmark::State &state, std::shared_ptr<spdlog::logger> logger) {
    spdlog::set_global_logger(std::move(logger));
    int i = 0;
    benchmark::DoNotOptimize(i);       // prevent unused warnings
    benchmark::DoNotOptimize(logger);  // prevent unused warnings
    for (auto _ : state) {
        SPDLOG_DEBUG("Hello logger: msg number {}...............", i++);
    }
}

#ifdef __linux__

void bench_dev_null() {
    auto dev_null_st = spdlog::create<basic_file_sink_st>("/dev/null_st", "/dev/null");
    benchmark::RegisterBenchmark("/dev/null_st", bench_logger, std::move(dev_null_st))->UseRealTime();

    auto dev_null_mt = spdlog::create<basic_file_sink_mt>("/dev/null_mt", "/dev/null");
    benchmark::RegisterBenchmark("/dev/null_mt", bench_logger, std::move(dev_null_mt))->UseRealTime();
}

#endif  // __linux__

// test spdlog::get() performance
// for this test we create multiple null loggers and then call spdlog::get() on one of them multiple times
// create multiple null loggers and return name of the one to test
static std::string prepare_null_loggers() {
    const std::string some_logger_name = "Some logger name";
    const int null_logger_count = 9;
    for (int i = 0; i < null_logger_count; i++) {
        spdlog::create<null_sink_mt>(some_logger_name + std::to_string(i));
    }
    return some_logger_name + std::to_string(null_logger_count / 2);
}

int main(int argc, char *argv[]) {
    using spdlog::sinks::null_sink_mt;
    using spdlog::sinks::null_sink_st;

    size_t file_size = 30 * 1024 * 1024;
    size_t rotating_files = 5;
    int n_threads = benchmark::CPUInfo::Get().num_cpus;

    auto full_bench = argc > 1 && std::string(argv[1]) == "full";

    // disabled loggers
    auto disabled_logger = std::make_shared<spdlog::logger>("bench", std::make_shared<null_sink_mt>());
    disabled_logger->set_level(spdlog::level::off);
    benchmark::RegisterBenchmark("disabled-at-compile-time", bench_disabled_macro, disabled_logger);
    benchmark::RegisterBenchmark("disabled-at-compile-time (global logger)", bench_disabled_macro_global_logger, disabled_logger);
    benchmark::RegisterBenchmark("disabled-at-runtime", bench_logger, disabled_logger);
    benchmark::RegisterBenchmark("disabled-at-runtime (global logger)", bench_global_logger, disabled_logger);

    auto null_logger_st = std::make_shared<spdlog::logger>("bench", std::make_shared<null_sink_st>());
    benchmark::RegisterBenchmark("null_sink_st (500_bytes c_str)", bench_c_string, std::move(null_logger_st));
    benchmark::RegisterBenchmark("null_sink_st", bench_logger, null_logger_st);
    benchmark::RegisterBenchmark("null_sink_st (global logger)", bench_global_logger, null_logger_st);

#ifdef __linux__
    bench_dev_null();
#endif  // __linux__

    if (full_bench) {
        // basic_st
        auto basic_st = spdlog::create<basic_file_sink_st>("basic_st", "latency_logs/basic_st.log", true);
        benchmark::RegisterBenchmark("basic_st", bench_logger, std::move(basic_st))->UseRealTime();

        // rotating st
        auto rotating_st =
            spdlog::create<rotating_file_sink_st>("rotating_st", "latency_logs/rotating_st.log", file_size, rotating_files);
        benchmark::RegisterBenchmark("rotating_st", bench_logger, std::move(rotating_st))->UseRealTime();

        // daily st
        auto daily_st = spdlog::create<daily_file_sink_st>("daily_st", "latency_logs/daily_st.log", 0, 1);
        benchmark::RegisterBenchmark("daily_st", bench_logger, std::move(daily_st))->UseRealTime();

        //
        // Multi threaded bench, 10 loggers using same logger concurrently
        //
        auto null_logger_mt = std::make_shared<spdlog::logger>("bench", std::make_shared<null_sink_mt>());
        benchmark::RegisterBenchmark("null_sink_mt", bench_logger, null_logger_mt)->Threads(n_threads)->UseRealTime();

        // basic_mt
        auto basic_mt = spdlog::create<basic_file_sink_mt>("basic_mt", "latency_logs/basic_mt.log", true);
        benchmark::RegisterBenchmark("basic_mt", bench_logger, std::move(basic_mt))->Threads(n_threads)->UseRealTime();

        // rotating mt
        auto rotating_mt =
            spdlog::create<rotating_file_sink_mt>("rotating_mt", "latency_logs/rotating_mt.log", file_size, rotating_files);
        benchmark::RegisterBenchmark("rotating_mt", bench_logger, std::move(rotating_mt))->Threads(n_threads)->UseRealTime();

        // daily mt
        auto daily_mt = spdlog::create<daily_file_sink_mt>("daily_mt", "latency_logs/daily_mt.log", 0, 1);
        benchmark::RegisterBenchmark("daily_mt", bench_logger, std::move(daily_mt))->Threads(n_threads)->UseRealTime();
    }
    using spdlog::sinks::async_sink;
    async_sink::config config;
    config.queue_size = async_sink::default_queue_size;;

    config.sinks.push_back(std::make_shared<null_sink_st>());
    config.policy = async_sink::overflow_policy::overrun_oldest;
    auto async_logger = std::make_shared<spdlog::logger>("async_logger", std::make_shared<async_sink>(config));
    benchmark::RegisterBenchmark("async_logger", bench_logger, async_logger)->Threads(n_threads)->UseRealTime();

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
}
