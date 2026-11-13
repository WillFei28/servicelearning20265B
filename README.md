# 鐵路平交道模擬（穩定版）

`main` 是穩定參考。接線、行為同程式以這份說明同 `src/main.cpp` 為準。

六個組別 branch 由這個版本開出，起點同 `main` 一樣：

- `gp1`
- `gp2`
- `gp3`
- `gp4`
- `gp5`
- `gp6`

組別可以換下面三個音效。換完之後，腳位同行為仍然睇 `main` 這份 README。

板：ESP32-WROOM-32 38-pin，插在 Goouuu ESP32S 38P 擴展板。  
上傳線插 **ESP32 模組本身的 Micro-USB**。擴展板的 USB 同 DC 座只供電，傳唔到程式。

## 而家點運作

開機時附近唔好有磁鐵。程式會記住 7 粒霍爾傳感器各自的靜止電壓。

某一粒同自己的靜止電壓相差 **250 mV** 或以上就算有磁鐵：

- 電壓下跌是 **N**
- 電壓上升是 **S**

| 情況 | 燈帶（GPIO18 MOSFET） | 舵機 SG90 | OLED | 喇叭 |
| --- | --- | --- | --- | --- |
| 無磁鐵 | 熄 | 0° | 空白 | 停 |
| 有 N | 長亮。有 N 就優先過 S | 睇有無 S | 大剔號 | 停，除非同時有 S |
| 有 S | 每秒閃 2 下 | 180° | 大交叉 | 循環播平交道聲 |
| N 同 S 一齊 | 長亮 | 180° | 交叉同剔號一齊 | 循環播平交道聲 |

最近一次有效的 N 或 S 會記住，磁鐵拿走都保留。拍 RFID 卡就用這筆記錄，用完即刻清走。清走之後，要先回到無磁鐵，先可以再記新一次。

| 記住的極 | 畫面（至少約 2.2 秒；聲音更長就播完先收） | 聲音 |
| --- | --- | --- |
| N | `+$2.0` | `audio/octopus.mp3` |
| S | `-$2.0` | `audio/error.mp3` |
| 無記錄 | 無車費畫面 | 無聲 |

幾粒同時有磁鐵時，車費記錄用相差最大的那一粒。

## 接線

黃針是擴展板 GPIO。黑 G 是 GND。紅針 5V 唔好接 RC522。

### 燈帶 MOSFET

| 模組 | 接到 |
| --- | --- |
| 信號 | GPIO18（P18） |
| `+` | 5V |

N 長亮，S 每秒閃 2 下，無磁鐵熄滅。

### 7 粒 49E 經 74HC4051

74HC4051 用 **3.3V**。`GND`、`VEE`、`E` 接黑 G。`Y7` 留空。

| 74HC4051 | ESP32 |
| --- | --- |
| S0 | GPIO27 |
| S1 | GPIO14 |
| S2 | GPIO12 |
| Z | GPIO34 |
| Y0–Y6 | 49E 第 1–7 粒的 OUT |

每粒 49E：`VCC` 接 3.3V，`GND` 接黑 G，有字一面朝外。

### SG90 舵機

| 線 | 接到 |
| --- | --- |
| 橙（信號） | GPIO13 |
| 紅 | 5V |
| 棕 | 黑 G |

有 S 去 180°，無 S 返 0°。

### MAX98357A 喇叭

`GAIN` 同 `SD` 留空，唔好接地，亦唔好接 3.3V。

| MAX98357A | ESP32 |
| --- | --- |
| LRC | GPIO33 |
| BCLK | GPIO25 |
| DIN | GPIO26 |
| VIN | 5V |
| GND | 黑 G |
| 喇叭兩端 | 模組的喇叭端子 |

接線圖在 `docs/wiring/exp1-sound.svg`。YAML 是 `docs/wiring/exp1-sound.yml`，本機 WireViz 加 Graphviz 畫出嚟，腳位同上面一樣。

### 0.96 吋 SSD1306（128×64，I2C 位址 0x3C）

擴展板右上 4 pin，跟實已試過的插法：

| OLED | 擴展板 |
| --- | --- |
| VCC | VCC |
| GND | GND |
| SCL | D21（GPIO21） |
| SDA | D22（GPIO22） |

程式用 `Wire.begin(22, 21)`，同這組線一致。

### RC522 RFID

**只可以接 3.3V。** 唔好接紅針 5V。`IRQ` 留空。

| RC522 | ESP32 |
| --- | --- |
| 3.3V | 3V3 |
| GND | 黑 G |
| RST | GPIO16 |
| SDA / SS | GPIO5 |
| SCK | GPIO17 |
| MOSI | GPIO23 |
| MISO | GPIO19 |

SCK 用 GPIO17，避開 GPIO18 的 MOSFET。

## 三個音效可以換

建置前 `embed_crossing.py` 會把這三個檔寫進程式。唔好手改 `src/crossing_mp3.h`，那個檔每次編譯會重新產生，亦唔會入 git。

| 檔案 | 何時播 | 而家大小 |
| --- | --- | --- |
| `audio/crossing.mp3` | 有 S 就循環播 | 50782 bytes |
| `audio/error.mp3` | 拍卡，記錄是 S | 24451 bytes |
| `audio/octopus.mp3` | 拍卡，記錄是 N | 24450 bytes |

換檔時：

1. 檔名保持 `crossing.mp3`、`error.mp3`、`octopus.mp3`。
2. 用普通 MP3。三個檔聲量盡量接近，太細會聽唔到。
3. 檔案開頭唔好帶 ID3 封面或標籤。而家三個穩定檔一開始就是 MP3 音訊框（`FF FB`）。前面如果有標籤，拍卡聲會慢一拍。
4. 只改自己組別的 branch，唔好直接改 `main`。

換完之後在專案資料夾上傳：

```bash
git checkout gp1
pio run -t upload --upload-port 上次成功的USB埠
```

`gp1` 換成自己的組別。Mac 上列出埠：

```bash
ls /dev/cu.usb*
```

序列埠監視是 115200。開機應見到 `Calibrating. Keep magnets away.`，以及每粒傳感器的靜止電壓。

## 穩定程式

行為同腳位常數在 `src/main.cpp` 開頭。PlatformIO 環境是 `esp32dev`，`espressif32@7.1.3`，Arduino framework。上傳速率 921600。

這份穩定版的程式提交是 `c98b87a`。README 在其後的提交加入。
