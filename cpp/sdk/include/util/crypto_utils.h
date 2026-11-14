#include "core/mlccore_export.h"
#ifndef MLC_CRYPTO_UTILS_H
#define MLC_CRYPTO_UTILS_H

#include <QString>
#include <QByteArray>

namespace CryptoUtils {

/// DES encrypt plaintext with key. Returns base64-encoded ciphertext.
/// Mirrors CryptographyUtils.DesEncrypt from Windows PCL.
MLCCORE_EXPORT QString desEncrypt(const QString &plainText, const QString &key);

/// DES decrypt base64-encoded ciphertext with key. Returns plaintext.
/// Mirrors CryptographyUtils.DesDecrypt from Windows PCL.
MLCCORE_EXPORT QString desDecrypt(const QString &cipherB64, const QString &key);

/// Generate the MLC encryption key: "MLC" + identify string
inline QString pclEncryptKey() { return QString("MLC") + "Liunx"; }

/// Encrypt with PCL standard key
inline QString pclEncrypt(const QString &plainText) {
    return desEncrypt(plainText, pclEncryptKey());
}

/// Decrypt with PCL standard key
inline QString pclDecrypt(const QString &cipherB64) {
    return desDecrypt(cipherB64, pclEncryptKey());
}

} // namespace CryptoUtils

#endif // MLC_CRYPTO_UTILS_H
