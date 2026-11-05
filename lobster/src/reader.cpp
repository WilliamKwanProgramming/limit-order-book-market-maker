#include "lob/lobster_reader.hpp"

#include <algorithm>
#include <cstring>
#include <istream>

namespace lob::lobster {

FileSource::FileSource(const std::string& path) : f_(std::fopen(path.c_str(), "rb")) {
  if (f_ != nullptr) std::setvbuf(f_, nullptr, _IONBF, 0);
}

FileSource::~FileSource() {
  if (f_ != nullptr) std::fclose(f_);
}

std::size_t FileSource::read(char* dst, std::size_t n) {
  if (f_ == nullptr) return 0;
  return std::fread(dst, 1, n, f_);
}

bool FileSource::failed() const { return f_ == nullptr || std::ferror(f_) != 0; }

std::size_t StreamSource::read(char* dst, std::size_t n) {
  in_->read(dst, static_cast<std::streamsize>(n));
  return static_cast<std::size_t>(in_->gcount());
}

bool StreamSource::failed() const { return in_->bad(); }

std::size_t StringSource::read(char* dst, std::size_t n) {
  const std::size_t k = std::min(n, s_.size() - pos_);
  if (k > 0) std::memcpy(dst, s_.data() + pos_, k);
  pos_ += k;
  return k;
}

LineReader::LineReader(ByteSource& src, std::size_t buffer_size)
    : src_(&src), buf_(std::max<std::size_t>(buffer_size, 16)) {}

bool LineReader::refill() {
  if (eof_) return false;
  if (begin_ > 0) {  // move the partial line to the front
    std::memmove(buf_.data(), buf_.data() + begin_, end_ - begin_);
    end_ -= begin_;
    begin_ = 0;
  }
  if (end_ == buf_.size()) buf_.resize(buf_.size() * 2);  // a line longer than the buffer
  const std::size_t got = src_->read(buf_.data() + end_, buf_.size() - end_);
  if (got == 0) {
    eof_ = true;
    return false;
  }
  end_ += got;
  return true;
}

bool LineReader::next(std::string_view& line) {
  std::size_t scan = begin_;
  for (;;) {
    const void* nl = scan < end_ ? std::memchr(buf_.data() + scan, '\n', end_ - scan) : nullptr;
    if (nl != nullptr) {
      const auto pos = static_cast<std::size_t>(static_cast<const char*>(nl) - buf_.data());
      std::size_t len = pos - begin_;
      if (len > 0 && buf_[begin_ + len - 1] == '\r') --len;
      line = std::string_view(buf_.data() + begin_, len);
      begin_ = pos + 1;
      break;
    }
    const std::size_t scanned = end_ - begin_;
    if (!refill()) {
      if (begin_ == end_) return false;
      std::size_t len = end_ - begin_;  // last line without a newline
      if (len > 0 && buf_[begin_ + len - 1] == '\r') --len;
      line = std::string_view(buf_.data() + begin_, len);
      begin_ = end_;
      break;
    }
    scan = begin_ + scanned;  // refill() may have moved the data; skip what was searched
  }
  ++line_no_;
  if (at_start_) {
    at_start_ = false;
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(line[i]); };
    if (line.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) {
      line.remove_prefix(3);
    }
  }
  return true;
}

ReadResult MessageReader::next(Message& out) {
  std::string_view line;
  while (lines_.next(line)) {
    if (is_blank(line)) {
      ++blank_lines_;
      continue;
    }
    raw_ = line;
    error_ = parse_message(line, out);
    return error_.ok() ? ReadResult::Record : ReadResult::Malformed;
  }
  raw_ = {};
  return ReadResult::End;
}

ReadResult BookReader::next(BookRow& out) {
  std::string_view line;
  while (lines_.next(line)) {
    if (is_blank(line)) {
      ++blank_lines_;
      continue;
    }
    raw_ = line;
    if (levels_ == 0) {
      const std::size_t n = count_book_levels(line);
      if (n == 0 || n > kMaxBookLevels) {
        error_ = ParseError{ParseCode::InvalidLevelCount, 0};
        return ReadResult::Malformed;
      }
      levels_ = n;
    }
    if (out.levels() != levels_) out.resize(levels_);
    error_ = parse_book_row(line, out);
    if (error_.code == ParseCode::WrongFieldCount && count_book_levels(line) != levels_) {
      error_.code = ParseCode::LevelCountMismatch;
    }
    return error_.ok() ? ReadResult::Record : ReadResult::Malformed;
  }
  raw_ = {};
  return ReadResult::End;
}

}  // namespace lob::lobster
