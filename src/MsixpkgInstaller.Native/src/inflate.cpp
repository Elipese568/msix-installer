#include "inflate.h"

#include <array>
#include <cstring>
#include <vector>

namespace msixpkg {
namespace {

[[noreturn]] void malformed(const std::string& detail) {
    fail(codes::ContainerInvalid, "The DEFLATE stream is malformed.", detail);
}

// ---- RFC 1951 常量 ----

constexpr std::array<int, 19> kCodeLengthOrder = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr std::array<int, 29> kLengthBase = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};

constexpr std::array<int, 29> kLengthExtra = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

constexpr std::array<int, 30> kDistanceBase = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577};

constexpr std::array<int, 30> kDistanceExtra = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// 规范 Huffman 表：counts[len] = 该码长的码字个数；symbols = 按码字升序排列的符号。
struct HuffmanTable {
    std::array<int, 16> counts{};
    std::vector<int> symbols;
    int max_length = 0;
};

HuffmanTable build_table(const std::vector<int>& lengths, int symbol_count) {
    HuffmanTable table;
    table.counts.fill(0);
    table.symbols.assign(static_cast<size_t>(symbol_count), 0);

    for (int symbol = 0; symbol < symbol_count; ++symbol) {
        const int length = lengths[static_cast<size_t>(symbol)];
        if (length < 0 || length > 15) {
            malformed("a code length is out of range");
        }
        ++table.counts[static_cast<size_t>(length)];
        table.max_length = (length > table.max_length) ? length : table.max_length;
    }

    table.counts[0] = 0;  // 长度 0 表示该符号不参与编码

    // 校验 Krasft 不等式（码字总数不能超过该长度可表示的数量）。
    int left = 1;
    for (int length = 1; length <= 15; ++length) {
        left <<= 1;
        left -= table.counts[static_cast<size_t>(length)];
        if (left < 0) {
            malformed("the Huffman code is over-subscribed");
        }
    }

    std::array<int, 16> offsets{};
    for (int length = 1; length < 15; ++length) {
        offsets[static_cast<size_t>(length + 1)] = offsets[static_cast<size_t>(length)] + table.counts[static_cast<size_t>(length)];
    }

    for (int symbol = 0; symbol < symbol_count; ++symbol) {
        const int length = lengths[static_cast<size_t>(symbol)];
        if (length != 0) {
            table.symbols[static_cast<size_t>(offsets[static_cast<size_t>(length)]++)] = symbol;
        }
    }

    return table;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed) {
    static std::array<uint32_t, 256> table{};
    static bool initialised = false;
    if (!initialised) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            table[i] = value;
        }
        initialised = true;
    }

    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

Inflater::Inflater(const uint8_t* input, size_t input_size, Sink sink)
    : input_(input), input_size_(input_size), sink_(std::move(sink)) {
    std::memset(chunk_, 0, sizeof(chunk_));
    std::memset(window_, 0, sizeof(window_));
}

int Inflater::read_bit() {
    if (bit_count_ == 0) {
        if (input_pos_ >= input_size_) {
            malformed("the stream ended in the middle of a bit field");
        }
        bit_buffer_ = input_[input_pos_++];
        bit_count_ = 8;
    }

    const int bit = static_cast<int>(bit_buffer_ & 1u);
    bit_buffer_ >>= 1;
    --bit_count_;
    return bit;
}

int Inflater::read_bits(int count) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
        value |= read_bit() << i;
    }
    return value;
}

void Inflater::emit(uint8_t byte) {
    chunk_[chunk_used_++] = byte;
    window_[window_pos_] = byte;
    window_pos_ = (window_pos_ + 1) % sizeof(window_);
    ++total_output_;

    if (chunk_used_ == sizeof(chunk_)) {
        flush();
    }
}

void Inflater::copy_from_window(uint32_t distance, uint32_t length) {
    if (distance == 0 || distance > sizeof(window_)) {
        malformed("a back-reference distance is out of range");
    }

    // 逐字节复制即可正确处理重叠（run-length）情形。
    size_t source = (window_pos_ + sizeof(window_) - distance) % sizeof(window_);
    for (uint32_t i = 0; i < length; ++i) {
        const uint8_t byte = window_[source];
        source = (source + 1) % sizeof(window_);
        emit(byte);
    }
}

void Inflater::flush() {
    if (chunk_used_ == 0) {
        return;
    }

    if (sink_) {
        sink_(chunk_, chunk_used_);
    }
    chunk_used_ = 0;
}

void Inflater::read_stored_block() {
    bit_count_ = 0;  // 存放块的起始按字节对齐
    bit_buffer_ = 0;

    if (input_pos_ + 4 > input_size_) {
        malformed("a stored block header is truncated");
    }

    const uint32_t length = static_cast<uint32_t>(input_[input_pos_]) | (static_cast<uint32_t>(input_[input_pos_ + 1]) << 8);
    const uint32_t inverse = static_cast<uint32_t>(input_[input_pos_ + 2]) | (static_cast<uint32_t>(input_[input_pos_ + 3]) << 8);
    input_pos_ += 4;

    if ((length ^ 0xFFFFu) != inverse) {
        malformed("a stored block length check failed");
    }

    if (input_pos_ + length > input_size_) {
        malformed("a stored block is truncated");
    }

    for (uint32_t i = 0; i < length; ++i) {
        emit(input_[input_pos_ + i]);
    }
    input_pos_ += length;
}

