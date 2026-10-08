#pragma once

// RFC 1951 raw DEFLATE 解码器（自己实现，零第三方依赖）。
//
// 为什么必须自己写：真实 .msix 有 335/344 个条目是 DEFLATE 压缩的（实测），而 Windows 的
// Compression API（cabinet.dll 的 CreateDecompressor）只提供 MSZIP/XPRESS/LZMS，没有 raw DEFLATE。
// 另一个可选方案是 appxpackaging.dll 的 IAppxPackageReader，但那样就把「读容器」这件事
// 交给了一个只在特定 Windows 版本行为稳定的 API，而我们自己要读的 .msixpkg 与 .msix 都是 ZIP。
//
// 设计：调用方保证压缩数据在内存里完整可见（容器用内存映射文件），因此只需要处理
// 「输出分段」。输出用 64 KiB 缓冲回调，历史窗口是 32 KiB 环形缓冲。
//
// 性能注记：采用 puff 风格的逐位 Huffman 解码，约 10–30 MB/s。
// 校验路径只解压 AppxManifest.xml(≈2.7 KB) 与 AppxSignature.p7x(≈1.5 KB)，
// 因此不追求解码吞吐；若将来需要解压大载荷，再换成查表解码。

#include <cstdint>
#include <functional>

#include "msixpkg/errors.h"

namespace msixpkg {

class Inflater {
public:
    /// 输出回调：按顺序接收解压后的字节块；返回的 buffer 只在回调期间有效。
    using Sink = std::function<void(const uint8_t* data, size_t size)>;

    Inflater(const uint8_t* input, size_t input_size, Sink sink);

    /// 解码全部并返回输出字节数；遇到非法流抛 E_CONTAINER_INVALID。
    uint64_t run();

private:
    // ---- 位读取 ----
    int read_bit();
    int read_bits(int count);

    // ---- 输出 ----
    void emit(uint8_t byte);
    void copy_from_window(uint32_t distance, uint32_t length);
    void flush();

    // ---- 块 ----
    void read_stored_block();
    void read_huffman_block(bool dynamic);

    const uint8_t* input_;
    size_t input_size_;
    size_t input_pos_ = 0;

    uint32_t bit_buffer_ = 0;
    int bit_count_ = 0;

    Sink sink_;
    uint8_t chunk_[64 * 1024];
    size_t chunk_used_ = 0;
    uint8_t window_[32 * 1024];
    size_t window_pos_ = 0;
    uint64_t total_output_ = 0;
    bool finished_ = false;
};

/// CRC-32（IEEE 802.3，ZIP 用的那个），用于校验每个条目。
uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed = 0);

}  // namespace msixpkg
