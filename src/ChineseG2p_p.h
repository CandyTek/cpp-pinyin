#ifndef ChineseG2pPRIVATE_H
#define ChineseG2pPRIVATE_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cpp-pinyin/ToneConverter.h>

#include "cpp-pinyin/U16Str.h"

namespace Pinyin
{
    // ---- 紧凑字典 ----
    // 词典加载后只读，所以不用 unordered_map<..., vector<u16string>>（2 万个单字 + 1 万多短语合计约 5 万次堆分配、
    // 3 MB 常驻），而是排序键数组 + 一段连续的字符池：常驻内存约为原来的 1/5，查找用二分，没有指针跳转，不比哈希慢。
    // 候选拼音在池里以 u'\0' 分隔，一个条目对应池里的 [begin, end) 区间。

    // 一个条目的候选拼音列表（指向字典内部的字符池，字典存活期间有效）
    struct CandidateList {
        const char16_t *begin = nullptr;
        const char16_t *end = nullptr;

        bool empty() const { return begin == end; }

        template <class Fn>
        void forEach(Fn &&fn) const {
            const char16_t *p = begin;
            while (p < end) {
                const char16_t *q = p;
                while (q < end && *q) ++q;
                fn(std::u16string_view(p, static_cast<size_t>(q - p)));
                p = q + 1;
            }
        }

        size_t count() const {
            size_t n = 0;
            forEach([&n](std::u16string_view) { ++n; });
            return n;
        }
    };

    // char16_t -> 候选拼音列表（word.txt）
    class CharDict {
    public:
        void build(const std::unordered_map<char16_t, std::vector<std::u16string>> &src) {
            m_keys.clear();
            m_offsets.clear();
            m_pool.clear();
            m_keys.reserve(src.size());
            for (const auto &kv : src) m_keys.push_back(kv.first);
            std::sort(m_keys.begin(), m_keys.end());
            m_offsets.reserve(m_keys.size() + 1);
            for (const char16_t key : m_keys) {
                m_offsets.push_back(static_cast<uint32_t>(m_pool.size()));
                for (const auto &s : src.at(key)) {
                    m_pool.append(s);
                    m_pool.push_back(u'\0');
                }
            }
            m_offsets.push_back(static_cast<uint32_t>(m_pool.size()));
            m_keys.shrink_to_fit();
            m_offsets.shrink_to_fit();
            m_pool.shrink_to_fit();
        }

        bool contains(const char16_t c) const { return std::binary_search(m_keys.begin(), m_keys.end(), c); }

        CandidateList find(const char16_t c) const {
            const auto it = std::lower_bound(m_keys.begin(), m_keys.end(), c);
            if (it == m_keys.end() || *it != c) return {};
            const size_t i = static_cast<size_t>(it - m_keys.begin());
            return {m_pool.data() + m_offsets[i], m_pool.data() + m_offsets[i + 1]};
        }

        void clear() {
            m_keys.clear();
            m_offsets.clear();
            m_pool.clear();
        }

    private:
        std::vector<char16_t> m_keys;
        std::vector<uint32_t> m_offsets; // m_keys.size() + 1 个，条目 i 的候选在池里的 [m_offsets[i], m_offsets[i+1])
        std::u16string m_pool;
    };

    // u16string -> 每个字一个拼音（phrases_dict.txt / user_dict.txt）
    class PhraseDict {
    public:
        void build(const std::unordered_map<std::u16string, std::vector<std::u16string>> &src) {
            m_entries.clear();
            m_pool.clear();
            std::vector<const std::pair<const std::u16string, std::vector<std::u16string>> *> sorted;
            sorted.reserve(src.size());
            for (const auto &kv : src) sorted.push_back(&kv);
            std::sort(sorted.begin(), sorted.end(), [](const auto *a, const auto *b) { return a->first < b->first; });
            m_entries.reserve(sorted.size());
            for (const auto *kv : sorted) {
                Entry e{};
                e.keyOffset = static_cast<uint32_t>(m_pool.size());
                e.keyLength = static_cast<uint32_t>(kv->first.size());
                m_pool.append(kv->first);
                e.valueOffset = static_cast<uint32_t>(m_pool.size());
                for (const auto &s : kv->second) {
                    m_pool.append(s);
                    m_pool.push_back(u'\0');
                }
                e.valueEnd = static_cast<uint32_t>(m_pool.size());
                m_entries.push_back(e);
            }
            m_entries.shrink_to_fit();
            m_pool.shrink_to_fit();
        }

