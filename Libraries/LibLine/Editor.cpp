/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Editor.h"
#include <AK/StringBuilder.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace Line {

Editor::Editor()
{
    m_pending_chars = ByteBuffer::create_uninitialized(0);
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
        m_num_columns = 80;
        m_num_lines = 25;
    } else {
        m_num_columns = ws.ws_col;
        m_num_lines = ws.ws_row;
    }
}

Editor::~Editor()
{
    if (m_initialized)
        tcsetattr(0, TCSANOW, &m_default_termios);
}

void Editor::add_to_history(const String& line)
{
    if ((m_history.size() + 1) > m_history_capacity)
        m_history.take_first();
    m_history.append(line);
}

void Editor::clear_line()
{
    for (size_t i = 0; i < m_cursor; ++i)
        fputc(0x8, stdout);
    fputs("\033[K", stdout);
    fflush(stdout);
    m_buffer.clear();
    m_cursor = 0;
}

void Editor::insert(const String& string)
{
    for (auto ch : string)
        insert(ch);
}

void Editor::insert(const char ch)
{
    m_pending_chars.append(&ch, 1);
    if (m_cursor == m_buffer.size()) {
        m_buffer.append(ch);
        m_cursor = m_buffer.size();
        return;
    }

    m_buffer.insert(m_cursor, ch);
    ++m_chars_inserted_in_the_middle;
    ++m_cursor;
}

void Editor::register_character_input_callback(char ch, Function<bool(Editor&)> callback)
{
    if (m_key_callbacks.contains(ch)) {
        dbg() << "Key callback registered twice for " << ch;
        ASSERT_NOT_REACHED();
    }
    m_key_callbacks.set(ch, make<KeyCallback>(move(callback)));
}

void Editor::stylize(const Span& span, const Style& style)
{
    auto starting_map = m_spans_starting.get(span.beginning()).value_or({});

    if (!starting_map.contains(span.end()))
        m_refresh_needed = true;

    starting_map.set(span.end(), style);

    m_spans_starting.set(span.beginning(), starting_map);

    auto ending_map = m_spans_ending.get(span.end()).value_or({});

    if (!ending_map.contains(span.beginning()))
        m_refresh_needed = true;
    ending_map.set(span.beginning(), style);

    m_spans_ending.set(span.end(), ending_map);
}

