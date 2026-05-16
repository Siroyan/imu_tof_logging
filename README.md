# imu_tof_logging

Sony Spresense/CXD5602PWBIMU と VL53L1X 系 ToF センサーを使い、IMU と距離データを MicroSD に CSV 形式で記録する Arduino スケッチです。

このリポジトリには、Arduino 側の初期化処理と、Sony の `cxd5602pwbimu` サンプルを基にした IMU ロギング処理が入っています。実行すると `/dev/imu0` から IMU データを読み、最新の ToF 測距値を各 IMU サンプル行に付与して `/mnt/sd0/imu_tof_log.csv` へ追記します。

## ファイル構成

| ファイル | 役割 |
| --- | --- |
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
