#include "util/file_utils.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDataStream>
#include <zlib.h>
#include <lzma.h>
#ifndef Q_OS_WIN
#include <iconv.h>
#endif

namespace FileUtils {

QString assetPathFromHash(const QString &hash) {
    return hash.left(2) + "/" + hash;
}

bool verifySha1(const QString &filePath, const QString &expectedHash) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QCryptographicHash hasher(QCryptographicHash::Sha1);
    hasher.addData(&file);
    file.close();

    QString actual = hasher.result().toHex().toLower();
    return actual == expectedHash.toLower();
}

QString mavenNameToPath(const QString &name) {
    QString n = name;
    QString ext = QStringLiteral("jar");
    int at = n.indexOf('@');
    if (at != -1) {
        ext = n.mid(at + 1);
        n = n.left(at);
    }
    const auto parts = n.split(':');
    if (parts.size() < 3) return {};
    QString file = parts[1] + '-' + parts[2];
    if (parts.size() > 3) file += '-' + parts[3];
    return QString("%1/%2/%3/%4.%5")
        .arg(QString(parts[0]).replace('.', '/'), parts[1], parts[2], file, ext);
}

static bool isNativeFile(const QString &name)
{
    if (name.startsWith("META-INF/")) return false;
    if (name.endsWith('/')) return false;
    return name.endsWith(".so") || name.endsWith(".dll") ||
           name.endsWith(".dylib") || name.endsWith(".jnilib");
}

static quint16 readU16(QDataStream &s)
{
    quint16 v;
    s.setByteOrder(QDataStream::LittleEndian);
    s >> v;
    return v;
}

static quint32 readU32(QDataStream &s)
{
    quint32 v;
    s.setByteOrder(QDataStream::LittleEndian);
    s >> v;
    return v;
}

/// Decompress raw DEFLATE data (no zlib/gzip header) using zlib
static QByteArray inflateRaw(const QByteArray &compressed, quint32 expectedSize)
{
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
    strm.avail_in = compressed.size();

    // -MAX_WBITS = raw deflate (no header)
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
        return {};

    QByteArray result;
    result.resize(expectedSize > 0 ? expectedSize : compressed.size() * 4);
    if (result.isEmpty()) result.resize(4096);

    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(result.data() + strm.total_out);
        strm.avail_out = result.size() - strm.total_out;

        if (strm.avail_out == 0) {
            // zip bomb 防护：单条目解压上限 512MB（恶意高压缩比条目会耗尽内存）
            if (result.size() >= 512 * 1024 * 1024) {
                inflateEnd(&strm);
                return {};
            }
            result.resize(result.size() * 2);
            strm.avail_out = result.size() - strm.total_out;
            strm.next_out = reinterpret_cast<Bytef*>(result.data() + strm.total_out);
        }

        ret = inflate(&strm, Z_NO_FLUSH);
        // Z_BUF_ERROR：输入耗尽但流未结束（归档截断/损坏）——不处理会死循环
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR ||
            ret == Z_BUF_ERROR || ret == Z_NEED_DICT) {
            inflateEnd(&strm);
            return {};
        }
    } while (ret != Z_STREAM_END);

    result.resize(strm.total_out);
    inflateEnd(&strm);
    return result;
}

// ---- 通用 ZIP 读取（基于中央目录，不猜数据区/描述符） ----

// zip 条目名解码：UTF-8 标记位 → UTF-8；无标记时按 GBK
// （中文 Windows 打包工具——PCL 导出的整合包——用 GBK 且不打 UTF-8 标记；
//  Linux 的 fromLocal8Bit 是 UTF-8，直接解会得到乱码路径）
static QString decodeZipName(const QByteArray &raw, bool utf8Flag) {
    if (utf8Flag) return QString::fromUtf8(raw);
#ifdef Q_OS_WIN
    return QString::fromLocal8Bit(raw);  // 中文 Windows 本地编码即 GBK
#else
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == (iconv_t)-1) return QString::fromLocal8Bit(raw);
    QByteArray out(raw.size() * 2 + 8, Qt::Uninitialized);
    char *in = const_cast<char*>(raw.constData());
    size_t inLeft = raw.size();
    char *outp = out.data();
    size_t outLeft = out.size();
    size_t n = iconv(cd, &in, &inLeft, &outp, &outLeft);
    iconv_close(cd);
    if (n == (size_t)-1) return QString::fromLocal8Bit(raw);
    out.resize(out.size() - outLeft);
    return QString::fromUtf8(out);
