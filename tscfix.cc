/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * See LICENSE file for licensing.
 */

#include <cstdint>
#include <atomic>
#include <vector>
#include <thread>
#include <cassert>
#include <limits>
#include <iostream>
#include <string>
#include <sstream>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>

static const uint64_t max_latency = 160;
static const int64_t allowed_drift = 100;

inline
uint64_t rdtscp() {
    uint64_t a, c, d;
    asm volatile("rdtscp" : "=a"(a), "=c"(c), "=d"(d));
    asm volatile("lfence");
    return a | (d << 32);
}

void set_affinity(unsigned cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    int r = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    assert(r == 0);
}

struct test {
    uint64_t seq1;
    uint64_t tsc;
    uint64_t seq2;
};

struct result {
    int64_t drift;
    uint64_t error;
};

struct calibrator {
    std::atomic<uint64_t> seq;

    std::vector<uint64_t> calibration;
    std::vector<test> tests;

    void run_test(uint64_t count);
    void run_calibration(uint64_t count);
    void reset(uint64_t count);
    result analyze();
    result measure(unsigned cpu);

};

void calibrator::run_test(uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        auto seq1 = seq.load(std::memory_order_relaxed);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        auto tsc = rdtscp();
        std::atomic_signal_fence(std::memory_order_seq_cst);
        auto seq2 = seq.load(std::memory_order_relaxed);
        tests.push_back({seq1, tsc, seq2});
    }
}

void calibrator::run_calibration(uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
        seq.fetch_add(1, std::memory_order_relaxed);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        auto tsc = rdtscp();
        std::atomic_signal_fence(std::memory_order_seq_cst);
        calibration.push_back(tsc);
    }
}

void calibrator::reset(uint64_t count) {
    calibration.clear();
    calibration.reserve(count);
    tests.clear();
    tests.reserve(count);
    seq.store(0, std::memory_order_seq_cst);
}

result calibrator::analyze() {
    uint64_t best_error = std::numeric_limits<uint64_t>::max();
    int64_t best_value = 0;
    for (auto&& e : tests) {
        if (e.seq1 == 0) {
            continue;
        }
        auto tsc1 = calibration[e.seq1 - 1];
        auto tsc = e.tsc;
        auto tsc2 = calibration[e.seq2];
        auto error = tsc2 - tsc1;
        auto average = (tsc1 + tsc2) / 2;
        int64_t value = tsc - average;
        if (error < best_error) {
            best_error = error;
            best_value = value;
        }
    }
    return { best_value, best_error };
}


result calibrator::measure(unsigned cpu) {
    uint64_t n = 10000;
    while (true) {
        reset(n);
        std::thread w([=] {
            set_affinity(cpu);
            run_test(n);
        });
        run_calibration(n);
        w.join();
        auto res = analyze();
        if (res.error <= max_latency) {
            return res;
        }
        n *= 2;
    }
}

void adjust_tsc(int cpu, int64_t delta) {
    std::thread t([=] {
        set_affinity(cpu);
#if 0
        std::ostringstream fname;
        fname << "/dev/cpu/" << cpu << "/msr";
        int fd = ::open(fname.str().c_str(), O_RDWR);
        ::lseek(fd, 0x10, SEEK_SET);
        uint64_t val = rdtscp() + delta;
        ::write(fd, &val, 8);
        ::close(fd);
#else
        int fd = ::open("/dev/tscadj", O_WRONLY);
        if (fd == -1) {
            std::cout << "unable to open /dev/tscadj\n";
            exit(1);
        }
        ::write(fd, &delta, 8);
        ::close(fd);
#endif
    });
    t.join();
}

int main(int ac, char** av) {
    set_affinity(0);
    calibrator c;
    auto fix = ac > 1 && std::string(av[1]) == "-f";
    for (unsigned cpu = 1; cpu < std::thread::hardware_concurrency(); ++cpu) {
        auto res = c.measure(cpu);
        if (fix) {
            adjust_tsc(cpu, -res.drift);
            // Simple PID controller to attempt to reduce residual drift,
            // accounting for adjust_tsc() latency
            auto kp = 0.1;
            auto ki = 0.001;
            auto kd = 0.1;
            auto sum = 0.0;
            int64_t last = 0;
            while (true) {
                auto res = c.measure(cpu);
                if (std::abs(res.drift) < allowed_drift && res.drift >= 0) {
                    break;
                }
                sum += res.drift;
                auto diff = res.drift - last;
                last = res.drift;
                auto cmd = kp * res.drift + ki * sum + kd * diff;
                adjust_tsc(cpu, -cmd);
            }
        } else {
            std::cout << "cpu " << cpu << " drift " << res.drift
                      << " error " << res.error << "\n";
        }
    }
}
