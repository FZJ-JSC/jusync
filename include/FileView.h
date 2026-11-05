#pragma once
// FileView - read-only view over a downloaded file's bytes.
//
// The transport already owns the payload in its frame buffer.  A view either
// aliases that buffer (single-chunk files, no copy) or owns one contiguous
// buffer assembled from several chunks (one copy, same as the legacy
// download-into-caller-buffer path).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace anari_usd_middleware {

class FileViewAssembler;

class FileView {
public:
    FileView() = default;
    FileView(FileView&&) noexcept = default;
    FileView& operator=(FileView&&) noexcept = default;
    FileView(const FileView&) = delete;
    FileView& operator=(const FileView&) = delete;

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }

    /// True when the view points directly at the transport buffer.
    bool isAliased() const { return aliased_; }

    explicit operator bool() const { return data_ != nullptr && size_ > 0; }

private:
    friend class FileViewAssembler;

    std::shared_ptr<const void> owner_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    bool aliased_ = false;
};

/// Reassembles file chunks into a FileView.
///
/// A file delivered as one chunk covering the whole payload is exposed in
/// place.  Anything else (several chunks, a gap, or a chunk shorter than the
/// announced file) is copied exactly once into a contiguous owned buffer.
class FileViewAssembler {
public:
    /// `owner` keeps the chunk's backing memory alive while it is aliased; it
    /// may be null when the caller guarantees the chunk outlives TakeView().
    void AddChunk(std::shared_ptr<const void> owner, const uint8_t* data,
                  size_t size, uint64_t offset, uint64_t totalSize) {
        if (size == 0) {
            if (totalSize > total_) total_ = totalSize;
            return;
        }
        if (!data || offset > std::numeric_limits<size_t>::max() ||
            size > std::numeric_limits<size_t>::max() - offset) {
            overflow_ = true;
            return;
        }

        const uint64_t end = offset + size;
        if (end > total_) total_ = end;
        if (totalSize > total_) total_ = totalSize;

        sawChunk_ = true;
        if (!owned_ && !firstPending_) {
            firstPending_ = true;
            firstOwner_ = std::move(owner);
            firstData_ = data;
            firstSize_ = size;
            firstOffset_ = static_cast<size_t>(offset);
            written_ = static_cast<size_t>(end);
            return;
        }

        EnsureOwned();
        std::memcpy(owned_->data() + offset, data, size);
        if (end > written_) written_ = static_cast<size_t>(end);
    }

    /// Marks the transfer finished.  Zero-fills any announced tail that no
    /// chunk covered, matching RequestFileIntoBuffer_C semantics.
    void Complete(uint64_t totalSize) {
        if (totalSize > total_) total_ = totalSize;

        if (!sawChunk_) {
            if (total_ > 0) {
                EnsureOwned();
                std::memset(owned_->data(), 0, static_cast<size_t>(total_));
                written_ = static_cast<size_t>(total_);
            }
            return;
        }

        if (!owned_ && firstOffset_ == 0 && firstSize_ == total_ && total_ > 0) {
            aliasOwner_ = std::move(firstOwner_);
            aliasData_ = firstData_;
            aliasSize_ = firstSize_;
            alias_ = true;
            firstPending_ = false;
            return;
        }

        EnsureOwned();
        if (total_ > written_) {
            std::memset(owned_->data() + written_, 0,
                        static_cast<size_t>(total_ - written_));
            written_ = static_cast<size_t>(total_);
        }
    }

    bool overflow() const { return overflow_; }

    FileView TakeView() {
        FileView view;
        if (alias_) {
            view.owner_ = std::move(aliasOwner_);
            view.data_ = aliasData_;
            view.size_ = aliasSize_;
            view.aliased_ = true;
        } else if (owned_ && total_ > 0) {
            view.data_ = owned_->data();
            view.size_ = static_cast<size_t>(total_);
            view.owner_ = std::move(owned_);
        }
        owned_.reset();
        return view;
    }

private:
    void EnsureOwned() {
        const size_t wanted = static_cast<size_t>(total_);
        if (!owned_) {
            owned_ = std::make_shared<std::vector<uint8_t>>(wanted);
        } else if (owned_->size() < wanted) {
            owned_->resize(wanted);
        }
        if (firstPending_) {
            std::memcpy(owned_->data() + firstOffset_, firstData_, firstSize_);
            firstPending_ = false;
            firstOwner_.reset();
        }
    }

    bool sawChunk_ = false;
    bool overflow_ = false;

    bool firstPending_ = false;
    std::shared_ptr<const void> firstOwner_;
    const uint8_t* firstData_ = nullptr;
    size_t firstSize_ = 0;
    size_t firstOffset_ = 0;

    bool alias_ = false;
    std::shared_ptr<const void> aliasOwner_;
    const uint8_t* aliasData_ = nullptr;
    size_t aliasSize_ = 0;

    std::shared_ptr<std::vector<uint8_t>> owned_;
    uint64_t total_ = 0;
    size_t written_ = 0;
};

} // namespace anari_usd_middleware