void Inflater::read_huffman_block(bool dynamic) {
    std::vector<int> literal_lengths(288, 0);
    std::vector<int> distance_lengths(32, 0);

    if (dynamic) {
        const int literal_count = read_bits(5) + 257;
        const int distance_count = read_bits(5) + 1;
        const int code_length_count = read_bits(4) + 4;

        if (literal_count > 286 || distance_count > 30) {
            malformed("a dynamic block declares too many symbols");
        }

        std::vector<int> code_lengths(19, 0);
        for (int i = 0; i < code_length_count; ++i) {
            code_lengths[static_cast<size_t>(kCodeLengthOrder[static_cast<size_t>(i)])] = read_bits(3);
        }

        const HuffmanTable code_table = build_table(code_lengths, 19);

        int index = 0;
        const int total = literal_count + distance_count;
        std::vector<int> lengths(static_cast<size_t>(total), 0);
        while (index < total) {
            const int symbol = [&] {
                // decode_symbol 的等价实现：用 canonical 码逐位下降。
                int code = 0;
                int first = 0;
                int offset = 0;
                for (int length = 1; length <= code_table.max_length; ++length) {
                    code |= read_bit();
                    const int count = code_table.counts[static_cast<size_t>(length)];
                    if (code - first < count) {
                        return code_table.symbols[static_cast<size_t>(offset + (code - first))];
                    }
                    offset += count;
                    first = (first + count) << 1;
                    code <<= 1;
                }
                malformed("an invalid code-length symbol was decoded");
            }();

            if (symbol < 16) {
                lengths[static_cast<size_t>(index++)] = symbol;
                continue;
            }

            int repeat = 0;
            int value = 0;
            if (symbol == 16) {
                if (index == 0) {
                    malformed("a repeat-previous code length appears at the start of the block");
                }
                value = lengths[static_cast<size_t>(index - 1)];
                repeat = 3 + read_bits(2);
            } else if (symbol == 17) {
                repeat = 3 + read_bits(3);
            } else {
                repeat = 11 + read_bits(7);
            }

            if (index + repeat > total) {
                malformed("a code-length repeat overruns the table");
            }

            for (int i = 0; i < repeat; ++i) {
                lengths[static_cast<size_t>(index++)] = value;
            }
        }

        for (int i = 0; i < literal_count; ++i) {
            literal_lengths[static_cast<size_t>(i)] = lengths[static_cast<size_t>(i)];
        }
        for (int i = 0; i < distance_count; ++i) {
            distance_lengths[static_cast<size_t>(i)] = lengths[static_cast<size_t>(literal_count + i)];
        }
    } else {
        for (int i = 0; i <= 143; ++i) literal_lengths[static_cast<size_t>(i)] = 8;
        for (int i = 144; i <= 255; ++i) literal_lengths[static_cast<size_t>(i)] = 9;
        for (int i = 256; i <= 279; ++i) literal_lengths[static_cast<size_t>(i)] = 7;
        for (int i = 280; i <= 287; ++i) literal_lengths[static_cast<size_t>(i)] = 8;
        for (int i = 0; i < 32; ++i) distance_lengths[static_cast<size_t>(i)] = 5;
    }

    const HuffmanTable literal_table = build_table(literal_lengths, 288);
    const HuffmanTable distance_table = build_table(distance_lengths, 32);

    const auto decode = [&](const HuffmanTable& table) {
        int code = 0;
        int first = 0;
        int offset = 0;
        for (int length = 1; length <= table.max_length; ++length) {
            code |= read_bit();
            const int count = table.counts[static_cast<size_t>(length)];
            if (code - first < count) {
                return table.symbols[static_cast<size_t>(offset + (code - first))];
            }
            offset += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        malformed("an invalid Huffman code was decoded");
    };

    while (true) {
        const int symbol = decode(literal_table);
        if (symbol == 256) {
            return;
        }

        if (symbol < 256) {
            emit(static_cast<uint8_t>(symbol));
            continue;
        }

        const int length_index = symbol - 257;
        if (length_index < 0 || length_index >= static_cast<int>(kLengthBase.size())) {
            malformed("an invalid length symbol was decoded");
        }

        const uint32_t length = static_cast<uint32_t>(kLengthBase[static_cast<size_t>(length_index)]) +
                                static_cast<uint32_t>(read_bits(kLengthExtra[static_cast<size_t>(length_index)]));

        const int distance_symbol = decode(distance_table);
        if (distance_symbol < 0 || distance_symbol >= static_cast<int>(kDistanceBase.size())) {
            malformed("an invalid distance symbol was decoded");
        }

        const uint32_t distance = static_cast<uint32_t>(kDistanceBase[static_cast<size_t>(distance_symbol)]) +
                                  static_cast<uint32_t>(read_bits(kDistanceExtra[static_cast<size_t>(distance_symbol)]));

        copy_from_window(distance, length);
    }
}

uint64_t Inflater::run() {
    while (true) {
        const int final_block = read_bit();
        const int block_type = read_bits(2);

        switch (block_type) {
            case 0:
                read_stored_block();
                break;
            case 1:
                read_huffman_block(false);
                break;
            case 2:
                read_huffman_block(true);
                break;
            default:
                malformed("an invalid block type was encountered");
        }

        if (final_block != 0) {
            finished_ = true;
            break;
        }
    }

    flush();
    return total_output_;
}

}  // namespace msixpkg
