#include <cpp-pinyin/ChineseG2p.h>
#include <cpp-pinyin/G2pglobal.h>

#include "ChineseG2p_p.h"
#include "DictUtil.h"

#include <algorithm>
#include <filesystem>

#include "cpp-pinyin/U16Str.h"

#if defined(_MSC_VER)
#pragma warning(disable : 4018)
#endif

namespace Pinyin
{
    static std::vector<std::u16string> splitString(const std::u16string &input) {
        std::vector<std::u16string> res;
        res.reserve(input.size());
        auto start = input.begin();
        const auto end = input.end();

        while (start != end) {
            const auto &currentChar = *start;
            if (Pinyin::isLetter(currentChar)) {
                auto letterStart = start;
                while (start != end && Pinyin::isLetter(*start)) {
                    ++start;
                }
                res.emplace_back(letterStart, start);
            } else if (currentChar >= 0xD800 && currentChar <= 0xDBFF && start + 1 != end &&
                       *(start + 1) >= 0xDC00 && *(start + 1) <= 0xDFFF) {
                // 代理对（emoji 等非 BMP 字符）作为一个整体保留；拆成两个孤立代理项会让 UTF-8 转换失败，
                // 一个带 emoji 的标题就会让整次转换抛异常
                res.emplace_back(start, start + 2);
                std::advance(start, 2);
            } else if (Pinyin::isHanzi(currentChar) || Pinyin::isDigit(currentChar) || !Pinyin::isSpace(currentChar)) {
                res.emplace_back(1, currentChar);
                ++start;
            } else if (Pinyin::isKana(currentChar)) {
                const int length = (start + 1 != end && Pinyin::isSpecialKana(*(start + 1))) ? 2 : 1;
                res.emplace_back(start, start + length);
                std::advance(start, length);
            } else {
                ++start;
            }
        }
        return res;
    }

    static std::u16string mid(const std::vector<char16_t> &inputList, const size_t cursor, const size_t length) {
        const size_t end = std::min(cursor + length, inputList.size());
        return {inputList.begin() + cursor, inputList.begin() + end};
    }

    // reset pinyin to raw string
    static PinyinResVector resetZH(const std::vector<std::u16string> &input, const PinyinResVector &res,
                                   const std::vector<bool> &positions) {
        PinyinResVector result;
        result.reserve(input.size());
        int offset = 0;
        for (int i = 0; i < input.size(); ++i) {
            const auto &encodeStr = u16strToUtf8str(input[i]);
            if (positions[i])
                result.emplace_back(PinyinRes{encodeStr, res[i - offset].pinyin,
                                              res[i - offset].candidates, false});
            else {
                result.emplace_back(PinyinRes{encodeStr, encodeStr, {encodeStr},
                                              true});
                offset++;
            }
        }
        return result;
    }

    // delete elements from the list
    template <class T>
    static inline void removeElements(std::vector<T> &vector, int start, int n) {
        vector.erase(vector.begin() + start, vector.begin() + start + n);
    }

    ChineseG2pPrivate::ChineseG2pPrivate(std::string language) :
        m_language(std::move(language)) {}

    ChineseG2pPrivate::~ChineseG2pPrivate() = default;

    // load zh convert dict
    void ChineseG2pPrivate::init() {
        const std::filesystem::path dict_dir = dictionaryPath() / m_language;

        // 先用原来的哈希表读文件，再压成紧凑字典；临时表在这里就释放
        std::unordered_map<char16_t, std::u16string> phrasesMap;
        std::unordered_map<std::u16string, std::vector<std::u16string>> phrases;
        std::unordered_map<char16_t, std::vector<std::u16string>> words;
        std::unordered_map<char16_t, char16_t> trans;
        initialized = loadDict(dict_dir / "phrases_map.txt", phrasesMap) &&
            loadDict(dict_dir / "phrases_dict.txt", phrases) &&
            loadAdditionalDict(dict_dir / "user_dict.txt", phrases) &&
            loadDict(dict_dir / "word.txt", words) &&
            loadDict(dict_dir / "trans_word.txt", trans);

        polyphonic.buildFromKeys(phrasesMap);
        phrases_dict.build(phrases);
        word_dict.build(words);
        trans_dict.build(trans);
    }

    // get all chinese characters and positions in the list
    void ChineseG2pPrivate::zhPosition(const std::vector<std::u16string> &input, std::vector<char16_t> &res,
                                       std::vector<bool> &positions) {
        res.reserve(input.size());
        for (int i = 0; i < input.size(); ++i) {
            const auto &item = input[i][0];
            if (!item)
                continue;

            const auto simItem = tradToSim(item);

            if (word_dict.contains(simItem)) {
                res.emplace_back(simItem);
                positions[i] = true;
            }
        }
    }