#endif
}

struct ZipEntryInfo {
    QString name;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint16 compression = 0;
    quint32 localHeaderOffset = 0;  // local header 偏移（解压时定位数据区）
};

// 从文件尾部定位 EOCD（zip 规范必有），返回中央目录偏移；找不到返回 -1
static qint64 findEocdOffset(QFile &file, quint16 *countOut)
{
    qint64 fileSize = file.size();
    if (fileSize < 22) return -1;

    // EOCD 固定 22 字节 + 最长 65535 字节注释，从尾部回扫签名
    qint64 scanLen = qMin(fileSize, (qint64)(22 + 65535));
    file.seek(fileSize - scanLen);
    QByteArray tail = file.read(scanLen);

    for (int i = tail.size() - 22; i >= 0; --i) {
        quint32 sig = quint32(quint8(tail[i])) | (quint32(quint8(tail[i+1])) << 8) |
                      (quint32(quint8(tail[i+2])) << 16) | (quint32(quint8(tail[i+3])) << 24);
        if (sig == 0x06054b50) {
            if (countOut)
                *countOut = quint16(quint8(tail[i+10])) | (quint16(quint8(tail[i+11])) << 8);
            return qint64(quint32(quint8(tail[i+16])) | (quint32(quint8(tail[i+17])) << 8) |
                          (quint32(quint8(tail[i+18])) << 16) | (quint32(quint8(tail[i+19])) << 24));
        }
    }
    return -1;
}

// 从文件尾部的 EOCD 定位并读取中央目录（zip 规范必有；无 EOCD 则不是有效 zip）
static QList<ZipEntryInfo> readZipCentralDirectory(QFile &file)
{
    QList<ZipEntryInfo> entries;
    quint16 cdCount = 0;
    qint64 cdOffset = findEocdOffset(file, &cdCount);
    if (cdOffset < 0 || !file.seek(cdOffset)) return entries;

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    for (int i = 0; i < cdCount; ++i) {
        if (readU32(stream) != 0x02014b50) break;

        readU16(stream);  // version made by
        readU16(stream);  // version needed
        quint16 flags = readU16(stream);
        ZipEntryInfo e;
        e.compression = readU16(stream);
        readU16(stream); readU16(stream);  // mod time/date
        readU32(stream);  // crc32
        e.compressedSize = readU32(stream);
        e.uncompressedSize = readU32(stream);
        quint16 nameLen = readU16(stream);
        quint16 extraLen = readU16(stream);
        quint16 commentLen = readU16(stream);
        readU16(stream);  // disk number start
        readU16(stream);  // internal attrs
        readU32(stream);  // external attrs
        e.localHeaderOffset = readU32(stream);

        QByteArray nameBytes(nameLen, Qt::Uninitialized);
        if (stream.readRawData(nameBytes.data(), nameLen) != (qint64)nameLen) break;
        e.name = decodeZipName(nameBytes, flags & 0x800);

        if (extraLen > 0) file.seek(file.pos() + extraLen);
        if (commentLen > 0) file.seek(file.pos() + commentLen);
        entries.append(e);
    }
    return entries;
}

// 解析 local header，返回数据区偏移（local 与 central 的 name/extra 长度可能不同，失败返回 -1）
static qint64 localDataOffset(QFile &file, quint32 localHeaderOffset)
{
    if (!file.seek(localHeaderOffset)) return -1;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    if (readU32(stream) != 0x04034b50) return -1;
    readU16(stream);  // version needed
    readU16(stream);  // flags
    readU16(stream);  // compression
    readU16(stream); readU16(stream);  // time/date
    readU32(stream);  // crc
    readU32(stream);  // compressed size
    readU32(stream);  // uncompressed size
    quint16 nameLen = readU16(stream);
    quint16 extraLen = readU16(stream);
    return localHeaderOffset + 30 + nameLen + extraLen;
}

