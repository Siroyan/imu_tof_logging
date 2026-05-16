import csv
import struct
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent

IMU_HEADER = struct.Struct("<8sIII")
IMU_RECORD = struct.Struct("<I7f")

ADC_HEADER = struct.Struct("<8sIIIfhh")
ADC_RECORD = struct.Struct("<h")

TOF_HEADER = struct.Struct("<8sIIIII")
TOF_RECORD = struct.Struct("<QIHBB")


def read_exact(file_obj, size, label):
    data = file_obj.read(size)
    if len(data) != size:
        raise ValueError(f"truncated {label}")
    return data


def magic_text(raw_magic):
    return raw_magic.rstrip(b"\0")


def convert_imu(bin_path, csv_path):
    with open(bin_path, "rb") as f, open(csv_path, "w", newline="") as out:
        magic, version, rate, record_size = IMU_HEADER.unpack(
            read_exact(f, IMU_HEADER.size, "IMU header")
        )
        if magic_text(magic) != b"IMULOG1":
            raise ValueError(f"unexpected IMU magic: {magic!r}")
        if record_size != IMU_RECORD.size:
            raise ValueError(f"unsupported IMU record size: {record_size}")

        writer = csv.writer(out)
        writer.writerow(["timestamp_sec", "temp", "gx", "gy", "gz", "ax", "ay", "az"])

        while True:
            data = f.read(record_size)
            if len(data) == 0:
                break
            if len(data) != record_size:
                raise ValueError("truncated IMU record")

            timestamp, temp, gx, gy, gz, ax, ay, az = IMU_RECORD.unpack(data)
            writer.writerow([timestamp / 19200000.0, temp, gx, gy, gz, ax, ay, az])


def convert_adc(bin_path, csv_path):
    with open(bin_path, "rb") as f, open(csv_path, "w", newline="") as out:
        magic, version, rate, record_size, ref_v, raw_min, raw_max = ADC_HEADER.unpack(
            read_exact(f, ADC_HEADER.size, "ADC header")
        )
        if magic_text(magic) != b"ADCA5L1":
            raise ValueError(f"unexpected ADC magic: {magic!r}")
        if record_size != ADC_RECORD.size:
            raise ValueError(f"unsupported ADC record size: {record_size}")

        writer = csv.writer(out)
        writer.writerow(["sample_index", "timestamp_sec", "raw", "voltage_v"])

        index = 0
        while True:
            data = f.read(record_size)
            if len(data) == 0:
                break
            if len(data) != record_size:
                raise ValueError("truncated ADC record")

            raw, = ADC_RECORD.unpack(data)
            clipped = min(max(raw, raw_min), raw_max)
            voltage = (clipped - raw_min) * ref_v / (raw_max - raw_min)
            writer.writerow([index, index / rate, raw, voltage])
            index += 1


def convert_tof(bin_path, csv_path):
    with open(bin_path, "rb") as f, open(csv_path, "w", newline="") as out:
        magic, version, address, period_ms, budget_us, record_size = TOF_HEADER.unpack(
            read_exact(f, TOF_HEADER.size, "ToF header")
        )
        if magic_text(magic) != b"TOFLOG1":
            raise ValueError(f"unexpected ToF magic: {magic!r}")
        if record_size != TOF_RECORD.size:
            raise ValueError(f"unsupported ToF record size: {record_size}")

        writer = csv.writer(out)
        writer.writerow([
            "timestamp_us",
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

            timestamp_us, sample_index, distance_mm, range_status, timeout = TOF_RECORD.unpack(data)
            writer.writerow([
                timestamp_us,
                sample_index,
                distance_mm,
                range_status,
                timeout,
                f"0x{address:02x}",
                period_ms,
                budget_us,
            ])


def main():
    conversions = [
        (convert_imu, "imu_log.bin", "imu_log.csv"),
        (convert_adc, "adc_a5_log.bin", "adc_a5_log.csv"),
        (convert_tof, "tof_log.bin", "tof_log.csv"),
    ]

    for convert, input_name, output_name in conversions:
        input_path = SCRIPT_DIR / input_name
        output_path = SCRIPT_DIR / output_name
        if not input_path.exists():
            print(f"skip: {input_path.name} not found")
            continue

        convert(input_path, output_path)
        print(f"wrote: {output_path.name}")


if __name__ == "__main__":
    main()
