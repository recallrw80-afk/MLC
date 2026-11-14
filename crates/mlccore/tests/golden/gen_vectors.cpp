// gen_vectors.cpp — golden 测试向量生成器（MLC Rust 迁移，加密兼容层）
//
// 从 sdk/src/util/crypto_utils.cpp 原样拷贝 DES 实现（quint64→uint64_t、
// QByteArray→std::string，字节语义逐一对应），补 MD5（RFC 1321）与 base64，
// 用于生成 Rust 侧 tests/golden/vectors.txt 的对照向量。
//
// 重新生成：g++ -O2 -o gen_vectors gen_vectors.cpp && ./gen_vectors > vectors.txt
// MD5 自检：程序内置 ("", "abc") 两个已知向量校验，不符则中止（防 K 表/移位写错）。
//
// 注意：C++ 原实现的 DES 位序是自洽变体（未必等同标准 DES/.NET），
// 兼容对象是它本身——此处代码必须与 crypto_utils.cpp 保持逐行一致，
// 改动任意一行都会导致与已发布版本的用户数据（MLC.ini 加密字段）不兼容。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef uint64_t quint64;
typedef uint32_t quint32;
typedef uint8_t quint8;

// ===================== 以下与 crypto_utils.cpp 逐行对应 =====================

static const int IP[] = {
    58, 50, 42, 34, 26, 18, 10, 2,  60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,  64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1,  59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,  63, 55, 47, 39, 31, 23, 15, 7
};

static const int FP[] = {
    40, 8, 48, 16, 56, 24, 64, 32,  39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,  37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,  35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,  33, 1, 41, 9,  49, 17, 57, 25
};

static const int E[] = {
    32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13,
    12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1
};

static const int S[8][4][16] = {
    {{14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7},
     {0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8},
     {4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0},
     {15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13}},
    {{15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10},
     {3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5},
     {0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15},
     {13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9}},
    {{10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8},
     {13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1},
     {13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7},
     {1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12}},
    {{7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15},
     {13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9},
     {10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4},
     {3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14}},
    {{2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9},
     {14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6},
     {4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14},
     {11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3}},
    {{12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11},
     {10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8},
     {9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6},
     {4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13}},
    {{4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1},
     {13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6},
     {1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2},
     {6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12}},
    {{13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7},
     {1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2},
     {7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8},
     {2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}}
};

static const int P[] = {
    16, 7,  20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
    2,  8,  24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25
};

static const int PC1[] = {
    57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
    10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
    14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4
};

static const int PC2[] = {
    14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
    26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
    51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32
};

static const int SHIFTS[] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

static quint64 permute(quint64 input, const int *table, int n, quint64 mask) {
    quint64 result = 0;
    for (int i = 0; i < n; ++i) {
        int bit = table[i] - 1;
        if (input & (1ULL << bit)) result |= (1ULL << i);
    }
    return result & mask;
}

static quint64 permute64to56(quint64 key) {
    quint64 result = 0;
    for (int i = 0; i < 56; ++i) {
        int bit = PC1[i] - 1;
        if (key & (1ULL << (63 - bit))) result |= (1ULL << (55 - i));
    }
    return result;
}

static quint64 permute56to48(quint64 key) {
    quint64 result = 0;
    for (int i = 0; i < 48; ++i) {
        int bit = PC2[i] - 1;
        if (key & (1ULL << (55 - bit))) result |= (1ULL << (47 - i));
    }
    return result;
}

static quint64 permuteIP(quint64 block) { return permute(block, IP, 64, 0xFFFFFFFFFFFFFFFFULL); }
static quint64 permuteFP(quint64 block) { return permute(block, FP, 64, 0xFFFFFFFFFFFFFFFFULL); }

