// Streaming readers for LOBSTER message and order-book files.
//
// Memory is O(1) in the file size: a LineReader keeps one buffer (1 MiB by default) and hands out
// std::string_view lines that stay valid until the next call. The buffer grows only if a single
// line is longer than it. Lines end at '\n'; one '\r' before it is removed, and a last line
// without a newline is returned as a line. A UTF-8 byte-order mark at the start is skipped.
// Blank lines (only spaces and tabs) are skipped by the record readers and counted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "lob/lobster_format.hpp"

namespace lob::lobster {

// Source of bytes. read() returns the number of bytes written to dst, 0 at end of input or on
// error; failed() tells the two apart.
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual std::size_t read(char* dst, std::size_t n) = 0;
  virtual bool failed() const = 0;
};

// A file opened in binary mode with stdio buffering turned off (LineReader buffers).
class FileSource final : public ByteSource {
 public:
  explicit FileSource(const std::string& path);
  ~FileSource() override;
  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;

  bool is_open() const noexcept { return f_ != nullptr; }
  std::size_t read(char* dst, std::size_t n) override;
  bool failed() const override;

 private:
  std::FILE* f_ = nullptr;
};

class StreamSource final : public ByteSource {
 public:
  explicit StreamSource(std::istream& in) : in_(&in) {}
  std::size_t read(char* dst, std::size_t n) override;
  bool failed() const override;

 private:
  std::istream* in_;
};

// Reads from memory (tests, small fixtures). The viewed bytes must outlive the source.
class StringSource final : public ByteSource {
 public:
  explicit StringSource(std::string_view s) : s_(s) {}
  std::size_t read(char* dst, std::size_t n) override;
  bool failed() const override { return false; }

 private:
  std::string_view s_;
  std::size_t pos_ = 0;
};

class LineReader {
 public:
  static constexpr std::size_t kDefaultBuffer = std::size_t{1} << 20;

  explicit LineReader(ByteSource& src, std::size_t buffer_size = kDefaultBuffer);

  // Next line without its terminator. Returns false at end of input (or on a read error).
  bool next(std::string_view& line);
  // 1-based number of the line last returned.
  std::uint64_t line_number() const noexcept { return line_no_; }
  bool read_failed() const { return src_->failed(); }

 private:
  bool refill();

  ByteSource* src_;
  std::vector<char> buf_;
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
  std::uint64_t line_no_ = 0;
  bool eof_ = false;
  bool at_start_ = true;
};

enum class ReadResult : std::uint8_t {
  Record,     // a well-formed record was parsed
  Malformed,  // a non-blank line failed to parse; see error() and line()
  End,        // end of input (check io_failed())
};

class MessageReader {
 public:
  explicit MessageReader(ByteSource& src, std::size_t buffer_size = LineReader::kDefaultBuffer)
      : lines_(src, buffer_size) {}

  ReadResult next(Message& out);
  std::uint64_t line() const noexcept { return lines_.line_number(); }
  const ParseError& error() const noexcept { return error_; }
  std::string_view raw_line() const noexcept { return raw_; }  // valid until the next call
  std::uint64_t blank_lines() const noexcept { return blank_lines_; }
  bool io_failed() const { return lines_.read_failed(); }

 private:
  LineReader lines_;
  ParseError error_;
  std::string_view raw_;
  std::uint64_t blank_lines_ = 0;
};

class BookReader {
 public:
  // expected_levels = 0: infer the level count from the first non-blank row.
  explicit BookReader(ByteSource& src, std::size_t expected_levels = 0,
                      std::size_t buffer_size = LineReader::kDefaultBuffer)
      : lines_(src, buffer_size), levels_(expected_levels) {}

  // `out` is resized to levels() if needed (once).
  ReadResult next(BookRow& out);
  std::size_t levels() const noexcept { return levels_; }
  std::uint64_t line() const noexcept { return lines_.line_number(); }
  const ParseError& error() const noexcept { return error_; }
  std::string_view raw_line() const noexcept { return raw_; }
  std::uint64_t blank_lines() const noexcept { return blank_lines_; }
  bool io_failed() const { return lines_.read_failed(); }

 private:
  LineReader lines_;
  std::size_t levels_;
  ParseError error_;
  std::string_view raw_;
  std::uint64_t blank_lines_ = 0;
};

}  // namespace lob::lobster
