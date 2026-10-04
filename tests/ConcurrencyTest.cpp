#include "ConcurrencyTest.h"

#include <cpp-pinyin/Pinyin.h>

#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace Test {
    bool concurrencyTest() {
        const Pinyin::Pinyin converter;
        if (!converter.initialized())
            return false;

        struct Query {
            std::string text;
            Pinyin::ManTone::Style style;
            bool candidates;
        };
        const std::array<Query, 6> queries{{
            {"重庆银行", Pinyin::ManTone::Style::NORMAL, true},
            {"音乐快乐", Pinyin::ManTone::Style::TONE, true},
            {"你好世界", Pinyin::ManTone::Style::TONE2, false},
            {"哪吒", Pinyin::ManTone::Style::TONE3, true},
            {"中国人民", Pinyin::ManTone::Style::NORMAL, false},
            {"重重困难", Pinyin::ManTone::Style::TONE3, true},
        }};

        std::vector<Pinyin::PinyinResVector> expected;
        expected.reserve(queries.size());
        for (const auto &query : queries)
            expected.push_back(converter.hanziToPinyin(query.text, query.style, Pinyin::Error::Default,
                                                        query.candidates));
        const auto defaultExpected = converter.getDefaultPinyin("重", Pinyin::ManTone::Style::TONE3);

        std::atomic<bool> start{false};
        std::atomic<bool> passed{true};
        std::vector<std::thread> workers;
        for (int threadIndex = 0; threadIndex < 8; ++threadIndex) {
            workers.emplace_back([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (int iteration = 0; iteration < 200 && passed.load(); ++iteration) {
                    for (size_t i = 0; i < queries.size(); ++i) {
                        const auto actual = converter.hanziToPinyin(queries[i].text, queries[i].style,
                                                                    Pinyin::Error::Default, queries[i].candidates);
                        if (actual.size() != expected[i].size()) {
                            passed = false;
                            return;
                        }
                        for (size_t j = 0; j < actual.size(); ++j) {
                            if (actual[j].hanzi != expected[i][j].hanzi ||
                                actual[j].pinyin != expected[i][j].pinyin ||
                                actual[j].candidates != expected[i][j].candidates ||
                                actual[j].error != expected[i][j].error) {
                                passed = false;
                                return;
                            }
                        }
                    }
                    if (converter.getDefaultPinyin("重", Pinyin::ManTone::Style::TONE3) != defaultExpected) {
                        passed = false;
                        return;
                    }
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto &worker : workers)
            worker.join();
        return passed;
    }
}
