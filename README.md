# imu_tof_logging

<<<<<<< HEAD
Sony Spresense/CXD5602PWBIMU の IMU データ、VL53L1X ToF 距離、
拡張ボード A5 pin の電圧取得用 HPADC 生値を MicroSD に
バイナリ形式で保存する Arduino スケッチです。

現在の `feat/adc` ブランチでは、IMU、ToF、ADC A5 は別ファイルに記録します。
ToF は I2C0 の VL53L1X (`0x29`) から読み出します。
A5 は Spresense Arduino core の割り当てに合わせて HPADC1 (`/dev/hpadc1`)
から読み出します。
=======
Sony Spresense/CXD5602PWBIMU と VL53L1X 系 ToF センサーを使い、IMU と距離データを MicroSD に CSV 形式で記録する Arduino スケッチです。

このリポジトリには、Arduino 側の初期化処理と、Sony の `cxd5602pwbimu` サンプルを基にした IMU ロギング処理が入っています。実行すると `/dev/imu0` から IMU データを読み、最新の ToF 測距値を各 IMU サンプル行に付与して `/mnt/sd0/imu_tof_log.csv` へ追記します。
>>>>>>> main

## ファイル構成

| ファイル | 役割 |
| --- | --- |
<<<<<<< HEAD
| `imu_tof_logging.ino` | Arduino スケッチ本体。CXD5602PWBIMU と MicroSD を初期化し、C 側の `main(0, NULL)` を呼び出します。 |
| `app_main.c` | C 側エントリポイント。`capture_session_run()` を呼び出します。 |
| `capture_session.c` / `capture_session.h` | IMU、ToF、ADC A5 の起動、poll ループ、収録時間管理、終了処理をまとめる収録制御層です。 |
| `imu_recorder.c` / `imu_recorder.h` | `/dev/imu0` の設定、読み出し、IMU バイナリログへの保存を担当します。 |
| `tof_recorder.cpp` / `tof_recorder.h` | I2C0 の VL53L1X (`0x29`) 初期化、連続測距、ToF バイナリログへの保存を担当します。 |
| `adc_a5_recorder.c` / `adc_a5_recorder.h` | `/dev/hpadc1` の設定、A5 生値の読み出し、ADC バイナリログへの保存を担当します。 |
| `binary_file.c` / `binary_file.h` | バイナリログを上書きモードで開き、`fwrite()` の結果確認を共通化します。 |
| `binary_stream.c` / `binary_stream.h` | 2面バッファと書き込みスレッドで、収録中のSD書き込みを担当します。 |

## 出力ファイル

### IMU

出力先: `/mnt/sd0/imu_log.bin`

| フィールド | 内容 |
| --- | --- |
| ヘッダ | `magic[8]`, `version`, `sample_rate_hz`, `record_size`, `session_start_us`。 |
| レコード | `session_time_us` と `cxd5602pwbimu_data_t` を `record_size` バイトずつ連続保存します。 |

`session_time_us` は全センサ共通の収録開始時刻からの経過時間です。
IMU timestamp の秒換算は、後段の変換処理で `timestamp / 19200000.0`
として行います。

### ToF

出力先: `/mnt/sd0/tof_log.bin`

| フィールド | 内容 |
| --- | --- |
| ヘッダ | `magic[8]`, `version`, `i2c_address`, `sample_period_ms`, `timing_budget_us`, `record_size`, `session_start_us`。 |
| レコード | `session_time_us`, `sample_index`, `distance_mm`, `range_status`, `timeout` を含む `tof_sample_t`。 |

ToF は `Wire`、つまり Spresense の I2C0 を使います。連続測距周期は `50` ms、
タイミングバジェットは `50000` us です。

### ADC A5

出力先: `/mnt/sd0/adc_a5_log.bin`

| フィールド | 内容 |
| --- | --- |
| ヘッダ | `magic[8]`, `version`, `sample_rate_hz`, `record_size`, `session_start_us`, `reference_voltage`, `raw_min`, `raw_max`。 |
| レコード | HPADC1 から読み出した `int16_t raw` を `record_size` バイトずつ連続保存します。 |

ADC の `sample_index` はヘッダ直後からのレコード順で復元します。
`session_time_us` は `sample_index / 16000` から計算します。
電圧推定値は、ヘッダ内の `reference_voltage`, `raw_min`, `raw_max` を使って
後段で線形換算します。

## 現在の固定設定

- 収録時間: `10` 秒
- IMU サンプリングレート: `1920` Hz
- IMU 加速度レンジ: `2`
- IMU ジャイロレンジ: `125`
- ToF センサ: VL53L1X
- ToF I2C: I2C0
- ToF I2Cアドレス: `0x29`
- ToF 測距周期: `50` ms
- ToF タイミングバジェット: `50000` us
- ADC pin: 拡張ボード A5
- ADC デバイス: `/dev/hpadc1`
- ADC サンプリングレート: `16` kHz
- ADC 周波数係数: `7`
- IMU 2面バッファ: `1024` samples/面
- ToF 2面バッファ: `64` samples/面
- ADC A5 2面バッファ: `8192` samples/面

## バッファ方式

収録中のSD書き込みは `binary_stream.c` の2面バッファで行います。
リングバッファは柔軟ですが、読み書き位置、満杯/空判定、欠落時の扱いが増えるため、
この用途ではコード量が増えます。固定長2面バッファは「収録側が active 面に詰める」
「満杯になった面を writer thread がSDへ書く」という役割に分けられるため、
実装量を抑えつつ、収録時間をRAM容量から切り離せます。