static quint32 f(quint32 r, quint64 subkey48) {
    quint64 expanded = 0;
    for (int i = 0; i < 48; ++i) {
        int bit = E[i] - 1;
        if (r & (1ULL << (31 - bit))) expanded |= (1ULL << (47 - i));
    }
    expanded ^= subkey48;

    quint32 result = 0;
    for (int box = 0; box < 8; ++box) {
        int shift = 42 - box * 6;
        quint8 sixBits = (expanded >> shift) & 0x3F;
        int row = ((sixBits >> 4) & 0x02) | (sixBits & 0x01);
        int col = (sixBits >> 1) & 0x0F;
        result = (result << 4) | S[box][row][col];
    }

    quint32 permuted = 0;
    for (int i = 0; i < 32; ++i) {
        int bit = P[i] - 1;
        if (result & (1ULL << (31 - bit))) permuted |= (1ULL << (31 - i));
    }
    return permuted;
}

static quint64 desEncrypt64(quint64 block, const std::string &keyBytes) {
    quint64 key = 0;
    for (int i = 0; i < 8; ++i) key = (key << 8) | (unsigned char)keyBytes[i];

    quint64 pc1 = permute64to56(key);
    quint32 c = (pc1 >> 28) & 0x0FFFFFFF;
    quint32 d = pc1 & 0x0FFFFFFF;

    quint64 subkeys[16];
    for (int round = 0; round < 16; ++round) {
        c = ((c << SHIFTS[round]) | (c >> (28 - SHIFTS[round]))) & 0x0FFFFFFF;
        d = ((d << SHIFTS[round]) | (d >> (28 - SHIFTS[round]))) & 0x0FFFFFFF;
        quint64 cd = ((quint64)c << 28) | d;
        subkeys[round] = permute56to48(cd);
    }

    block = permuteIP(block);
    quint32 left = (block >> 32) & 0xFFFFFFFF;
    quint32 right = block & 0xFFFFFFFF;

    for (int round = 0; round < 16; ++round) {
        quint32 temp = left;
        left = right;
        right = temp ^ f(right, subkeys[round]);
    }

    block = ((quint64)right << 32) | left;
    block = permuteFP(block);
    return block;
}

static std::string padPkcs7(const std::string &data, int blockSize = 8) {
    int padLen = blockSize - (int)(data.size() % blockSize);
    std::string padded = data;
    padded.append((size_t)padLen, (char)padLen);
    return padded;
}

static std::string desEcbEncrypt(const std::string &data, const std::string &keyBytes) {
    std::string result;
    for (size_t i = 0; i < data.size(); i += 8) {
        std::string block = data.substr(i, 8);
        while (block.size() < 8) block += '\0';
        quint64 blockVal = 0;
        for (int j = 0; j < 8; ++j) blockVal = (blockVal << 8) | (unsigned char)block[j];
        blockVal = desEncrypt64(blockVal, keyBytes);
        for (int j = 7; j >= 0; --j) result += (char)((blockVal >> (j * 8)) & 0xFF);
    }
    return result;
}

// ===================== 辅助：MD5 与 base64 =====================

// MD5（RFC 1321）——deriveDesKey 用 MD5(UTF-8 key) 的前 8 个原始字节
struct MD5 {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    uint64_t total = 0;
    unsigned char buf[64];
    size_t bl = 0;

