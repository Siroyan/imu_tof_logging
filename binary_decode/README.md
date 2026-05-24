# binary_decode

## バイナリログ変換

```sh
python main.py
```

ADC A5 の raw int16 を 16kHz/mono/16bit PCM WAV に変換する場合:

```sh
python main.py --adc-wav
```

CSV変換を省略してWAVだけ出力する場合:

```sh
python main.py --skip-csv --adc-wav
```

## ADC A5 FFT

`adc_a5_log.csv` の `voltage_v` 列から DC 成分を除去し、20msごとに
Hann 窓をかけて1kHz以下の最大ピーク周波数を求めます。

```sh
python fft.py
```

結果は `adc_a5_fft_peaks.csv` に出力されます。各行は1つの20ms窓で、
`center_sec` と `peak_frequency_hz` を見ればピーク周波数の時間変化を確認できます。
同時に、横軸 `center_sec`、縦軸 `peak_frequency_hz` のPNG画像も同じ場所に保存されます。
画像の表示範囲は横軸 0-10s、縦軸 0-200Hz です。

任意のCSVを指定する場合:

```sh
python fft.py path/to/adc_a5_log.csv
```

主なオプション:

```sh
python fft.py --column raw --max-freq 1000 --min-freq 1
python fft.py --sample-rate 16000
python fft.py --window-ms 20 --hop-ms 20
python fft.py --window 320 --hop 160
```

PNG画像のファイル名は、入力ファイル名と実際のwindow/hopサンプル数から決まります。
例えば `adc_a5_log.csv` を `--window 4096 --hop 32` で解析した場合は、
`adc_a5_log_fft_peaks_window4096_hop32.png` が作成されます。

50Hz と 100Hz をピーク探索から除外する場合:

```sh
python fft.py --exclude-freq 50 --exclude-freq 100 --exclude-width 5
python fft.py -x 50 -x 100 -w 5
```