// 读单个条目的解压内容（尺寸取自中央目录，无需处理 data descriptor）
static QByteArray inflateEntry(QFile &file, const ZipEntryInfo &e)
{
    if (e.compressedSize == 0xFFFFFFFF) return {};  // ZIP64 未支持
    if (e.uncompressedSize > 512 * 1024 * 1024) return {};  // zip bomb 防护：单条目上限 512MB
    qint64 offset = localDataOffset(file, e.localHeaderOffset);
    if (offset < 0 || !file.seek(offset)) return {};

    QByteArray data(e.compressedSize, Qt::Uninitialized);
    if (file.read(data.data(), e.compressedSize) != (qint64)e.compressedSize)
        return {};

    if (e.compression == 0) return data;      // STORED
    if (e.compression == 8)                   // DEFLATE
        return inflateRaw(data, e.uncompressedSize);
    return {};  // 不支持的压缩方式
}

// 大条目（>512MB）流式解压：不全量进内存（zip bomb 防护的 512MB 上限只挡内存路径，
// 外壳包内嵌的整 GB 整合包是合法场景——嵌套整合包检测/导入都依赖这里）
static bool inflateEntryToFileStreaming(QFile &file, const ZipEntryInfo &e, const QString &outPath)
{
    if (e.compressedSize == 0xFFFFFFFF) return false;
    qint64 offset = localDataOffset(file, e.localHeaderOffset);
    if (offset < 0 || !file.seek(offset)) return false;

    QDir().mkpath(QFileInfo(outPath).absolutePath());
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly)) return false;

    const qint64 CHUNK = 1024 * 1024;
    if (e.compression == 0) {  // STORED：分块拷贝
        QByteArray buf(CHUNK, Qt::Uninitialized);
        quint32 left = e.compressedSize;
        while (left > 0) {
            qint64 n = file.read(buf.data(), qMin<qint64>(CHUNK, left));
            if (n <= 0) return false;
            if (out.write(buf.data(), n) != n) return false;
            left -= quint32(n);
        }
        out.close();
        return true;
    }
    if (e.compression != 8) return false;  // 不支持其他压缩方式

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return false;

    QByteArray inBuf(CHUNK, Qt::Uninitialized);
    QByteArray outBuf(CHUNK, Qt::Uninitialized);
    int ret = Z_OK;
    bool ok = false;
    while (ret != Z_STREAM_END) {
        if (strm.avail_in == 0) {
            qint64 n = file.read(inBuf.data(), CHUNK);
            if (n <= 0) { inflateEnd(&strm); return false; }
            strm.next_in = reinterpret_cast<Bytef *>(inBuf.data());
            strm.avail_in = static_cast<uInt>(n);
        }
        strm.next_out = reinterpret_cast<Bytef *>(outBuf.data());
        strm.avail_out = CHUNK;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR ||
            ret == Z_NEED_DICT || ret == Z_BUF_ERROR) {
            inflateEnd(&strm);
            return false;
        }
        auto produced = CHUNK - strm.avail_out;
        if (produced > 0 && out.write(outBuf.data(), produced) != qint64(produced)) {
            inflateEnd(&strm);
            return false;
        }
    }
    ok = true;
    inflateEnd(&strm);
    out.close();
    return ok;
}

static bool inflateEntryToFile(QFile &file, const ZipEntryInfo &e, const QString &outPath)
{
    if (e.uncompressedSize > 512 * 1024 * 1024)
        return inflateEntryToFileStreaming(file, e, outPath);
    QByteArray content = inflateEntry(file, e);
    if (content.isEmpty() && e.uncompressedSize > 0) return false;

    QDir().mkpath(QFileInfo(outPath).absolutePath());
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.write(content);
    out.close();
    return true;
}

QStringList listZipEntries(const QString &zipPath)
{
    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QStringList names;
    for (const auto &e : readZipCentralDirectory(file))
        names.append(e.name);
    return names;
}

