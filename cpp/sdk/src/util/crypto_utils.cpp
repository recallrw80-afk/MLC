#include "util/crypto_utils.h"

#include <QByteArray>
#include <QCryptographicHash>

// Minimal DES implementation — mirrors .NET DESCryptoServiceProvider
// We implement DES-ECB manually to avoid OpenSSL dep
// Key: 8 bytes, padded/truncated from input key

namespace {

QByteArray deriveDesKey(const QString &key) {
    // DES key = 8 bytes. Use first 8 bytes of MD5 hash of input key.
    QByteArray hash = QCryptographicHash::hash(
        key.toUtf8(), QCryptographicHash::Md5);
    return hash.left(8);
}

QByteArray padPkcs7(const QByteArray &data, int blockSize = 8) {
    int padLen = blockSize - (data.size() % blockSize);
    QByteArray padded = data;
    padded.append(QByteArray(padLen, static_cast<char>(padLen)));
    return padded;
}

QByteArray unpadPkcs7(const QByteArray &data) {
    if (data.isEmpty()) return data;
    int padLen = static_cast<unsigned char>(data.at(data.size() - 1));
    if (padLen < 1 || padLen > 8) return data;
    return data.left(data.size() - padLen);
}

// DES block cipher core operations (software fallback)
// DES uses 56-bit key with 16 rounds of Feistel network

// Initial Permutation table
static const int IP[] = {
    58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1,
    59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,
    63, 55, 47, 39, 31, 23, 15, 7
};

// Final Permutation (IP inverse)
static const int FP[] = {
    40, 8, 48, 16, 56, 24, 64, 32,
    39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,
    35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41, 9,  49, 17, 57, 25
};

// Expansion table (32 → 48)
static const int E[] = {
    32, 1,  2,  3,  4,  5,
    4,  5,  6,  7,  8,  9,
    8,  9,  10, 11, 12, 13,
    12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,
    20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,
    28, 29, 30, 31, 32, 1
};

// S-boxes (8 boxes, 4 rows, 16 cols)
static const int S[8][4][16] = {
    { // S1
        {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7},
        {0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8},
        {4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0},
        {15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13}
    },
    { // S2
        {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10},
        {3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5},
        {0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15},
        {13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9}
    },
    { // S3
        {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8},
        {13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1},
        {13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7},
        {1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12}
    },
    { // S4
        {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15},
        {13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9},
        {10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4},
        {3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14}
    },
    { // S5
        {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9},
        {14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6},
        {4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14},
        {11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3}
    },
    { // S6
        {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11},
        {10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8},
        {9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6},
        {4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13}
    },
    { // S7
        {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1},
        {13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6},
        {1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2},
        {6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12}
    },
    { // S8
        {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7},
        {1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2},
        {7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8},
        {2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}
    }
};

// Permutation P (32 → 32)
static const int P[] = {
    16, 7,  20, 21, 29, 12, 28, 17,
    1,  15, 23, 26, 5,  18, 31, 10,
    2,  8,  24, 14, 32, 27, 3,  9,
    19, 13, 30, 6,  22, 11, 4,  25
};

// PC-1 (key schedule permutation 1)
static const int PC1[] = {
    57, 49, 41, 33, 25, 17, 9,
    1,  58, 50, 42, 34, 26, 18,
    10, 2,  59, 51, 43, 35, 27,
    19, 11, 3,  60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,
    7,  62, 54, 46, 38, 30, 22,
    14, 6,  61, 53, 45, 37, 29,
    21, 13, 5,  28, 20, 12, 4
};

// PC-2 (key schedule permutation 2)
static const int PC2[] = {
    14, 17, 11, 24, 1,  5,  3,  28,
    15, 6,  21, 10, 23, 19, 12, 4,
    26, 8,  16, 7,  27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40,
    51, 45, 33, 48, 44, 49, 39, 56,
    34, 53, 46, 42, 50, 36, 29, 32
};

// Key shift schedule
static const int SHIFTS[] = {
    1, 1, 2, 2, 2, 2, 2, 2,
    1, 2, 2, 2, 2, 2, 2, 1
};

static quint64 permute(quint64 input, const int *table, int n, quint64 mask) {
    quint64 result = 0;
    for (int i = 0; i < n; ++i) {
        int bit = table[i] - 1;
        if (input & (1ULL << bit))
            result |= (1ULL << i);
    }
    return result & mask;
}

static quint64 permute64to56(quint64 key) {
    quint64 result = 0;
    for (int i = 0; i < 56; ++i) {
        int bit = PC1[i] - 1;
        if (key & (1ULL << (63 - bit)))
            result |= (1ULL << (55 - i));
    }
    return result;
}

static quint64 permute56to48(quint64 key) {
    quint64 result = 0;
    for (int i = 0; i < 48; ++i) {
        int bit = PC2[i] - 1;
        if (key & (1ULL << (55 - bit)))
            result |= (1ULL << (47 - i));
    }
    return result;
}

static quint64 permuteIP(quint64 block) {
    return permute(block, IP, 64, 0xFFFFFFFFFFFFFFFFULL);
}

static quint64 permuteFP(quint64 block) {
    return permute(block, FP, 64, 0xFFFFFFFFFFFFFFFFULL);
}

static quint32 f(quint32 r, quint64 subkey48) {
    // Expansion
    quint64 expanded = 0;
    for (int i = 0; i < 48; ++i) {
        int bit = E[i] - 1;
        if (r & (1ULL << (31 - bit)))
            expanded |= (1ULL << (47 - i));
    }

    // XOR with subkey
    expanded ^= subkey48;

    // S-box substitution
    quint32 result = 0;
    for (int box = 0; box < 8; ++box) {
        int shift = 42 - box * 6;
        quint8 sixBits = (expanded >> shift) & 0x3F;
        int row = ((sixBits >> 4) & 0x02) | (sixBits & 0x01);
        int col = (sixBits >> 1) & 0x0F;
        result = (result << 4) | S[box][row][col];
    }

    // Permutation P
    quint32 permuted = 0;
    for (int i = 0; i < 32; ++i) {
        int bit = P[i] - 1;
        if (result & (1ULL << (31 - bit)))
            permuted |= (1ULL << (31 - i));
    }
    return permuted;
}

static QByteArray desEncryptBlock(QByteArray key) {
    // Not used directly — encryptBlock/decryptBlock operate on 64-bit blocks
    Q_UNUSED(key);
    return {};
}

// Encrypt 8-byte block with DES key (8 bytes → 56-bit + parity)
static quint64 desEncrypt64(quint64 block, const QByteArray &keyBytes) {
    // Load key
    quint64 key = 0;
    for (int i = 0; i < 8; ++i)
        key = (key << 8) | static_cast<unsigned char>(keyBytes.at(i));

    // Generate subkeys
    quint64 pc1 = permute64to56(key);
    quint32 c = (pc1 >> 28) & 0x0FFFFFFF;
    quint32 d = pc1 & 0x0FFFFFFF;

    quint64 subkeys[16];
    for (int round = 0; round < 16; ++round) {
        c = ((c << SHIFTS[round]) | (c >> (28 - SHIFTS[round]))) & 0x0FFFFFFF;
        d = ((d << SHIFTS[round]) | (d >> (28 - SHIFTS[round]))) & 0x0FFFFFFF;
        quint64 cd = (static_cast<quint64>(c) << 28) | d;
        subkeys[round] = permute56to48(cd);
    }

    // IP
    block = permuteIP(block);

    // 16 rounds
    quint32 left = (block >> 32) & 0xFFFFFFFF;
    quint32 right = block & 0xFFFFFFFF;

    for (int round = 0; round < 16; ++round) {
        quint32 temp = left;
        left = right;
        right = temp ^ f(right, subkeys[round]);
    }

    // Swap and FP
    block = (static_cast<quint64>(right) << 32) | left;
    block = permuteFP(block);

    return block;
}

static quint64 desDecrypt64(quint64 block, const QByteArray &keyBytes) {
    quint64 key = 0;
    for (int i = 0; i < 8; ++i)
        key = (key << 8) | static_cast<unsigned char>(keyBytes.at(i));

    quint64 pc1 = permute64to56(key);
    quint32 c = (pc1 >> 28) & 0x0FFFFFFF;
    quint32 d = pc1 & 0x0FFFFFFF;

    quint64 subkeys[16];
    for (int round = 0; round < 16; ++round) {
        c = ((c << SHIFTS[round]) | (c >> (28 - SHIFTS[round]))) & 0x0FFFFFFF;
        d = ((d << SHIFTS[round]) | (d >> (28 - SHIFTS[round]))) & 0x0FFFFFFF;
        quint64 cd = (static_cast<quint64>(c) << 28) | d;
        subkeys[round] = permute56to48(cd);
    }

    block = permuteIP(block);
    quint32 left = (block >> 32) & 0xFFFFFFFF;
    quint32 right = block & 0xFFFFFFFF;

    for (int round = 15; round >= 0; --round) {
        quint32 temp = left;
        left = right;
        right = temp ^ f(right, subkeys[round]);
    }

    block = (static_cast<quint64>(right) << 32) | left;
    block = permuteFP(block);
    return block;
}

QByteArray desEcb(const QByteArray &data, const QByteArray &keyBytes, bool encrypt) {
    QByteArray result;
    for (int i = 0; i < data.size(); i += 8) {
        QByteArray block = data.mid(i, 8);
        // Pad block to 8 bytes if needed
        while (block.size() < 8) block.append('\0');

        quint64 blockVal = 0;
        for (int j = 0; j < 8; ++j)
            blockVal = (blockVal << 8) | static_cast<unsigned char>(block.at(j));

        blockVal = encrypt ? desEncrypt64(blockVal, keyBytes)
                            : desDecrypt64(blockVal, keyBytes);

        for (int j = 7; j >= 0; --j)
            result.append(static_cast<char>((blockVal >> (j * 8)) & 0xFF));
    }
    return result;
}

} // anonymous namespace

namespace CryptoUtils {

QString desEncrypt(const QString &plainText, const QString &key) {
    QByteArray keyBytes = deriveDesKey(key);
    QByteArray data = plainText.toUtf8();
    data = padPkcs7(data, 8);

    QByteArray encrypted = desEcb(data, keyBytes, true);
    return encrypted.toBase64();
}

QString desDecrypt(const QString &cipherB64, const QString &key) {
    QByteArray keyBytes = deriveDesKey(key);
    QByteArray data = QByteArray::fromBase64(cipherB64.toUtf8());
    if (data.isEmpty()) return {};

    QByteArray decrypted = desEcb(data, keyBytes, false);
    decrypted = unpadPkcs7(decrypted);
    return QString::fromUtf8(decrypted);
}

} // namespace CryptoUtils
