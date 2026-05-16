# imu_tof_logging

Sony Spresense/CXD5602PWBIMU の IMU データと、拡張ボード A5 pin の電圧を MicroSD に CSV 形式で保存する Arduino スケッチです。

現在の `feat/adc` ブランチでは、IMU と ADC A5 は別ファイルに記録します。A5 は Spresense Arduino core の割り当てに合わせて HPADC1 (`/dev/hpadc1`) から読み出します。

## ファイル構成

| ファイル | 役割 |
| --- | --- |
| `imu_tof_logging.ino` | Arduino スケッチ本体。CXD5602PWBIMU と MicroSD を初期化し、C 側の `main(0, NULL)` を呼び出します。 |
| `cxd5602pwbimu_main.c` | IMU と HPADC1/A5 を同時に読み出し、それぞれ別CSVへ追記します。 |

## 出力ファイル

### IMU

出力先: `/mnt/sd0/imu_log.csv`

| 列名 | 内容 |
| --- | --- |
| `timestamp` | IMU の `timestamp` を `19200000.0` で割った秒単位の値。 |
| `temp` | `cxd5602pwbimu_data_t` の温度値。 |
| `gx`, `gy`, `gz` | ジャイロ値。 |
| `ax`, `ay`, `az` | 加速度値。 |

### ADC A5

出力先: `/mnt/sd0/adc_a5_log.csv`

| 列名 | 内容 |
| --- | --- |
| `sample_index` | ADC A5 の連番サンプル番号。 |
| `timestamp_sec` | `sample_index / 16000` で計算したADCログ内の相対時刻。 |
| `raw` | HPADC1 から読み出した `int16_t` 生値。 |
| `voltage_v` | A5 の電圧推定値。Spresense Arduino core の HPADC1 マッピングを元に、拡張ボードの 0-5V として換算します。 |

## 現在の固定設定

- 収録時間: `10` 秒
- IMU サンプリングレート: `1920` Hz
- IMU 加速度レンジ: `2`
- IMU ジャイロレンジ: `125`
- ADC pin: 拡張ボード A5
- ADC デバイス: `/dev/hpadc1`
- ADC サンプリングレート: `16` kHz
- ADC 周波数係数: `7`

## 必要な環境

- Sony Spresense Arduino 環境
- CXD5602PWBIMU が `/dev/imu0` として使える環境
- MicroSD カードが `/mnt/sd0` にマウントされる環境
- HPADC1 が有効な Spresense SDK/Arduino core 設定

## 注意点

- CSV は既存ファイルがある場合は追記されます。空または存在しない場合のみヘッダーを書きます。
- ADC の `timestamp_sec` はADCサンプル番号から計算した相対時刻で、RTCやIMU timestampではありません。
- `voltage_v` はソース内の線形換算値です。厳密な校正が必要な場合は、実測に基づいて `ADC_A5_RAW_MIN`/`ADC_A5_RAW_MAX` または換算式を調整してください。
- このリポジトリにはビルドスクリプト、Arduino IDE 設定、テストは含まれていません。