    ChineseG2p::ChineseG2p(const std::string &language) {
        d_ptr = std::make_unique<ChineseG2pPrivate>(language);
        d_ptr->init();
    }

    ChineseG2p::~ChineseG2p() = default;

    bool ChineseG2p::initialized() const {
        return d_ptr->initialized;
    }

    /*
    Style:
        陟罚臧否:zhi4 fa2 zang2 pi3
        汤汤:shang1 shang1
        到了:dao4 le1
    */
    bool ChineseG2p::loadCustomUserDict(const std::filesystem::path &filePath, const bool clearRawData) const {
        // 紧凑字典只读：还原成哈希表、追加、再重新压缩
        std::unordered_map<std::u16string, std::vector<std::u16string>> phrases;
        if (!clearRawData)
            d_ptr->phrases_dict.exportTo(phrases);
        const bool ok = loadAdditionalDict(filePath, phrases);
        d_ptr->phrases_dict.build(phrases);
        return ok;
    }

    /*
    Style:
        䰾:鲃
        魚:鱼
        䴉:鹮
    */
    bool ChineseG2p::loadCustomFanJianDictionary(const std::filesystem::path &filePath, const bool clearRawData) const {
        std::unordered_map<char16_t, char16_t> trans;
        if (!clearRawData)
            d_ptr->trans_dict.exportTo(trans);
        const bool ok = loadDict(filePath, trans);
        d_ptr->trans_dict.build(trans);
        return ok;
    }

    void ChineseG2p::setToneConverter(const ToneConverter &toneConverter) const {
        d_ptr->m_toneConverter = toneConverter;
    }

    PinyinResVector ChineseG2p::hanziToPinyin(const std::string &hans, int style, Error error, bool candidates,
                                              bool v_to_u, bool neutral_tone_with_five) const {
        return hanziToPinyin(splitString(utf8strToU16str(hans)), style, error, candidates, v_to_u,
                             neutral_tone_with_five);
    }

    PinyinResVector ChineseG2p::hanziToPinyin(const std::vector<std::string> &hans, int style, Error error,
                                              bool candidates, bool v_to_u, bool neutral_tone_with_five) const {
        std::vector<std::u16string> hansList;
        hansList.reserve(hans.size());
        for (const auto &item : hans) {
            hansList.emplace_back(utf8strToU16str(item));
        }
        return hanziToPinyin(hansList, style, error, candidates, v_to_u, neutral_tone_with_five);
    }

    PinyinResVector ChineseG2p::hanziToPinyin(const std::vector<std::u16string> &hans, int style, Error error,
                                              bool candidates, bool v_to_u, bool neutral_tone_with_five) const {
        std::vector<char16_t> hansList;
        std::vector<bool> inputPos(hans.size(), false);
        // get char&pos in dict
        d_ptr->zhPosition(hans, hansList, inputPos);
        return resetZH(hans, hanziToPinyin(hansList, style, error, candidates, v_to_u, neutral_tone_with_five),
                       inputPos);
    }