bool extractZip(const QString &zipPath, const QString &destDir, QString *errorOut)
{
    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = "Cannot open: " + zipPath;
        return false;
    }
    auto entries = readZipCentralDirectory(file);
    if (entries.isEmpty()) {
        // 区分"不是 zip"和"合法空包"：EOCD 存在且 cdCount=0 → 空包，视为成功
        quint16 cdCount = 0;
        if (findEocdOffset(file, &cdCount) >= 0 && cdCount == 0)
            return true;
        if (errorOut) *errorOut = "Not a valid zip archive: " + zipPath;
        return false;
    }

    int failed = 0;
    for (const auto &e : entries) {
        if (e.name.endsWith('/')) continue;  // 目录条目
        // zip-slip 防护：拒绝绝对路径和 .. 穿越（恶意整合包可任意写文件）。
        // 计为失败——否则"只含恶意条目"的包会被当成解压成功，与头文件
        // "全部条目成功才返回 true" 的契约相悖。
        if (e.name.startsWith('/') || e.name.split('/').contains("..")) {
            qWarning() << "extractZip: reject unsafe entry" << e.name;
            failed++;
            continue;
        }
        if (!inflateEntryToFile(file, e, destDir + "/" + e.name)) {
            qWarning() << "extractZip: failed entry" << e.name;
            failed++;
        }
    }
    if (errorOut && failed > 0)
        *errorOut = QString("%1/%2 entries failed").arg(failed).arg(entries.size());
    return failed == 0;
}

bool extractZipEntry(const QString &zipPath, const QString &entryName, const QString &destPath)
{
    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    for (const auto &e : readZipCentralDirectory(file)) {
        if (e.name == entryName)
            return inflateEntryToFile(file, e, destPath);
    }
    return false;
}

QByteArray readZipEntry(const QString &zipPath, const QString &entryName)
{
    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    for (const auto &e : readZipCentralDirectory(file)) {
        if (e.name == entryName)
            return inflateEntry(file, e);
    }
    return {};
}

QStringList extractNativesJar(const QString &jarPath, const QString &destDir)
{
    QStringList extracted;
    QFile file(jarPath);
    if (!file.open(QIODevice::ReadOnly)) return extracted;
    QDir().mkpath(destDir);

    for (const auto &e : readZipCentralDirectory(file)) {
        if (!isNativeFile(e.name)) continue;
        // natives 只取文件名（拍平到 destDir 根）
        if (inflateEntryToFile(file, e, destDir + "/" + QFileInfo(e.name).fileName()))
            extracted.append(destDir + "/" + QFileInfo(e.name).fileName());
    }
    return extracted;
}

bool copyDir(const QString &src, const QString &dst)
{
    QDir srcDir(src);
    if (!srcDir.exists()) return false;
    QDir().mkpath(dst);

    bool ok = true;
    const auto entries = srcDir.entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const auto &entry : entries) {
        QString target = dst + "/" + entry.fileName();
        if (entry.isDir()) {
            if (!copyDir(entry.absoluteFilePath(), target)) ok = false;
        } else {
            QFile::remove(target);  // 覆盖已有文件
            if (!QFile::copy(entry.absoluteFilePath(), target)) {
                qWarning() << "copyDir: failed" << entry.absoluteFilePath();
                ok = false;
            }
        }
    }
    return ok;
}

// ---- tar.gz / tar.xz 解压（Adoptium JRE 包、自更新包用） ----

// xz 解压（liblzma stream_buffer_decode；输出缓冲不足时倍增重试）
static QByteArray unxz(const QByteArray &compressed)
{
    uint64_t memlimit = UINT64_MAX;
    size_t inPos = 0;
    size_t outCap = qMax<size_t>(size_t(compressed.size()) * 4, size_t(1) << 20);
    for (int attempt = 0; attempt < 8; ++attempt) {
        QByteArray out(outCap, Qt::Uninitialized);
        size_t outPos = 0;
        lzma_ret ret = lzma_stream_buffer_decode(&memlimit, 0, nullptr,
            reinterpret_cast<const uint8_t *>(compressed.constData()), &inPos,
            size_t(compressed.size()),
            reinterpret_cast<uint8_t *>(out.data()), &outPos, outCap);
        if (ret == LZMA_OK) {
            out.resize(outPos);
            return out;
        }
        if (ret == LZMA_BUF_ERROR) {  // 输出缓冲不够：放大重来
            inPos = 0;
            outCap *= 2;
            continue;
        }
        return {};
    }
    return {};
}

