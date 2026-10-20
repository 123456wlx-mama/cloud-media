// Phase 6 T6-6：Codec / Buffer 模糊测试驱动（独立可执行，非 gtest）。
//
// 设计目标（对应 docs/specs/02-协议设计.md 与 docs/plans/phase-2.md 的 Deferred）：
//   - 1M 次随机输入喂给 Codec::decode / decodeBulk / Buffer，验证不崩溃、不越界；
//   - 确定性 PRNG（固定种子），任何一轮崩溃都可复现；
//   - 配合 -DENABLE_ASAN=ON 编译运行，靠 ASan/UBSan 捕获内存错误；
//   - 分三类输入：结构化变异（能打到 CRC/类型/长度深层分支）、纯随机、Buffer 操作。
//
// 用法：cm_fuzz_driver [iterations]       默认 1000000
// 退出码：0 表示全部迭代无崩溃。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cm/base/buffer.h"
#include "cm/protocol/codec.h"

namespace {

// xorshift64* —— 固定种子，保证可复现。
std::uint64_t g_state = 0x9E3779B97F4A7C15ull;

std::uint64_t rnd() {
    std::uint64_t x = g_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_state = x;
    return x * 2685821657736338717ull;
}

std::size_t rndRange(std::size_t lo, std::size_t hi) {
    if (hi <= lo) {
        return lo;
    }
    return lo + static_cast<std::size_t>(rnd() % (hi - lo));
}

std::string randomBytes(std::size_t len) {
    std::string s(len, '\0');
    for (std::size_t j = 0; j < len; ++j) {
        s[j] = static_cast<char>(rnd() & 0xFFu);
    }
    return s;
}

// 统计各 DecodeStatus 分布（用于证明深层分支确实被覆盖）。
std::size_t g_ok = 0;
std::size_t g_needMore = 0;
std::size_t g_protoErr = 0;

const std::uint16_t kKnownTypes[] = {
    10002, 10006, 11003, 11002, 10030, 10012, 10013
};

std::string makeValidFrame() {
    const std::uint16_t type = kKnownTypes[rndRange(0, sizeof(kKnownTypes) / sizeof(kKnownTypes[0]))];
    const std::size_t payloadLen = rndRange(0, 256);
    std::string frame;
    if (cm::protocol::Codec::encode(type, static_cast<std::uint32_t>(rnd()),
                                    randomBytes(payloadLen), &frame) != cm::ErrorCode::Ok) {
        return std::string();
    }
    return frame;
}

void countStatus(cm::protocol::DecodeStatus st) {
    switch (st) {
        case cm::protocol::DecodeStatus::Ok: ++g_ok; break;
        case cm::protocol::DecodeStatus::NeedMore: ++g_needMore; break;
        default: ++g_protoErr; break;
    }
}

// 把 data 喂给 decode 与 decodeBulk。chunked=true 时分块流式喂入，
// 覆盖半包（NeedMore）与粘包路径。
void feedCodec(const std::string& data, bool chunked) {
    cm::Buffer buf;
    if (!chunked) {
        buf.append(data);
    } else {
        std::size_t off = 0;
        while (off < data.size()) {
            std::size_t chunk = rndRange(1, 40);
            if (chunk > data.size() - off) {
                chunk = data.size() - off;
            }
            buf.append(data.data() + off, chunk);
            off += chunk;
            cm::protocol::Frame f;
            cm::protocol::DecodeStatus st = cm::protocol::DecodeStatus::Ok;
            (void)cm::protocol::Codec::decode(buf, &f, &st);
            countStatus(st);
        }
    }

    cm::protocol::Frame f;
    cm::protocol::DecodeStatus st = cm::protocol::DecodeStatus::Ok;
    cm::ErrorCode rc = cm::protocol::Codec::decode(buf, &f, &st);
    (void)rc;
    countStatus(st);

    // decodeBulk 用独立 Buffer（会消费数据），同样做一次。
    cm::Buffer bulk;
    bulk.append(data);
    cm::protocol::RawFrame raw;
    cm::protocol::DecodeStatus bst = cm::protocol::DecodeStatus::Ok;
    cm::ErrorCode brc = cm::protocol::Codec::decodeBulk(bulk, &raw, &bst);
    (void)brc;
    countStatus(bst);
}

// 结构化变异：从合法帧出发做字节翻转/截断/字段篡改，能命中深层校验分支。
std::string mutateFrame(std::string frame) {
    const std::size_t size = frame.size();
    const int op = static_cast<int>(rnd() % 7);
    switch (op) {
        case 0: {  // 截断到随机长度
            const std::size_t cut = rndRange(0, size > 0 ? size + 1 : 1);
            frame.resize(cut);
            break;
        }
        case 1: {  // 翻转 1~4 个随机字节
            const int flips = static_cast<int>(rndRange(1, 5));
            for (int i = 0; i < flips && !frame.empty(); ++i) {
                const std::size_t pos = rndRange(0, frame.size());
                frame[pos] = static_cast<char>(frame[pos] ^ static_cast<char>(1u << (rnd() % 8)));
            }
            break;
        }
        case 2: {  // 篡改长度字段（offset 7..10）
            if (size >= 11) {
                const std::uint32_t len = static_cast<std::uint32_t>(rnd());
                frame[7] = static_cast<char>((len >> 24) & 0xFFu);
                frame[8] = static_cast<char>((len >> 16) & 0xFFu);
                frame[9] = static_cast<char>((len >> 8) & 0xFFu);
                frame[10] = static_cast<char>(len & 0xFFu);
            }
            break;
        }
        case 3: {  // 篡改类型字段（offset 5..6）
            if (size >= 7) {
                const std::uint16_t type = static_cast<std::uint16_t>(rnd());
                frame[5] = static_cast<char>((type >> 8) & 0xFFu);
                frame[6] = static_cast<char>(type & 0xFFu);
            }
            break;
        }
        case 4: {  // 篡改版本字节（offset 4）
            if (size >= 5) {
                frame[4] = static_cast<char>(rnd() & 0xFFu);
            }
            break;
        }
        case 5: {  // 篡改魔数（offset 0..3）
            if (size >= 4) {
                frame[0] = static_cast<char>(rnd() & 0xFFu);
                frame[1] = static_cast<char>(rnd() & 0xFFu);
            }
            break;
        }
        case 6: {  // 追加随机尾部（粘包）
            frame.append(randomBytes(rndRange(0, 64)));
            break;
        }
    }
    return frame;
}

void structuredCodec() {
    const std::string base = makeValidFrame();
    if (base.empty()) {
        return;
    }
    std::string data = mutateFrame(base);
    feedCodec(data, (rnd() % 2) == 0);
}

void randomCodec() {
    const std::size_t len = rndRange(0, 65536);
    feedCodec(randomBytes(len), (rnd() % 2) == 0);
}

// Buffer 操作模糊：随机 append/retrieve/retrieveAsString/shrinkTo/reset，
// 校验不变量（可读字节与 peek/retrieve 行为一致）。
void bufferFuzz() {
    cm::Buffer buf;
    const int ops = static_cast<int>(rndRange(1, 24));
    for (int i = 0; i < ops; ++i) {
        const int op = static_cast<int>(rnd() % 6);
        switch (op) {
            case 0:
                buf.append(randomBytes(rndRange(0, 4096)));
                break;
            case 1: {
                const std::size_t readable = buf.readableBytes();
                if (readable > 0) {
                    const char* p = buf.peek();
                    if (p == nullptr) {
                        std::fprintf(stderr, "INVARIANT: peek()==nullptr while readable=%zu\n",
                                     static_cast<std::size_t>(readable));
                        std::exit(3);
                    }
                }
                break;
            }
            case 2: {
                const std::size_t n = rndRange(0, 64);
                const std::size_t before = buf.readableBytes();
                buf.retrieve(n);
                if (n <= before && buf.readableBytes() != before - n) {
                    std::fprintf(stderr, "INVARIANT: retrieve(%zu) changed readable wrong\n",
                                 static_cast<std::size_t>(n));
                    std::exit(3);
                }
                break;
            }
            case 3:
                (void)buf.retrieveAsString(rndRange(0, 64));
                break;
            case 4:
                buf.shrinkTo(rndRange(0, 4096));
                break;
            case 5:
                buf.reset();
                break;
        }
    }
    (void)buf.retrieveAllAsString();
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t iterations = 1000000;
    if (argc > 1) {
        iterations = std::strtoull(argv[1], 0, 10);
    }
    std::fprintf(stderr, "fuzz: %llu iterations, seed fixed\n",
                 static_cast<unsigned long long>(iterations));

    for (std::uint64_t i = 0; i < iterations; ++i) {
        const int sel = static_cast<int>(rnd() % 100);
        if (sel < 50) {
            structuredCodec();
        } else if (sel < 85) {
            randomCodec();
        } else {
            bufferFuzz();
        }

        if ((i + 1) % 100000 == 0) {
            std::fprintf(stderr, "  ... %llu / %llu\n",
                         static_cast<unsigned long long>(i + 1),
                         static_cast<unsigned long long>(iterations));
        }
    }

    std::fprintf(stderr,
                 "FUZZ OK: ok=%zu needMore=%zu protoErr=%zu (no crash)\n",
                 g_ok, g_needMore, g_protoErr);
    return 0;
}
