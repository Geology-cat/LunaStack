#include "stackcore/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace stackcore {

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void MappedFile::open(const std::string& path) {
    close();

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("ファイルを開けません: " + path + " (" + std::strerror(errno) + ")");
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        throw std::runtime_error("ファイル情報を取得できません: " + path + " (" + std::strerror(e) + ")");
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        throw std::runtime_error("通常ファイルではありません: " + path);
    }
    if (st.st_size <= 0) {
        ::close(fd);
        throw std::runtime_error("ファイルが空です: " + path);
    }

    const std::size_t size = static_cast<std::size_t>(st.st_size);
    void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    const int map_errno = errno;
    ::close(fd);  // mmap 後は fd を閉じてよい

    if (p == MAP_FAILED) {
        throw std::runtime_error("mmapに失敗しました: " + path + " (" + std::strerror(map_errno) + ")");
    }

    data_ = static_cast<const std::uint8_t*>(p);
    size_ = size;
    path_ = path;  // 低メモリモードでマッピングを張り直すために覚えておく
    read_since_remap_ = 0;
}

void MappedFile::close() noexcept {
    if (data_ != nullptr) {
        ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_);
        data_ = nullptr;
        size_ = 0;
    }
    path_.clear();
    read_since_remap_ = 0;
}

void MappedFile::set_reclaim_budget(std::size_t budget_bytes) {
    reclaim_budget_ = budget_bytes;
    read_since_remap_ = 0;
}

void MappedFile::note_read(std::size_t bytes) const {
    if (reclaim_budget_ == 0 || !data_) return;
    read_since_remap_ += bytes;
    if (read_since_remap_ >= reclaim_budget_) {
        remap();
        read_since_remap_ = 0;
    }
}

void MappedFile::remap() const {
    if (!data_ || path_.empty()) return;
    ::munmap(const_cast<std::uint8_t*>(data_), size_);
    data_ = nullptr;

    const int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("低メモリモード: ファイルを開き直せません: " + path_);
    }
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    const int map_errno = errno;
    ::close(fd);
    if (p == MAP_FAILED) {
        throw std::runtime_error("低メモリモード: mmapし直せません: " + path_ + " (" +
                                 std::strerror(map_errno) + ")");
    }
    data_ = static_cast<const std::uint8_t*>(p);
}

void MappedFile::advise_sequential() const noexcept {
    if (!data_ || size_ == 0) return;
    ::madvise(const_cast<std::uint8_t*>(data_), size_, MADV_SEQUENTIAL);
}

#if 0
void MappedFile::release_range(std::size_t offset, std::size_t length) const noexcept {
    if (!data_ || size_ == 0 || length == 0) return;
    if (offset >= size_) return;
    if (offset + length > size_) length = size_ - offset;

    // ページ境界に丸める。境界内に有効なデータが残っている部分を
    // 巻き込んで捨てないよう、内側へ寄せる。
    static const std::size_t page = static_cast<std::size_t>(::getpagesize());
    const std::size_t begin = (offset + page - 1) / page * page;
    const std::size_t end = (offset + length) / page * page;
    if (end <= begin) return;

    ::madvise(const_cast<std::uint8_t*>(data_ + begin), end - begin, MADV_DONTNEED);
}
#endif

}  // namespace stackcore
