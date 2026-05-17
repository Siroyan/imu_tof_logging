import argparse
import csv
import struct
import wave
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent

IMU_HEADER = struct.Struct("<8sIII")
IMU_HEADER_V2_TAIL = struct.Struct("<IQ")
IMU_RECORD_V1 = struct.Struct("<I7f")
IMU_RECORD_V2 = struct.Struct("<QI7f")

ADC_HEADER_PREFIX = struct.Struct("<8sI")
ADC_HEADER_V1_TAIL = struct.Struct("<IIfhh")
ADC_HEADER_V2_TAIL = struct.Struct("<IIIQfhhII")
ADC_RECORD = struct.Struct("<h")

TOF_HEADER_PREFIX = struct.Struct("<8sI")
TOF_HEADER_V1_TAIL = struct.Struct("<IIII")
TOF_HEADER_V2_TAIL = struct.Struct("<IIIIIQ")
TOF_RECORD = struct.Struct("<QIHBB")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Convert Spresense binary logs to CSV, and optionally WAV."
    )
    parser.add_argument(
        "--adc-wav",
        action="store_true",
        help="Also convert adc_a5_log.bin to 16-bit mono WAV.",
    )
    parser.add_argument(
        "--adc-wav-output",
        default=SCRIPT_DIR / "adc_a5_log.wav",
        type=Path,
        help="ADC WAV output path. Defaults to binary_decode/adc_a5_log.wav.",
    )
    parser.add_argument(
        "--skip-csv",
        action="store_true",
        help="Skip CSV conversion and run only explicitly requested outputs.",
    )
    return parser.parse_args()


def read_exact(file_obj, size, label):
    data = file_obj.read(size)
    if len(data) != size:
        raise ValueError(f"truncated {label}")
    return data


def magic_text(raw_magic):
    return raw_magic.rstrip(b"\0")


def read_adc_header(file_obj):
    magic, version = ADC_HEADER_PREFIX.unpack(
        read_exact(file_obj, ADC_HEADER_PREFIX.size, "ADC header prefix")
    )
    if magic_text(magic) != b"ADCA5L1":
        raise ValueError(f"unexpected ADC magic: {magic!r}")
    if version >= 2:
        (rate, record_size, _reserved0, session_start_us, ref_v,
         raw_min, raw_max, _reserved1, _reserved2) = ADC_HEADER_V2_TAIL.unpack(
            read_exact(file_obj, ADC_HEADER_V2_TAIL.size, "ADC v2 header")
        )
    else:
        rate, record_size, ref_v, raw_min, raw_max = ADC_HEADER_V1_TAIL.unpack(
            read_exact(file_obj, ADC_HEADER_V1_TAIL.size, "ADC v1 header")
        )
        session_start_us = None

    if record_size != ADC_RECORD.size:
        raise ValueError(f"unsupported ADC record size: {record_size}")

    return rate, record_size, session_start_us, ref_v, raw_min, raw_max


def convert_imu(bin_path, csv_path):
    with open(bin_path, "rb") as f, open(csv_path, "w", newline="") as out:
        magic, version, rate, record_size = IMU_HEADER.unpack(
            read_exact(f, IMU_HEADER.size, "IMU header")
        )
        if magic_text(magic) != b"IMULOG1":
            raise ValueError(f"unexpected IMU magic: {magic!r}")
        session_start_us = None
        if version >= 2:
            _reserved, session_start_us = IMU_HEADER_V2_TAIL.unpack(
                read_exact(f, IMU_HEADER_V2_TAIL.size, "IMU v2 header")
            )
            record_struct = IMU_RECORD_V2
        else:
            record_struct = IMU_RECORD_V1

        if record_size != record_struct.size:
            raise ValueError(f"unsupported IMU record size: {record_size}")

        writer = csv.writer(out)
        writer.writerow([
            "session_time_us",
            "monotonic_us",
            "imu_timestamp_sec",
            "temp",
            "gx",
            "gy",
            "gz",
            "ax",
            "ay",
            "az",
        ])

        index = 0
        while True:
            data = f.read(record_size)
            if len(data) == 0:
                break
            if len(data) != record_size:
                raise ValueError("truncated IMU record")

            if version >= 2:
                (session_time_us, timestamp, temp, gx, gy, gz, ax, ay,
                 az) = record_struct.unpack(data)
                monotonic_us = session_start_us + session_time_us
            else:
                timestamp, temp, gx, gy, gz, ax, ay, az = record_struct.unpack(data)
                session_time_us = int(index * 1000000 / rate)
                monotonic_us = ""

            writer.writerow([
                session_time_us,
                monotonic_us,
                timestamp / 19200000.0,
                temp,
                gx,
                gy,
                gz,
                ax,
                ay,
                az,
            ])
            index += 1