String Editor::get_line(const String& prompt)
{
    set_prompt(prompt);
    reset();
    set_origin();

    m_history_cursor = m_history.size();
    for (;;) {
        refresh_display();
        char keybuf[16];
        ssize_t nread = read(0, keybuf, sizeof(keybuf));
        // FIXME: exit()ing here is a bit off. Should communicate failure to caller somehow instead.
        if (nread == 0)
            exit(0);
        if (nread < 0) {
            if (errno == EINTR) {
                if (m_was_interrupted) {
                    m_was_interrupted = false;
                    if (!m_buffer.is_empty())
                        printf("^C");
                }
                if (m_was_resized)
                    continue;

                m_buffer.clear();
                putchar('\n');
                return String::empty();
            }
            perror("read failed");
            // FIXME: exit()ing here is a bit off. Should communicate failure to caller somehow instead.
            exit(2);
        }

        auto reverse_tab = false;
        auto increment_suggestion_index = [&] {
            if (m_suggestions.size())
                m_next_suggestion_index = (m_next_suggestion_index + 1) % m_suggestions.size();
            else
                m_next_suggestion_index = 0;
        };
        auto decrement_suggestion_index = [&] {
            if (m_next_suggestion_index == 0)
                m_next_suggestion_index = m_suggestions.size();
            m_next_suggestion_index--;
        };
        for (ssize_t i = 0; i < nread; ++i) {
            char ch = keybuf[i];
            if (ch == 0)
                continue;

            switch (m_state) {
            case InputState::ExpectBracket:
                if (ch == '[') {
                    m_state = InputState::ExpectFinal;
                    continue;
                } else {
                    m_state = InputState::Free;
                    break;
                }
            case InputState::ExpectFinal:
                switch (ch) {
                case 'A': // up
                    if (m_history_cursor > 0)
                        --m_history_cursor;
                    if (m_history_cursor < m_history.size()) {
                        auto& line = m_history[m_history_cursor];
                        m_buffer.clear();
                        for (auto& c : line)
                            m_buffer.append(c);
                        m_cursor = m_buffer.size();
                        m_refresh_needed = true;
                    }
                    m_state = InputState::Free;
                    continue;
                case 'B': // down
                    if (m_history_cursor < m_history.size())
                        ++m_history_cursor;
                    if (m_history_cursor < m_history.size()) {
                        auto& line = m_history[m_history_cursor];
                        m_buffer.clear();
                        for (auto& c : line)
                            m_buffer.append(c);
                        m_cursor = m_buffer.size();
                        m_refresh_needed = true;
                    }
                    m_state = InputState::Free;
                    continue;
                case 'D': // left
                    if (m_cursor > 0) {
                        --m_cursor;
                    }
                    m_state = InputState::Free;
                    continue;
                case 'C': // right
                    if (m_cursor < m_buffer.size()) {
                        ++m_cursor;
                    }
                    m_state = InputState::Free;
                    continue;
                case 'H':
                    m_cursor = 0;
                    m_state = InputState::Free;
                    continue;
                case 'F':
                    m_cursor = m_buffer.size();
                    m_state = InputState::Free;
                    continue;
                case 'Z': // shift+tab
                    reverse_tab = true;
                    m_state = InputState::Free;
                    break;
                case '3':
                    if (m_cursor == m_buffer.size()) {
                        fputc('\a', stdout);
                        fflush(stdout);
                        continue;
                    }
                    m_buffer.remove(m_cursor);
                    m_refresh_needed = true;
                    m_state = InputState::ExpectTerminator;
                    continue;
                default:
                    dbgprintf("Shell: Unhandled final: %02x (%c)\n", ch, ch);
                    m_state = InputState::Free;
                    continue;
                }
                break;
            case InputState::ExpectTerminator:
                m_state = InputState::Free;
                continue;
            case InputState::Free:
                if (ch == 27) {
                    m_state = InputState::ExpectBracket;
                    continue;
                }
                break;
            }

            auto cb = m_key_callbacks.get(ch);
            if (cb.has_value()) {
                if (!cb.value()->callback(*this)) {
                    continue;
                }
            }

            if (ch == '\t' || reverse_tab) {
                if (!on_tab_complete_first_token || !on_tab_complete_other_token)
                    continue;

                bool is_empty_token = m_cursor == 0 || m_buffer[m_cursor - 1] == ' ';

                // reverse tab can count as regular tab here
                m_times_tab_pressed++;

                int token_start = m_cursor - 1;
                if (!is_empty_token) {
                    while (token_start >= 0 && m_buffer[token_start] != ' ')
                        --token_start;
                    ++token_start;
                }

                bool is_first_token = true;
                for (int i = token_start - 1; i >= 0; --i) {
                    if (m_buffer[i] != ' ') {
                        is_first_token = false;
                        break;
                    }
                }

                String token = is_empty_token ? String() : String(&m_buffer[token_start], m_cursor - token_start);

                // ask for completions only on the first tab
                // further tabs simply show the cached completions
                if (m_times_tab_pressed == 1) {
                    if (is_first_token)
                        m_suggestions = on_tab_complete_first_token(token);
                    else
                        m_suggestions = on_tab_complete_other_token(token);
                    m_prompt_lines_at_suggestion_initiation = num_lines();
                }

                // Adjust already incremented / decremented index when switching tab direction
                if (reverse_tab && m_tab_direction != TabDirection::Backward) {
                    decrement_suggestion_index();
                    decrement_suggestion_index();
                    m_tab_direction = TabDirection::Backward;
                }
                if (!reverse_tab && m_tab_direction != TabDirection::Forward) {
                    increment_suggestion_index();
                    increment_suggestion_index();
                    m_tab_direction = TabDirection::Forward;
                }
                reverse_tab = false;

                auto current_suggestion_index = m_next_suggestion_index;
                if (m_next_suggestion_index < m_suggestions.size()) {
                    if (!m_last_shown_suggestion.is_null()) {
                        auto actual_offset = m_times_tab_pressed == 1 ? m_cursor : m_cursor - m_last_shown_suggestion.length() + m_next_suggestion_invariant_offset;
                        for (size_t i = m_next_suggestion_invariant_offset; i < m_last_shown_suggestion.length(); ++i)
                            m_buffer.remove(actual_offset);
                        m_cursor = actual_offset;
                        m_refresh_needed = true;
                    }
                    m_last_shown_suggestion = m_suggestions[m_next_suggestion_index];
                    insert(m_last_shown_suggestion.substring_view(m_next_suggestion_invariant_offset, m_last_shown_suggestion.length() - m_next_suggestion_invariant_offset));
                    if (m_tab_direction == TabDirection::Forward)
                        increment_suggestion_index();
                    else
                        decrement_suggestion_index();
                } else {
                    m_next_suggestion_index = 0;
                }

                if (m_times_tab_pressed > 1 && !m_suggestions.is_empty()) {
                    size_t longest_suggestion_length = 0;

                    for (auto& suggestion : m_suggestions) {
                        longest_suggestion_length = max(longest_suggestion_length, suggestion.length());
                    }

                    size_t num_printed = 0;
                    size_t lines_used { 1 };
                    size_t index { 0 };
                    vt_save_cursor();
                    vt_clear_lines(0, m_lines_used_for_last_suggestions);
                    vt_restore_cursor();
                    auto spans_entire_line { false };
                    auto max_line_count = (m_cached_prompt_length + longest_suggestion_length + m_num_columns - 1) / m_num_columns;
                    if (longest_suggestion_length >= m_num_columns - 2) {
                        spans_entire_line = true;
                        // we should make enough space for the biggest entry in
                        // the suggestion list to fit in the prompt line
                        auto start = max_line_count - m_prompt_lines_at_suggestion_initiation;
                        for (size_t i = start; i < max_line_count; ++i) {
                            putchar('\n');
                        }
                        lines_used += max_line_count;
                        longest_suggestion_length = 0;
                    }
                    vt_move_absolute(max_line_count + m_origin_x, 1);
                    for (auto& suggestion : m_suggestions) {
                        size_t next_column = num_printed + suggestion.length() + longest_suggestion_length + 2;

                        if (next_column > m_num_columns) {
                            auto lines = (suggestion.length() + m_num_columns - 1) / m_num_columns;
                            lines_used += lines;
                            putchar('\n');
                            num_printed = 0;
                        }

                        // show just enough suggestions to fill up the screen
                        // without moving the prompt out of view
                        if (lines_used + m_prompt_lines_at_suggestion_initiation >= m_num_lines)
                            break;

                        if (index == current_suggestion_index) {
                            vt_apply_style({ Style::Foreground(Style::Color::Blue) });
                            fflush(stdout);
                        }

                        if (spans_entire_line) {
                            num_printed += m_num_columns;
                            fprintf(stderr, "%s", suggestion.characters());
                        } else {
                            num_printed += fprintf(stderr, "%-*s", static_cast<int>(longest_suggestion_length) + 2, suggestion.characters());
                        }

                        if (index == current_suggestion_index) {
                            vt_apply_style({});
                            fflush(stdout);
                        }
                        ++index;
                    }
                    m_lines_used_for_last_suggestions = lines_used;

                    // adjust for the case that we scroll up after writing the suggestions
                    if (m_origin_x + lines_used >= m_num_lines) {
                        m_origin_x = m_num_lines - lines_used;
                    }
                    reposition_cursor();
                }
                if (m_suggestions.size() < 2) {
                    // we have none, or just one suggestion
                    // we should just commit that and continue
                    // after it, as if it were auto-completed
                    suggest(0, 0);
                    m_last_shown_suggestion = String::empty();
                    m_suggestions.clear();
                    m_times_tab_pressed = 0;
                }
                continue;
            }

            if (m_times_tab_pressed) {
                // we probably have some suggestions drawn
                // let's clean them up
                if (m_lines_used_for_last_suggestions) {
                    vt_clear_lines(0, m_lines_used_for_last_suggestions);
                    reposition_cursor();
                    m_refresh_needed = true;
                    m_lines_used_for_last_suggestions = 0;
                }
                m_last_shown_suggestion = String::empty();
                m_suggestions.clear();
                suggest(0, 0);
            }
            m_times_tab_pressed = 0; // Safe to say if we get here, the user didn't press TAB

            auto do_backspace = [&] {
                if (m_cursor == 0) {
                    fputc('\a', stdout);
                    fflush(stdout);
                    return;
                }
                m_buffer.remove(m_cursor - 1);
                --m_cursor;
                // we will have to redraw :(
                m_refresh_needed = true;
            };

            if (ch == 8 || ch == m_termios.c_cc[VERASE]) {
                do_backspace();
                continue;
            }
            if (ch == m_termios.c_cc[VWERASE]) {
                bool has_seen_nonspace = false;
                while (m_cursor > 0) {
                    if (isspace(m_buffer[m_cursor - 1])) {
                        if (has_seen_nonspace)
                            break;
                    } else {
                        has_seen_nonspace = true;
                    }
                    do_backspace();
                }
                continue;
            }
            if (ch == m_termios.c_cc[VKILL]) {
                for (size_t i = 0; i < m_cursor; ++i)
                    m_buffer.remove(0);
                m_cursor = 0;
                m_refresh_needed = true;
                continue;
            }
            if (ch == 0xc) {                    // ^L
                printf("\033[3J\033[H\033[2J"); // Clear screen.
                vt_move_absolute(1, 1);
                m_origin_x = 1;
                m_origin_y = 1;
                m_refresh_needed = true;
                continue;
            }
            if (ch == 0x01) { // ^A
                m_cursor = 0;
                continue;
            }
            if (ch == m_termios.c_cc[VEOF]) { // Normally ^D
                if (m_buffer.is_empty()) {
                    printf("<EOF>\n");
                    exit(0);
                }
                continue;
            }
            if (ch == 0x05) { // ^E

                m_cursor = m_buffer.size();
                continue;
            }
            if (ch == '\n') {
                putchar('\n');
                fflush(stdout);
                auto string = String::copy(m_buffer);
                m_buffer.clear();
                return string;
            }

            insert(ch);
        }
    }
}

