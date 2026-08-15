#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace stackcore {

// 読み取り専用の mmap ラッパ。
// 数万フレームの動画から必要なフレームだけを読み出すため、ファイル全体を
// メモリに載せずにランダムアクセスできるようにする（仕様書 §4.1）。
class MappedFile {
public:
    MappedFile() noexcept = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // 失敗時は std::runtime_error を投げる。
    void open(const std::string& path);
    void close() noexcept;

    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool is_open() const noexcept { return data_ != nullptr; }

    // 「先頭から順に読む」ことをカーネルに伝える。
    // 先読みが効き、読み終えたページを手放しやすくなる。
    // 助言はマップの内容を変えないので const で呼べる。
    void advise_sequential() const noexcept;

    // 低メモリモード（仕様書 §7.3 の「設定で2GBまで引き下げ可能」）。
    //
    // budget_bytes バイトぶん読み進めるごとに、マッピングを張り直して
    // 常駐ページを一括で手放す。0で無効。
    //
    // **madvise では効かないことを実測で確認している。** 2.6GBのSERで
    // MADV_DONTNEED / MADV_FREE / MADV_FREE_REUSABLE のいずれを呼んでも
    // 最大RSSは2651MBのまま変わらなかった（macOSではファイルバックドの
    // 常駐ページに対する助言が最大RSSに反映されない）。
    // マッピングを張り直す方式なら、512MBごとで最大RSS 512MB、
    // 128MBごとで128MBと、狙いどおりに効く。
    //
    // 張り直すと過去に data() で得たポインタは無効になる。
    // 呼び出し側は毎回 data() を取り直すこと。
    void set_reclaim_budget(std::size_t budget_bytes);

    // budget ぶん読んだかを記録し、超えていればマッピングを張り直す。
    // set_reclaim_budget(0) のときは何もしない。
    void note_read(std::size_t bytes) const;

private:
    void remap() const;

    std::string path_;
    mutable const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t reclaim_budget_ = 0;
    mutable std::size_t read_since_remap_ = 0;
};

}  // namespace stackcore
