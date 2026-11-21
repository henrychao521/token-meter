# 額度顯示器（LILYGO T-QT Pro）

在桌上用 0.85 吋小螢幕輪播 AI 工具的剩餘額度：Gemini、Claude／GPT 的週額度與 5 小時額度，
Antigravity 生圖剩餘張數、Colab 運算單元與 Claude Code 今日用量。

**實作教學**：https://henrychao521.github.io/token-meter/

- `firmware/token_meter/`：板子韌體（Arduino＋LovyanGFX）。複製 `secrets.example.h` 成 `secrets.h` 再編譯。
- `server/meter_server.py`：電腦端資料服務（埠 8817，`/meter.json`），額度資料取自
  [antigravity-bridge](https://github.com/henrychao521/antigravity-bridge)。
- `tools/make_u8g2_font.py`：TTF 轉 U8g2 中文點陣字型子集。

字型：俐方體11號 Cubic 11，SIL Open Font License 1.1（`tools/Cubic_11_OFL.txt`）。