    static int ss(int i) {
        static const int s[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
        return s[i];
    }
    static uint32_t k(int i) {
        // K[i] = floor(|sin(i+1)| * 2^32)；main 里用已知向量自检兜底
        return (uint32_t)floor(fabs(sin((double)i + 1.0)) * 4294967296.0);
    }
    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void block(const unsigned char *p) {
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = (uint32_t)p[4*i] | ((uint32_t)p[4*i+1] << 8) |
                   ((uint32_t)p[4*i+2] << 16) | ((uint32_t)p[4*i+3] << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t Fv; int g;
            if (i < 16)      { Fv = (B & C) | (~B & D); g = i; }
            else if (i < 32) { Fv = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { Fv = B ^ C ^ D;          g = (3*i + 5) % 16; }
            else             { Fv = C ^ (B | ~D);       g = (7*i) % 16; }
            Fv = Fv + A + k(i) + M[g];
            A = D; D = C; C = B;
            B = B + rotl(Fv, ss(i));
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    void update(const unsigned char *p, size_t n) {
        total += n;
        while (n) {
            if (bl == 64) { block(buf); bl = 0; }
            size_t take = n < (64 - bl) ? n : (64 - bl);
            memcpy(buf + bl, p, take);
            bl += take; p += take; n -= take;
        }
    }
    std::string raw() {
        uint64_t msg = total;
        unsigned char p80 = 0x80; update(&p80, 1);
        unsigned char z = 0; while (bl != 56) update(&z, 1);
        unsigned char l[8];
        uint64_t bits = msg * 8;
        for (int i = 0; i < 8; ++i) l[i] = (unsigned char)((bits >> (8*i)) & 0xFF);
        memcpy(buf + 56, l, 8); block(buf); bl = 0;
        uint32_t w[4] = {a0, b0, c0, d0};
        std::string out(16, '\0');
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out[4*i + j] = (char)((w[i] >> (8*j)) & 0xFF);
        return out;
    }
};

static std::string md5raw(const std::string &s) {
    MD5 m;
    m.update((const unsigned char *)s.data(), s.size());
    return m.raw();
}

static std::string md5hex(const std::string &s) {
    std::string r = md5raw(s), h;
    h.reserve(32);
    char buf[3];
    for (unsigned char c : r) { sprintf(buf, "%02x", c); h += buf; }
    return h;
}

static std::string b64encode(const std::string &in) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8) | (unsigned char)in[i+2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t v = (unsigned char)in[i] << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += "=";
    }
    return out;
}

// ===================== 生成入口 =====================

int main() {
    // MD5 自检（RFC 1321 已知向量）：不符说明 K 表/移位实现有误，中止
    if (md5hex("") != "d41d8cd98f00b204e9800998ecf8427e" ||
        md5hex("abc") != "900150983cd24fb0d6963f7d28e17f72") {
        fprintf(stderr, "MD5 self-check FAILED\n");
        return 1;
    }

    struct Case { const char *plain; const char *key; };
    const Case cases[] = {
        {"", "MLCLiunx"},
        {"hello world", "MLCLiunx"},
        {"12345678", "MLCLiunx"},
        {"1234567890123456", "MLCLiunx"},
        {"a", "MLCLiunx"},
        {"ab", "MLCLiunx"},
        {"测试中文abc", "MLCLiunx"},
        {"emoji😀test", "MLCLiunx"},
        {"eyJhbGciOiJIUzI1NiJ9.payload.signature-token", "MLCLiunx"},
        {"有 空 格 与,逗号。句号！", "MLCLiunx"},
        {"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "MLCLiunx"},
        {"a", "OtherKey123"},
        {"user@example.com", "OtherKey123"},
    };

    printf("# MLC golden vectors — cipherB64 = desEncrypt(plain, key)，TAB 分隔\n");
    printf("# 由 gen_vectors.cpp 生成（重生成命令见文件头）；密钥派生 = MD5(UTF-8 key) 前 8 原始字节\n");
    printf("# md5(MLCLiunx) hex = %s\n", md5hex("MLCLiunx").c_str());
    printf("# md5(OtherKey123) hex = %s\n", md5hex("OtherKey123").c_str());
    for (const auto &c : cases) {
        // deriveDesKey：hash 是 16 个原始字节，取前 8 字节（不是 hex 字符串！）
        std::string keyBytes = md5raw(c.key).substr(0, 8);
        std::string data = padPkcs7(std::string(c.plain));
        std::string cipher = desEcbEncrypt(data, keyBytes);
        printf("%s\t%s\t%s\n", c.plain, c.key, b64encode(cipher).c_str());
    }
    return 0;
}