def convert_adc(bin_path, csv_path):
    with open(bin_path, "rb") as f, open(csv_path, "w", newline="") as out:
        rate, record_size, session_start_us, ref_v, raw_min, raw_max = (
            read_adc_header(f)
        )

        writer = csv.writer(out)
        writer.writerow([
            "sample_index",
            "session_time_us",
            "monotonic_us",
            "timestamp_sec",
            "raw",
            "voltage_v",
        ])

        index = 0
        while True:
            data = f.read(record_size)
            if len(data) == 0:
                break
            if len(data) != record_size:
                raise ValueError("truncated ADC record")

            raw, = ADC_RECORD.unpack(data)
            session_time_us = int(index * 1000000 / rate)
            monotonic_us = (
                session_start_us + session_time_us
                if session_start_us is not None
                else ""
            )
            clipped = min(max(raw, raw_min), raw_max)
            voltage = (clipped - raw_min) * ref_v / (raw_max - raw_min)
            writer.writerow([index, session_time_us, monotonic_us, index / rate, raw, voltage])
            index += 1


def convert_adc_wav(bin_path, wav_path):
    with open(bin_path, "rb") as f, wave.open(str(wav_path), "wb") as out:
        rate, record_size, _session_start_us, _ref_v, _raw_min, _raw_max = (
            read_adc_header(f)
        )

        out.setnchannels(1)
        out.setsampwidth(ADC_RECORD.size)
        out.setframerate(rate)

        while True:
            data = f.read(record_size * 4096)
            if len(data) == 0:
                break
            if len(data) % record_size != 0:
                raise ValueError("truncated ADC record")

            # ADC raw int16をそのまま16-bit PCM monoとして保存する。
            out.writeframesraw(data)


def convert_tof(bin_path, csv_path):
    with open(bin_path, "rb") as f, open(csv_path, "w", newline="") as out:
        magic, version = TOF_HEADER_PREFIX.unpack(
            read_exact(f, TOF_HEADER_PREFIX.size, "ToF header prefix")
        )
        if magic_text(magic) != b"TOFLOG1":
            raise ValueError(f"unexpected ToF magic: {magic!r}")
        if version >= 2:
            (address, period_ms, budget_us, record_size, _reserved,
             session_start_us) = TOF_HEADER_V2_TAIL.unpack(
                read_exact(f, TOF_HEADER_V2_TAIL.size, "ToF v2 header")
            )
        else:
            address, period_ms, budget_us, record_size = TOF_HEADER_V1_TAIL.unpack(
                read_exact(f, TOF_HEADER_V1_TAIL.size, "ToF v1 header")
            )
            session_start_us = None

        if record_size != TOF_RECORD.size:
            raise ValueError(f"unsupported ToF record size: {record_size}")

        writer = csv.writer(out)
        writer.writerow([
            "session_time_us",
            "monotonic_us",
            "sample_index",
            "distance_mm",
            "range_status",
            "timeout",
            "i2c_address",
            "sample_period_ms",
            "timing_budget_us",
        ])

        while True:
            data = f.read(record_size)
            if len(data) == 0:
                break
            if len(data) != record_size:
                raise ValueError("truncated ToF record")

            (session_time_us, sample_index, distance_mm, range_status,
             timeout) = TOF_RECORD.unpack(data)
            monotonic_us = (
                session_start_us + session_time_us
                if session_start_us is not None
                else ""
            )
            writer.writerow([
                session_time_us,
                monotonic_us,
                sample_index,
                distance_mm,
                range_status,
                timeout,
                f"0x{address:02x}",
                period_ms,
                budget_us,
            ])


def main():
    args = parse_args()
    conversions = [
        (convert_imu, "imu_log.bin", "imu_log.csv"),
        (convert_adc, "adc_a5_log.bin", "adc_a5_log.csv"),
        (convert_tof, "tof_log.bin", "tof_log.csv"),
    ]

    if not args.skip_csv:
        for convert, input_name, output_name in conversions:
            input_path = SCRIPT_DIR / input_name
            output_path = SCRIPT_DIR / output_name
            if not input_path.exists():
                print(f"skip: {input_path.name} not found")
                continue

            convert(input_path, output_path)
            print(f"wrote: {output_path.name}")

    if args.adc_wav:
        input_path = SCRIPT_DIR / "adc_a5_log.bin"
        if not input_path.exists():
            print(f"skip: {input_path.name} not found")
        else:
            convert_adc_wav(input_path, args.adc_wav_output)
            print(f"wrote: {args.adc_wav_output}")


if __name__ == "__main__":
    main()
