/*
 * pin_store —— 解锁凭据存储（电脑 PIN / 电脑密码），存在 NVS 里。
 *
 * 规则：
 *   - 电脑 PIN：4–16 位纯数字
 *   - 电脑密码：1–32 位 ASCII 可打印字符（0x20–0x7E）
 *   - 两者可以同时保存，但同一时刻只有一种作为解锁方式
 *   - 首次使用两者都为空，必须先到配置网页设置
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define PIN_MIN_LEN     4
#define PIN_MAX_LEN     16
#define PWD_MIN_LEN     1
#define PWD_MAX_LEN     32

typedef enum {
    UNLOCK_MODE_NONE = 0,       /* 还没设置，短按不动作 */
    UNLOCK_MODE_PIN = 1,        /* 用电脑 PIN（纯数字）解锁 */
    UNLOCK_MODE_PASSWORD = 2,   /* 用电脑密码（字母/符号）解锁 */
} unlock_mode_t;

/* 初始化 NVS 并读出已保存的凭据（没有就保持为空） */
esp_err_t pin_store_init(void);

unlock_mode_t pin_store_get_mode(void);
bool pin_store_has_pin(void);
bool pin_store_has_password(void);

/* 拷贝当前 PIN / 密码（含结尾 '\0'） */
esp_err_t pin_store_get_pin(char *out, size_t out_len);
esp_err_t pin_store_get_password(char *out, size_t out_len);

bool pin_store_pin_valid(const char *pin);
bool pin_store_pwd_valid(const char *pwd);

/* 同时保存两组凭据并指定用哪一种解锁。
 * pin / pwd 传空串表示清空该组。mode 必须与内容一致：
 *   PIN  → pin 非空
 *   PASSWORD → pwd 非空
 *   NONE → 两组都为空（会清空全部） */
esp_err_t pin_store_set_credentials(const char *pin, const char *pwd, unlock_mode_t mode);

/* 清空全部凭据（恢复出厂用） */
void pin_store_reset(void);