void Editor::recalculate_origin()
{
    // changing the columns can affect our origin if
    // the new size is smaller than our prompt, which would
    // cause said prompt to take up more space, so we should
    // compensate for that
    if (m_cached_prompt_length >= m_num_columns) {
        auto added_lines = (m_cached_prompt_length + 1) / m_num_columns - 1;
        m_origin_x += added_lines;
    }

    // we also need to recalculate our cursor position
    // but that will be calculated and applied at the next
    // refresh cycle
}
void Editor::refresh_display()
{
    auto cleanup = [&] {
        vt_move_relative(0, m_pending_chars.size() - m_chars_inserted_in_the_middle);
        auto current_line = cursor_line();

        vt_clear_lines(current_line - 1, num_lines() - current_line);
        vt_move_relative(-num_lines() + 1, -offset_in_line() - m_old_prompt_length - m_pending_chars.size() + m_chars_inserted_in_the_middle);
    };
    auto has_cleaned_up = false;
    // someone changed the window size, figure it out
    // and react to it, we might need to redraw
    if (m_was_resized) {
        auto previous_num_columns = m_num_columns;

        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
            m_num_columns = 80;
            m_num_lines = 25;
        } else {
            m_num_columns = ws.ws_col;
            m_num_lines = ws.ws_row;
        }

        if (previous_num_columns != m_num_columns) {
            // we need to cleanup and redo everything
            m_cached_prompt_valid = false;
            m_refresh_needed = true;
            swap(previous_num_columns, m_num_columns);
            recalculate_origin();
            cleanup();
            swap(previous_num_columns, m_num_columns);
            has_cleaned_up = true;
        }
    }
    // do not call hook on pure cursor movement
    if (m_cached_prompt_valid && !m_refresh_needed && m_pending_chars.size() == 0) {
        // probably just moving around
        reposition_cursor();
        m_cached_buffer_size = m_buffer.size();
        return;
    }

    if (on_display_refresh)
        on_display_refresh(*this);

    if (m_cached_prompt_valid) {
        if (!m_refresh_needed && m_cursor == m_buffer.size()) {
            // just write the characters out and continue
            // no need to refresh the entire line
            char null = 0;
            m_pending_chars.append(&null, 1);
            fputs((char*)m_pending_chars.data(), stdout);
            m_pending_chars.clear();
            m_drawn_cursor = m_cursor;
            m_cached_buffer_size = m_buffer.size();
            fflush(stdout);
            return;
        }
    }

    // ouch, reflow entire line
    // FIXME: handle multiline stuff
    if (!has_cleaned_up) {
        cleanup();
    }
    vt_move_absolute(m_origin_x, m_origin_y);

    fputs(m_new_prompt.characters(), stdout);

    vt_clear_to_end_of_line();
    HashMap<u32, Style> empty_styles {};
    for (size_t i = 0; i < m_buffer.size(); ++i) {
        auto ends = m_spans_ending.get(i).value_or(empty_styles);
        auto starts = m_spans_starting.get(i).value_or(empty_styles);
        if (ends.size()) {
            // go back to defaults
            vt_apply_style(find_applicable_style(i));
        }
        if (starts.size()) {
            // set new options
            vt_apply_style(starts.begin()->value); // apply some random style that starts here
        }
        fputc(m_buffer[i], stdout);
    }
    vt_apply_style({}); // don't bleed to EOL
    m_pending_chars.clear();
    m_refresh_needed = false;
    m_cached_buffer_size = m_buffer.size();
    m_chars_inserted_in_the_middle = 0;
    if (!m_cached_prompt_valid) {
        m_cached_prompt_valid = true;
    }

    reposition_cursor();
    fflush(stdout);
}

