// 複製成 secrets.h 後填入自己的值（secrets.h 不要放上 GitHub）
#pragma once
#define WIFI_SSID "你的Wi-Fi名稱"
#define WIFI_PASS "你的Wi-Fi密碼"
#define OTA_PASS  "自訂一組OTA密碼"
#define MBP_HOST  "你的Mac區網名稱"        // 終端機 scutil --get LocalHostName，不含 .local
#define MBP_FALLBACK_IP 192, 168, 1, 10    // mDNS 查不到時改用的 Mac IP
