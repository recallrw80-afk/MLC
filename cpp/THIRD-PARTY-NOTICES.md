# Third-Party Notices / 第三方声明

MLC 的发布包（`mlc-linux-*.tar.gz`）为便于零依赖运行，收编了以下第三方组件的运行库。
各组件版权归其作者所有，按各自许可证使用。完整许可证文本见各项目官网。

## Qt 6（Core / Network）— LGPL-3.0

- https://www.qt.io/ — https://www.gnu.org/licenses/lgpl-3.0.txt
- 以动态链接方式使用（发布包内 `lib/` 下的 `libQt6*.so`），允许用户替换/重链这些库
- Qt © The Qt Company Ltd.

## OpenSSL（libssl / libcrypto）— Apache-2.0

- https://www.openssl.org/ — https://www.apache.org/licenses/LICENSE-2.0.txt
- 由 Qt 的 TLS 插件在运行时加载，用于 HTTPS

## ICU（libicui18n / libicuuc / libicudata）— Unicode License

- https://icu.unicode.org/ — https://www.unicode.org/license.txt

## zlib — Zlib License

- https://zlib.net/ — https://zlib.net/zlib_license.html

## zstd — BSD-3-Clause

- https://facebook.github.io/zstd/ — https://github.com/facebook/zstd/blob/dev/LICENSE

## Brotli — MIT

- https://github.com/google/brotli — https://github.com/google/brotli/blob/master/LICENSE

## GLib — LGPL-2.1

- https://docs.gtk.org/glib/ — https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt

## PCRE2 — BSD-2-Clause

- https://pcre2project.github.io/pcre2/ — https://www.pcre2.org/licence.txt

## MIT Kerberos（libgssapi_krb5 等）— MIT-style

- https://web.mit.edu/kerberos/ — https://web.mit.edu/kerberos/krb5-1.21/doc/mitK5license.html

## 构建期依赖（不进入发布包）

- nlohmann-json — MIT（https://github.com/nlohmann/json）
- authlib-injector（运行时按需下载，不开源于本仓库分发）— MIT（https://github.com/yushijinhun/authlib-injector）
- Lucide 图标（GUI 资源）— ISC（https://lucide.dev/license）
