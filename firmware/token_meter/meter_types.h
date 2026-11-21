#pragma once
#include <stdint.h>
// 放在獨立標頭檔：Arduino 自動產生的函式宣告會排在 .ino 的結構定義之前
struct Bucket { bool has = false; float pct = 0; uint32_t reset = 0; };