void Editor::reposition_cursor()
{
    m_drawn_cursor = m_cursor;

    auto line = cursor_line() - 1;
    auto column = offset_in_line();

    vt_move_absolute(line + m_origin_x, column + m_origin_y);
}

void Editor::vt_move_absolute(u32 x, u32 y)
{
    printf("\033[%d;%dH", x, y);
    fflush(stdout);
}

void Editor::vt_move_relative(int x, int y)
{
    char x_op = 'A', y_op = 'D';

    if (x > 0)
        x_op = 'B';
    else
        x = -x;
    if (y > 0)
        y_op = 'C';
    else
        y = -y;

    if (x > 0)
        printf("\033[%d%c", x, x_op);
    if (y > 0)
        printf("\033[%d%c", y, y_op);
}

Style Editor::find_applicable_style(size_t offset) const
{
    // walk through our styles and find one that fits in the offset
    for (auto& entry : m_spans_starting) {
        if (entry.key > offset)
            continue;
        for (auto& style_value : entry.value) {
            if (style_value.key <= offset)
                continue;
            return style_value.value;
        }
    }
    return {};
}

void Editor::vt_apply_style(const Style& style)
{
    printf(
        "\033[%d;%d;%d;%d;%dm",
        style.bold() ? 1 : 22,
        style.underline() ? 4 : 24,
        style.italic() ? 3 : 23,
        (int)style.foreground() + 30,
        (int)style.background() + 40);
}

