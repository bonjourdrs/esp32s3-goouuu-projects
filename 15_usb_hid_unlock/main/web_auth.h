/*
 * web_auth —— 配置网页的登录密码，存在 NVS 里。
 *
 * 出厂默认 0000，且标记为“从未修改”；第一次登录后必须改密，
 * 改完之前除了改密接口，其它配置接口一律拒绝。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define WEB_PWD_DEFAULT     "0000"
#define WEB_PWD_MIN_LEN     4
#define WEB_PWD_MAX_LEN     16

/* 需在 pin_store_init() 之后调用（NVS 已初始化） */
esp_err_t web_auth_init(void);

/* 是否还停留在出厂密码、必须先改密 */
bool web_auth_must_change(void);

bool web_auth_check(const char *pwd);

/* 4–16 位纯数字；成功后 must_change 变为 false */
esp_err_t web_auth_set(const char *pwd);

/* 恢复出厂：密码回到 0000，并重新要求首次改密 */
void web_auth_reset(void);
