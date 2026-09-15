#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace slick::net::detail {

// Incremental parser for a text/event-stream body. The body may be fed in pieces split anywhere, even inside a
// CRLF, and each byte is examined once, so parsing stays linear in the body size however it is fragmented.
//
// Lines end in CRLF, LF or CR. A "data:" line appends its value, less one leading space, to the current event,
// joined to earlier non-empty data by LF; a blank line dispatches the event if its data is non-empty. Comment
// lines and fields other than data are ignored.
class sse_parser {
public:
    // Parses the next piece of the body, calling on_event(const char* data, std::size_t size) for each event the
    // piece completes. The event data is only valid during the call.
    template <typename OnEvent>
    void feed(const char* data, std::size_t size, OnEvent&& on_event) {
        const char* p = data;
        const char* const end = data + size;

        // The previous piece ended in CR, which ended its line, so an LF opening this piece completes that CRLF
        if (after_cr_ && p != end) {
            after_cr_ = false;
            p += *p == '\n';
        }

        while (p != end) {
            const char* const eol = find_line_end(p, end);
            consume_line_content(p, eol);
            if (eol == end) {
                break;  // The line continues in the next piece
            }
            p = eol + 1;
            if (*eol == '\r') {
                if (p == end) {
                    after_cr_ = true;
                } else {
                    p += *p == '\n';
                }
            }
            end_line(on_event);
        }
    }

    // Discards any partially received line and event, readying the parser for a new body
    void reset() noexcept {
        data_.clear();
        state_ = line_state::field;
        matched_ = 0;
        after_cr_ = false;
    }

private:
    enum class line_state : std::uint8_t {
        field,        // Matching the line against "data:"; matched_ holds how many bytes matched so far
        value_start,  // Just after "data:", where a single space is dropped
        value,        // Inside a data value, which is appended to the event
        ignored,      // A comment or a field other than data
    };

    static constexpr std::string_view data_field = "data:";

    static const char* find_line_end(const char* p, const char* const end) noexcept {
        while (p != end && *p != '\n' && *p != '\r') {
            ++p;
        }
        return p;
    }

    // Handles the bytes [p, end) of the current line, which hold no line ending
    void consume_line_content(const char* p, const char* const end) {
        while (p != end) {
            switch (state_) {
            case line_state::field:
                if (*p != data_field[matched_]) {
                    state_ = line_state::ignored;
                    return;
                }
                ++p;
                if (++matched_ == data_field.size()) {
                    if (!data_.empty()) {
                        data_.push_back('\n');
                    }
                    state_ = line_state::value_start;
                }
                break;
            case line_state::value_start:
                p += *p == ' ';
                state_ = line_state::value;
                break;
            case line_state::value:
                data_.append(p, static_cast<std::size_t>(end - p));
                return;
            case line_state::ignored:
                return;
            }
        }
    }

    template <typename OnEvent>
    void end_line(OnEvent& on_event) {
        // A blank line dispatches the event
        if (state_ == line_state::field && matched_ == 0 && !data_.empty()) {
            on_event(data_.data(), data_.size());
            data_.clear();
        }
        state_ = line_state::field;
        matched_ = 0;
    }

    std::string data_;  // Data of the event being received; clear() keeps the capacity for the next event
    line_state state_ = line_state::field;
    std::uint8_t matched_ = 0;
    bool after_cr_ = false;  // The last piece ended in CR, so an LF opening the next one is not a line ending
};

} // namespace slick::net::detail