        CandidateList find(const std::u16string_view key) const {
            const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), key,
                                             [this](const Entry &e, const std::u16string_view k) { return keyOf(e) < k; });
            if (it == m_entries.end() || keyOf(*it) != key) return {};
            return {m_pool.data() + it->valueOffset, m_pool.data() + it->valueEnd};
        }

        void clear() {
            m_entries.clear();
            m_pool.clear();
        }

        // 还原成可修改的容器（loadCustomUserDict 追加条目后重新 build）
        void exportTo(std::unordered_map<std::u16string, std::vector<std::u16string>> &out) const {
            for (const Entry &e : m_entries) {
                std::vector<std::u16string> values;
                CandidateList{m_pool.data() + e.valueOffset, m_pool.data() + e.valueEnd}.forEach(
                    [&values](const std::u16string_view v) { values.emplace_back(v); });
                out[std::u16string(keyOf(e))] = std::move(values);
            }
        }

    private:
        struct Entry {
            uint32_t keyOffset;
            uint32_t keyLength;
            uint32_t valueOffset;
            uint32_t valueEnd;
        };

        std::u16string_view keyOf(const Entry &e) const { return {m_pool.data() + e.keyOffset, e.keyLength}; }

        std::vector<Entry> m_entries; // 按键排序
        std::u16string m_pool;
    };

    // char16_t -> char16_t（繁 -> 简，trans_word.txt）
    class CharMap {
    public:
        void build(const std::unordered_map<char16_t, char16_t> &src) {
            m_pairs.assign(src.begin(), src.end());
            std::sort(m_pairs.begin(), m_pairs.end());
            m_pairs.shrink_to_fit();
        }

        // 没有映射时返回 c 本身
        char16_t map(const char16_t c) const {
            const auto it = std::lower_bound(m_pairs.begin(), m_pairs.end(), c,
                                             [](const std::pair<char16_t, char16_t> &p, const char16_t k) { return p.first < k; });
            return it != m_pairs.end() && it->first == c ? it->second : c;
        }

        void clear() { m_pairs.clear(); }

        void exportTo(std::unordered_map<char16_t, char16_t> &out) const {
            for (const auto &p : m_pairs) out[p.first] = p.second;
        }

    private:
        std::vector<std::pair<char16_t, char16_t>> m_pairs;
    };

    // 多音字集合（phrases_map.txt 的键；它的值从未被使用）
    class CharSet {
    public:
        template <class Map>
        void buildFromKeys(const Map &src) {
            m_keys.clear();
            m_keys.reserve(src.size());
            for (const auto &kv : src) m_keys.push_back(kv.first);
            std::sort(m_keys.begin(), m_keys.end());
            m_keys.shrink_to_fit();
        }

        bool contains(const char16_t c) const { return std::binary_search(m_keys.begin(), m_keys.end(), c); }

    private:
        std::vector<char16_t> m_keys;
    };

    class ChineseG2pPrivate final {
    public:
        explicit ChineseG2pPrivate(std::string language);
        ~ChineseG2pPrivate();

        void init();

        bool initialized = false;

        CharSet polyphonic;
        PhraseDict phrases_dict;
        CharDict word_dict;
        CharMap trans_dict;

        std::string m_language;
        ToneConverter m_toneConverter;

        inline bool isPolyphonic(const char16_t &oneHanzi) const {
            return polyphonic.contains(oneHanzi);
        }

        inline char16_t tradToSim(const char16_t &oneHanzi) const {
            return trans_dict.map(oneHanzi);
        }

        // 短语的逐字拼音；不存在时返回空列表
        inline CandidateList findPhrase(const std::u16string &phrase) const {
            return phrases_dict.find(phrase);
        }

        inline std::u16string toneConvert(const std::u16string &pinyin, int style, bool v_to_u = false,
                                          bool neutral_tone_with_five = false) const {
            return m_toneConverter.convert({pinyin.begin(), pinyin.end()}, style, v_to_u, neutral_tone_with_five);
        }

        inline std::vector<std::u16string> toneConvert(const CandidateList &pinyin, int style,
                                                       bool v_to_u = false,
                                                       bool neutral_tone_with_five = false) const {
            std::vector<std::u16string> tonePinyin;
            pinyin.forEach([&](const std::u16string_view p) {
                tonePinyin.push_back(m_toneConverter.convert(std::u16string(p), style, v_to_u, neutral_tone_with_five));
            });
            return tonePinyin;
        }

        inline std::vector<std::string> getDefaultPinyin(const char16_t &oneHanzi, int style = 0,
                                                         bool v_to_u = false,
                                                         bool neutral_tone_with_five = false) const {
            const CandidateList candidates = word_dict.find(oneHanzi);
            if (candidates.empty())
                return {u16strToUtf8str(oneHanzi)};

            std::unordered_set<std::string> toneSeen;
            std::vector<std::string> toneCandidates;
            candidates.forEach([&](const std::u16string_view pinyin) {
                const auto &tarPinyin = u16strToUtf8str(
                    m_toneConverter.convert(std::u16string(pinyin), style, v_to_u, neutral_tone_with_five));
                if (toneSeen.insert(tarPinyin).second) {
                    toneCandidates.push_back(tarPinyin);
                }
            });

            if (toneCandidates.empty())
                return {u16strToUtf8str(oneHanzi)};
            return toneCandidates;
        }

        void zhPosition(const std::vector<std::u16string> &input, std::vector<char16_t> &res,
                        std::vector<bool> &positions);
    };
}

#endif // ChineseG2pPRIVATE_H
