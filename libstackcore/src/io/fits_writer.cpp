#include "stackcore/fits_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace stackcore {
namespace {

constexpr std::size_t kCardSize = 80;
constexpr std::size_t kBlockSize = 2880;

std::string value_card(const std::string& keyword, const std::string& value,
                       const std::string& comment) {
    std::string card(kCardSize, ' ');
    const std::size_t keyword_size = std::min<std::size_t>(8, keyword.size());
    std::copy(keyword.begin(), keyword.begin() + keyword_size, card.begin());
    card[8] = '=';
    card[9] = ' ';

    // FITSの慣例どおり、値は20文字幅で右寄せする。
    const std::size_t field_size = std::min<std::size_t>(20, value.size());
    const std::size_t value_start = 10 + 20 - field_size;
    std::copy(value.end() - static_cast<std::ptrdiff_t>(field_size), value.end(),
              card.begin() + static_cast<std::ptrdiff_t>(value_start));

    if (!comment.empty()) {
        const std::size_t comment_start = 32;
        card[comment_start] = '/';
        card[comment_start + 1] = ' ';
        const std::size_t count =
            std::min(kCardSize - comment_start - 2, comment.size());
        std::copy(comment.begin(), comment.begin() + static_cast<std::ptrdiff_t>(count),
                  card.begin() + static_cast<std::ptrdiff_t>(comment_start + 2));
    }
    return card;
}

std::string text_card(const std::string& keyword, const std::string& text) {
    std::string card(kCardSize, ' ');
    const std::size_t keyword_size = std::min<std::size_t>(8, keyword.size());
    std::copy(keyword.begin(), keyword.begin() + keyword_size, card.begin());
    if (!text.empty()) {
        card[8] = ' ';
        const std::size_t count = std::min(kCardSize - 9, text.size());
        std::copy(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(count),
                  card.begin() + 9);
    }
    return card;
}

class FileWriter {
public:
    explicit FileWriter(const std::string& path) : path_(path), file_(std::fopen(path.c_str(), "wb")) {
        if (!file_) throw std::runtime_error("FITS: ファイルを作成できません: " + path);
    }

    ~FileWriter() {
        if (file_) std::fclose(file_);
    }

    void write(const void* data, std::size_t size) {
        if (size > 0 && std::fwrite(data, 1, size, file_) != size) {
            throw std::runtime_error("FITS: 書き込みに失敗しました: " + path_);
        }
    }

    void close() {
        if (!file_) return;
        if (std::fclose(file_) != 0) {
            file_ = nullptr;
            throw std::runtime_error("FITS: ファイルを閉じられません: " + path_);
        }
        file_ = nullptr;
    }

private:
    std::string path_;
    std::FILE* file_;
};

float normalized_sample(float value) {
    if (!std::isfinite(value)) return 0.0f;
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

}  // namespace

void write_fits_float32(const std::string& path, const FrameBuffer& image) {
    if (image.empty()) throw std::invalid_argument("FITS: 空の画像は書き出せません");
    if (image.channels() != 1 && image.channels() != 3) {
        throw std::invalid_argument("FITS: 1ch または 3ch のみ対応しています (指定: " +
                                    std::to_string(image.channels()) + "ch)");
    }

    std::vector<std::string> cards;
    cards.push_back(value_card("SIMPLE", "T", "conforms to FITS standard"));
    cards.push_back(value_card("BITPIX", "-32", "IEEE 754 32-bit floating point"));
    cards.push_back(value_card("NAXIS", image.channels() == 1 ? "2" : "3",
                               "number of data axes"));
    cards.push_back(value_card("NAXIS1", std::to_string(image.width()), "image width"));
    cards.push_back(value_card("NAXIS2", std::to_string(image.height()), "image height"));
    if (image.channels() == 3) {
        cards.push_back(value_card("NAXIS3", "3", "RGB channel planes"));
    }
    cards.push_back(value_card("EXTEND", "T", "extensions may be present"));
    cards.push_back(value_card("ORIGIN", "'LunaStack'", "software that created this file"));
    cards.push_back(text_card("COMMENT", "Pixel samples are normalized to the [0,1] range."));
    cards.push_back(text_card("COMMENT", "For RGB cubes, channel order is red, green, blue."));
    cards.push_back(text_card("END", ""));

    std::string header;
    header.reserve(kBlockSize);
    for (const std::string& card : cards) header += card;
    const std::size_t header_padding =
        (kBlockSize - (header.size() % kBlockSize)) % kBlockSize;
    header.append(header_padding, ' ');

    FileWriter writer(path);
    writer.write(header.data(), header.size());

    std::vector<unsigned char> row(static_cast<std::size_t>(image.width()) * 4u);
    for (int c = 0; c < image.channels(); ++c) {
        for (int y = 0; y < image.height(); ++y) {
            const float* source = image.row(c, y);
            for (int x = 0; x < image.width(); ++x) {
                const float value = normalized_sample(source[x]);
                std::uint32_t raw = 0;
                static_assert(sizeof(raw) == sizeof(value), "float32が必要です");
                std::memcpy(&raw, &value, sizeof(raw));
                const std::size_t offset = static_cast<std::size_t>(x) * 4u;
                row[offset + 0] = static_cast<unsigned char>((raw >> 24) & 0xFFu);
                row[offset + 1] = static_cast<unsigned char>((raw >> 16) & 0xFFu);
                row[offset + 2] = static_cast<unsigned char>((raw >> 8) & 0xFFu);
                row[offset + 3] = static_cast<unsigned char>(raw & 0xFFu);
            }
            writer.write(row.data(), row.size());
        }
    }

    const std::size_t data_bytes = static_cast<std::size_t>(image.width()) * image.height() *
                                   image.channels() * sizeof(float);
    const std::size_t data_padding =
        (kBlockSize - (data_bytes % kBlockSize)) % kBlockSize;
    if (data_padding > 0) {
        const std::vector<unsigned char> zeroes(data_padding, 0);
        writer.write(zeroes.data(), zeroes.size());
    }
    writer.close();
}

}  // namespace stackcore
