/* 自动生成请对照 rust/crates/mlccore-ffi/src/lib.rs（阶段 3 C ABI）
 * 头文件守卫与 cbindgen.toml 保持一致：MLC_FFI_H
 * 约定：
 *   - 入参为 NUL 结尾 UTF-8 C 字符串
 *   - 出参字符串/JSON 用 mlc_string_free 释放
 *   - 列表与结构体出参为 JSON 文本
 *   - 异步 API 在内部 tokio runtime 上阻塞至完成
 */
#pragma once
#ifndef MLC_FFI_H
#define MLC_FFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- 基础 ---- */

/* 静态版本串，禁止 free */
const char *mlc_version(void);
void mlc_string_free(char *s);

/* ---- 实例 ---- */

/* JSON 数组字符串：显示名列表 */
char *mlc_list_versions(void);
/* JSON 数组字符串：已安装 MC 版本 id */
char *mlc_list_mc_versions(void);
bool mlc_remove_instance(const char *name);
/* JSON 对象：{dirName,path,version,modCount}；dirName 空 = 不存在 */
char *mlc_instance_info(const char *display_name);
/* JSON 数组：[{fileName,size,enabled}] */
char *mlc_list_mods(const char *display_name);
bool mlc_set_mod_enabled(const char *display_name, const char *file_name, bool enabled);
bool mlc_delete_mod(const char *display_name, const char *file_name);

/* ---- 配置 / Java ---- */

/* JSON：{version,commit,gameFolder,gameFolderSet,players[],selectedPlayer} */
char *mlc_get_config(void);
/* JSON 数组：[{pathJava,pathFolder,display,majorVersion,is64Bit,isUserImport}] */
char *mlc_list_javas(void);

/* ---- 玩家 ---- */

/* JSON 数组：[{uuid,name,avatar,skinType}] */
char *mlc_list_players(void);
/* JSON 对象新条目 */
char *mlc_add_player(const char *name, const char *avatar, const char *skin_type,
                     const char *custom_uuid);
bool mlc_update_player(const char *uuid, const char *name, const char *avatar,
                       const char *skin_type, const char *new_uuid);
bool mlc_remove_player(const char *uuid);
bool mlc_select_player(const char *uuid);
/* JSON：{name,uuid,accessToken,loginType,clientToken}；skin_type: slim|wide|default */
char *mlc_create_offline_login(const char *name, const char *skin_type);

/* ---- Authlib / CF key ---- */

void mlc_logout_authlib(void);
/* JSON：{loggedIn,server,name,uuid} */
char *mlc_current_authlib_login(void);
/* 空串 = 清除 */
void mlc_set_cf_api_key(const char *key);
/* "user" | "embedded" | "none"（禁止回显 key 本体） */
char *mlc_cf_api_key_source(void);

/* ---- Mod 平台（CF/Modrinth）----
 * platform: 0=CurseForge 1=Modrinth
 * rtype: 0=Mod 1=ModPack 2=ResourcePack 3=Shader 4=DataPack
 */

/* JSON 数组：[{id,name,summary,author,iconUrl,downloadCount,versions,...}] */
char *mlc_mod_search(int platform, int rtype, const char *query,
                     unsigned int page, unsigned int page_size);
/* JSON 对象；失败 {"error":"..."} */
char *mlc_mod_details(int platform, const char *mod_id);
/* JSON 数组：[{id,displayName,fileName,downloadUrl,gameVersions,loaders,fileSize,...}] */
char *mlc_mod_files(int platform, const char *mod_id);
bool mlc_mod_download(int platform, const char *mod_id, const char *file_id,
                      const char *dest_path, mlc_progress_callback on_progress);

/* ---- 游戏目录 ---- */

bool mlc_set_folder(const char *path);
char *mlc_mc_folder(void);

/* ---- 回调类型 ---- */

typedef void (*mlc_progress_callback)(const char *step, int percent);
typedef void (*mlc_import_complete_callback)(bool ok, const char *msg,
                                             const char *data_json);
typedef void (*mlc_log_callback)(const char *line);
typedef void (*mlc_exit_callback)(int exit_code);
typedef void (*mlc_authlib_login_callback)(bool ok, const char *error_or_name,
                                           const char *info_json);

/* ---- 异步（阻塞至完成）---- */

bool mlc_install_version(const char *version_id);
void mlc_login_authlib(const char *server, const char *username,
                       const char *password, mlc_authlib_login_callback callback);
void mlc_import_modpack(const char *file_path, const char *instance_name,
                        const char *target_instance,
                        mlc_progress_callback on_progress,
                        mlc_import_complete_callback on_complete);
bool mlc_launch_version(const char *version_id, mlc_log_callback on_log,
                        mlc_exit_callback on_exit);
bool mlc_install_server(const char *version_id, const char *loader_type,
                        const char *loader_version,
                        mlc_progress_callback on_progress);
int mlc_start_server(const char *server_id, mlc_log_callback on_log,
                     mlc_exit_callback on_exit);

#ifdef __cplusplus
}
#endif

#endif /* MLC_FFI_H */