    PinyinResVector ChineseG2p::hanziToPinyin(const std::vector<char16_t> &hansList, int style, Error error,
                                              bool candidates,
                                              bool v_to_u, bool neutral_tone_with_five) const {

        PinyinResVector result;
        result.reserve(hansList.size());
        int cursor = 0;
        while (cursor < hansList.size()) {
            // get char
            const char16_t &current_char = hansList[cursor];

            // not in dict
            if (!d_ptr->word_dict.contains(current_char)) {
                result.emplace_back(PinyinRes{u16strToUtf8str(current_char)});
                cursor++;
                continue;
            }

            // is polypropylene
            if (!d_ptr->isPolyphonic(current_char)) {
                const auto &pinyin = d_ptr->getDefaultPinyin(current_char, style, v_to_u, neutral_tone_with_five);
                result.emplace_back(PinyinRes{
                    u16strToUtf8str(current_char),
                    pinyin[0],
                    candidates ? pinyin : std::vector<std::string>{},
                    false
                });
                cursor++;
            } else {
                bool found = false;
                for (int length = 4; length >= 2 && !found; length--) {
                    if (cursor + length <= hansList.size()) {
                        // cursor: 地, subPhrase: 地久天长
                        const std::u16string subPhrase = mid(hansList, cursor, length);
                        const CandidateList phrase = d_ptr->findPhrase(subPhrase);
                        if (!phrase.empty()) {
                            const auto &subRes = d_ptr->toneConvert(phrase, style, v_to_u, neutral_tone_with_five);
                            for (int i = 0; i < subRes.size(); i++) {
                                const auto &lyric = subPhrase[i];
                                result.emplace_back(PinyinRes{
                                    u16strToUtf8str(lyric),
                                    u16strToUtf8str(subRes[i]),
                                    candidates
                                    ? d_ptr->getDefaultPinyin(lyric, style, v_to_u, neutral_tone_with_five)
                                    : std::vector<std::string>{},
                                    false
                                });
                            }
                            cursor += length;
                            found = true;
                        }

                        if (cursor >= 1) {
                            // cursor: 重, subPhrase_1: 语重心长
                            const std::u16string subPhrase1 = mid(hansList, cursor - 1, length);
                            const CandidateList phrase1 = d_ptr->findPhrase(subPhrase1);
                            if (!phrase1.empty()) {
                                result.pop_back();
                                const auto &subRes1 = d_ptr->toneConvert(
                                    phrase1, style, v_to_u, neutral_tone_with_five);
                                for (int i = 0; i < subRes1.size(); i++) {
                                    const auto &lyric = subPhrase1[i];
                                    result.emplace_back(PinyinRes{
                                        u16strToUtf8str(lyric),
                                        u16strToUtf8str(subRes1[i]),
                                        candidates
                                        ? d_ptr->getDefaultPinyin(lyric, style, v_to_u, neutral_tone_with_five)
                                        : std::vector<std::string>{},
                                        false
                                    });
                                }
                                cursor += length - 1;
                                found = true;
                            }
                        }
                    }

                    if (cursor + 1 >= length && cursor + 1 <= hansList.size()) {
                        // cursor: 好, xSubPhrase: 各有所好
                        const std::u16string subPhraseBack = mid(hansList, cursor + 1 - length, length);
                        const CandidateList phraseBack = d_ptr->findPhrase(subPhraseBack);
                        if (!phraseBack.empty()) {
                            // overwrite pinyin
                            removeElements(result, cursor + 1 - length, length - 1);
                            const auto &subResBack = d_ptr->toneConvert(phraseBack, style, v_to_u,
                                                                        neutral_tone_with_five);
                            for (int i = 0; i < subResBack.size(); i++) {
                                const auto &lyric = subPhraseBack[i];
                                result.emplace_back(PinyinRes{
                                    u16strToUtf8str(lyric),
                                    u16strToUtf8str(subResBack[i]),
                                    candidates
                                    ? d_ptr->getDefaultPinyin(lyric, style, v_to_u, neutral_tone_with_five)
                                    : std::vector<std::string>{},
                                    false
                                });
                            }
                            cursor += 1;
                            found = true;
                        }
                    }

                    if (cursor + 2 >= length && cursor + 2 <= hansList.size()) {
                        // cursor: 好, xSubPhrase: 叶公好龙
                        const std::u16string subPhraseBack1 = mid(hansList, cursor + 2 - length, length);
                        const CandidateList phraseBack1 = d_ptr->findPhrase(subPhraseBack1);
                        if (!phraseBack1.empty()) {
                            // overwrite pinyin
                            removeElements(result, cursor + 2 - length, length - 2);
                            const auto &subResBack1 = d_ptr->toneConvert(
                                phraseBack1, style, v_to_u, neutral_tone_with_five);
                            for (int i = 0; i < subResBack1.size(); i++) {
                                const auto &lyric = subPhraseBack1[i];
                                result.emplace_back(PinyinRes{
                                    u16strToUtf8str(lyric),
                                    u16strToUtf8str(subResBack1[i]),
                                    candidates
                                    ? d_ptr->getDefaultPinyin(lyric, style, v_to_u, neutral_tone_with_five)
                                    : std::vector<std::string>{},
                                    false
                                });
                            }
                            cursor += 2;
                            found = true;
                        }
                    }
                }

                // not found, use default pinyin
                if (!found) {
                    const auto &pinyin = d_ptr->getDefaultPinyin(current_char, style, v_to_u, neutral_tone_with_five);
                    result.emplace_back(PinyinRes{
                        u16strToUtf8str(current_char),
                        pinyin[0],
                        candidates ? pinyin : std::vector<std::string>{},
                        false
                    });
                    cursor++;
                }
            }
        }

        // Alloc 2
        if (error == Error::Ignore) {
            return result;
        }
        return result;
    }

    std::string ChineseG2p::tradToSim(const std::string &oneHanzi) const {
        return u16strToUtf8str(d_ptr->tradToSim(utf8strToU16str(oneHanzi)[0]));
    }

    bool ChineseG2p::isPolyphonic(const std::string &oneHanzi) const {
        return d_ptr->isPolyphonic(utf8strToU16str(oneHanzi)[0]);
    }

    std::vector<std::string> ChineseG2p::getDefaultPinyin(const std::string &oneHanzi, int style, bool v_to_u,
                                                          bool neutral_tone_with_five) const {
        return d_ptr->getDefaultPinyin(d_ptr->tradToSim(utf8strToU16str(oneHanzi)[0]), style, v_to_u,
                                       neutral_tone_with_five);
    }
}