void Editor::vt_clear_lines(size_t count_above, size_t count_below)
{
    // go down count_below lines
    if (count_below > 0)
        printf("\033[%dB", (int)count_below);
    // then clear lines going upwards
    for (size_t i = count_below + count_above; i > 0; --i)
        fputs(i == 1 ? "\033[2K" : "\033[2K\033[A", stdout);
}

void Editor::vt_save_cursor()
{
    fputs("\033[s", stdout);
    fflush(stdout);
}

void Editor::vt_restore_cursor()
{
    fputs("\033[u", stdout);
    fflush(stdout);
}

void Editor::vt_clear_to_end_of_line()
{
    fputs("\033[K", stdout);
    fflush(stdout);
}

size_t Editor::actual_rendered_string_length(const StringView& string) const
{
    size_t length { 0 };
    enum VTState {
        Free = 1,
        Escape = 3,
        Bracket = 5,
        BracketArgsSemi = 7,
        Title = 9,
    } state { Free };
    for (size_t i = 0; i < string.length(); ++i) {
        auto c = string[i];
        switch (state) {
        case Free:
            if (c == '\x1b') {
                // escape
                state = Escape;
                continue;
            }
            // FIXME: This will not support anything sophisticated
            ++length;
            break;
        case Escape:
            if (c == ']') {
                if (string.length() > i && string[i + 1] == '0')
                    state = Title;
                continue;
            }
            if (c == '[') {
                state = Bracket;
                continue;
            }
            // FIXME: This does not support non-VT (aside from set-title) escapes
            break;
        case Bracket:
            if (isdigit(c)) {
                state = BracketArgsSemi;
                continue;
            }
            break;
        case BracketArgsSemi:
            if (c == ';') {
                state = Bracket;
                continue;
            }
            if (!isdigit(c))
                state = Free;
            break;
        case Title:
            if (c == 7)
                state = Free;
            break;
        }
    }
    return length;
}

Vector<size_t, 2> Editor::vt_dsr()
{
    fputs("\033[6n", stdout);
    fflush(stdout);

    char buf[16];
    u32 length { 0 };
    do {
        auto nread = read(0, buf + length, 16 - length);
        if (nread < 0) {
            if (errno == 0) {
                // ????
                continue;
            }
            dbg() << "Error while reading DSR: " << strerror(errno);
            return { 1, 1 };
        }
        if (nread == 0) {
            dbg() << "Terminal DSR issue; received no response";
            return { 1, 1 };
        }
        length += nread;
    } while (buf[length - 1] != 'R' && length < 16);
    size_t x { 1 }, y { 1 };

    if (buf[0] == '\033' && buf[1] == '[') {
        auto parts = StringView(buf + 2, length - 3).split_view(';');
        bool ok;
        x = parts[0].to_int(ok);
        if (!ok) {
            dbg() << "Terminal DSR issue; received garbage x";
        }
        y = parts[1].to_int(ok);
        if (!ok) {
            dbg() << "Terminal DSR issue; received garbage y";
        }
    }
    return { x, y };
}
}