## 必要な環境

- Sony Spresense Arduino 環境
- CXD5602PWBIMU が `/dev/imu0` として使える環境
- MicroSD カードが `/mnt/sd0` にマウントされる環境
- VL53L1X センサが I2C0 のアドレス `0x29` で応答する環境
- VL53L1X Arduino ライブラリ
- HPADC1 が有効な Spresense SDK/Arduino core 設定

## 注意点

- バイナリログは収録開始時に上書きされます。過去ログを残す場合は、実行前に別名へ退避してください。
- 収録中のSD書き込みは2面バッファと書き込みスレッドで行います。
  SD書き込みが追いつかず2面とも埋まった場合は、サンプル欠落を避けるため
  エラーとして収録を止めます。
- 実行中の `BUF` 表示はIMUの active 面使用率です。面の切り替え後に
  `0%` 付近へ戻るのは正常で、収録全体の進捗率ではありません。
- 各ログの `session_start_us` は同じ CLOCK_MONOTONIC 基準時刻です。
  CSV復元時は各行に `session_time_us` と `monotonic_us` を出力します。
- バイナリ値は Spresense 側のネイティブ表現です。別環境で読む場合はヘッダの `record_size` を確認してください。
- ADC の `session_time_us` はADCサンプル番号から計算する値で、RTCやIMU timestampではありません。
- ADC の電圧値はログ内に直接保存せず、`int16_t raw` とヘッダ内の
  線形換算情報から復元します。厳密な校正が必要な場合は、実測に基づいて
  `ADC_A5_RAW_MIN`/`ADC_A5_RAW_MAX` または換算式を調整してください。
- このリポジトリにはビルドスクリプト、Arduino IDE 設定、テストは含まれていません。
=======
| `imu_tof_logging.ino` | Arduino スケッチ本体。CXD5602PWBIMU、MicroSD、I2C/ToF センサーを初期化し、C 側の `main(0, NULL)` を呼び出します。 |
| `cxd5602pwbimu_main.c` | IMU デバイス `/dev/imu0` を開き、サンプリング設定、読み出し、ToF 値の合成、CSV 追記を行います。 |

## 動作内容

1. `setup()` で `board_cxd5602pwbimu_initialize(5)` を呼び、CXD5602PWBIMU を初期化します。
2. `SD.begin()` で MicroSD をマウントします。
3. `VL53L1X` を I2C アドレス `0x29` で初期化します。
   - I2C クロック: `400000` Hz
   - ToF タイムアウト: `20` ms
   - 距離モード: `Short`
   - 測定タイミング予算: `20000` us
   - 連続測定周期: `20` ms
4. `cxd5602pwbimu_main.c` の `main()` が `/dev/imu0` を開きます。
5. IMU を以下の固定設定で開始します。
   - サンプリングレート: `1920` Hz
   - 加速度レンジ: `2`
   - ジャイロレンジ: `125`
   - FIFO しきい値: `1`
6. IMU サンプルを読み出すループ内で ToF もポーリングし、最後に取得できた ToF サンプルを IMU 行へ付与します。
7. 収録開始から最大 `10` 秒で停止します。
8. CSV は `128` サンプルごとに `fflush()` され、最後に残りのバッファも書き出されます。

## 出力ファイル

出力先は固定で `/mnt/sd0/imu_tof_log.csv` です。ファイルが存在しない、または空の場合のみヘッダーを書きます。既存ファイルがある場合は追記します。

CSV の列は次の通りです。

| 列名 | 内容 |
| --- | --- |
| `imu_timestamp` | IMU の `timestamp` を `19200000.0` で割った秒単位の値。 |
| `imu_temp` | `cxd5602pwbimu_data_t` の温度値。 |
| `imu_gx`, `imu_gy`, `imu_gz` | `cxd5602pwbimu_data_t` のジャイロ値。 |
| `imu_ax`, `imu_ay`, `imu_az` | `cxd5602pwbimu_data_t` の加速度値。 |
| `tof_timestamp_ms` | ToF サンプル取得時の Arduino `millis()` 値。ToF 未取得時は `0`。 |
| `tof_distance_mm` | VL53L1X の距離値 mm。ToF 未取得時は `-1`。 |
| `tof_status` | VL53L1X の `range_status`。ToF 未取得時は `-1`。 |

## 必要な環境

- Sony Spresense 系の Arduino 環境
- CXD5602PWBIMU が `/dev/imu0` として使える環境
- MicroSD カードが `/mnt/sd0` にマウントされる環境
- `SDHCI`, `Wire`, `VL53L1X` ライブラリ
- I2C アドレス `0x29` の VL53L1X 系 ToF センサー

## 現状の注意点

- このリポジトリにはビルドスクリプト、Arduino IDE 設定、テストは含まれていません。
- 収録時間、サンプリングレート、レンジ、CSV 出力先はソース内の定数として固定されています。
- ToF 初期化に失敗しても IMU 側の記録処理は続行されます。その場合、ToF 列は未取得値になります。
- MicroSD のマウントや CSV オープンに失敗すると、CSV 記録処理は開始できません。
- `setup()` から C 側の `main()` を一度呼び出す構成です。`loop()` は空なので、1 回の起動で 1 回の収録を行います。
>>>>>>> main