// gzip 解压（inflateInit2 带 gzip 头，windowBits = 15+16）
static QByteArray gunzip(const QByteArray &compressed)
{
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
    strm.avail_in = compressed.size();

    if (inflateInit2(&strm, 15 + 16) != Z_OK)
        return {};

    QByteArray result(compressed.size() * 3 + 4096, Qt::Uninitialized);
    int ret;
    do {
        if (strm.total_out >= (size_t)result.size())
            result.resize(result.size() * 2);
        strm.next_out = reinterpret_cast<Bytef*>(result.data() + strm.total_out);
        strm.avail_out = result.size() - strm.total_out;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR ||
            ret == Z_BUF_ERROR || ret == Z_NEED_DICT) {
            inflateEnd(&strm);
            return {};
        }
    } while (ret != Z_STREAM_END);

    result.resize(strm.total_out);
    inflateEnd(&strm);
    return result;
}

// tar 头的 size 字段（八进制 ASCII）
static qint64 tarOctal(const char *p, int len)
{
    qint64 v = 0;
    for (int i = 0; i < len && p[i] >= '0' && p[i] <= '7'; ++i)
        v = v * 8 + (p[i] - '0');
    return v;
}

bool extractTarGz(const QString &tgzPath, const QString &destDir, QString *errorOut)
{
    QFile f(tgzPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = "Cannot open: " + tgzPath;
        return false;
    }
    QByteArray blob = f.readAll();
    f.close();
    // 按魔数选解码器：xz（FD 37 7A 58 5A 00）或 gzip（1F 8B，兼容旧包）
    QByteArray tar;
    if (blob.startsWith("\xFD" "7zXZ"))
        tar = unxz(blob);
    else
        tar = gunzip(blob);
    if (tar.isEmpty()) {
        if (errorOut) *errorOut = "decompress failed: " + tgzPath;
        return false;
    }

    int failed = 0;
    qint64 pos = 0;
    QString longName;
    while (pos + 512 <= tar.size()) {
        const char *h = tar.constData() + pos;
        if (h[0] == '\0') break;  // 空块结束

        QString name = longName.isEmpty()
            ? QString::fromUtf8(QByteArray(h, strnlen(h, 100)))
            : longName;
        longName.clear();
        qint64 size = tarOctal(h + 124, 12);
        char type = h[156];
        pos += 512;

        if (type == 'L') {
            // GNU 长文件名：本块内容是名字（含结尾 NUL），下一个头才是真实条目
            if (pos + size > tar.size()) break;
            longName = QString::fromUtf8(QByteArray(tar.constData() + pos, size));
            while (longName.endsWith('\0')) longName.chop(1);
            pos += (size + 511) / 512 * 512;
            continue;
        }
        // zip-slip 防护：拒绝绝对路径和 .. 穿越
        if (name.startsWith('/') || name.split('/').contains("..")) {
            pos += (size + 511) / 512 * 512;
            continue;
        }
        if (type == '5' || name.endsWith('/')) {
            QDir().mkpath(destDir + "/" + name);
        } else if (type == '0' || type == '\0') {
            QString outPath = destDir + "/" + name;
            QDir().mkpath(QFileInfo(outPath).absolutePath());
            QFile out(outPath);
            if (!out.open(QIODevice::WriteOnly) ||
                pos + size > tar.size() ||
                out.write(tar.constData() + pos, size) != size) {
                failed++;
            } else {
                out.close();
                // 保留可执行位（JRE 的 bin/ 工具需要）
                qint64 mode = tarOctal(h + 100, 8);
                if (mode & 0100)
                    QFile::setPermissions(outPath, QFile::permissions(outPath) |
                        QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
            }
        }
        pos += (size + 511) / 512 * 512;
    }

    if (errorOut && failed > 0)
        *errorOut = QString("%1 entries failed").arg(failed);
    return failed == 0;
}

bool removeDirContents(const QString &path) {
    QDir d(path);
    bool ok = true;
    const auto entries = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto &entry : entries) {
        if (entry.isSymLink() || entry.isFile())
            ok = QFile::remove(entry.absoluteFilePath()) && ok;
        else if (entry.isDir())
            ok = QDir(entry.absoluteFilePath()).removeRecursively() && ok;
    }
    return ok;
}

bool removeTree(const QString &path) {
    if (!QDir(path).exists()) return true;
    bool ok = removeDirContents(path);
    return QDir().rmdir(path) && ok;
}

} // namespace FileUtils
