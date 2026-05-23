import argparse
import csv
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError as exc:
    raise SystemExit(
        "numpy is required. Install dependencies with `uv sync` or `pip install numpy`."
    ) from exc


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_INPUT = SCRIPT_DIR / "adc_a5_log.csv"
DEFAULT_OUTPUT = SCRIPT_DIR / "adc_a5_fft_peaks.csv"
DEFAULT_SAMPLE_RATE_HZ = 16000.0
DEFAULT_MAX_FREQ_HZ = 1000.0
DEFAULT_WINDOW_MS = 20.0


def parse_args():
    parser = argparse.ArgumentParser(
        description="Track dominant ADC A5 frequency peaks with windowed FFT."
    )
    parser.add_argument(
        "csv_path",
        nargs="?",
        default=DEFAULT_INPUT,
        type=Path,
        help="ADC CSV path. Defaults to binary_decode/adc_a5_log.csv.",
    )
    parser.add_argument(
        "--output",
        default=DEFAULT_OUTPUT,
        type=Path,
        help="Output CSV path. Defaults to binary_decode/adc_a5_fft_peaks.csv.",
    )
    parser.add_argument(
        "--column",
        default="voltage_v",
        help="Signal column to analyze. Defaults to voltage_v.",
    )
    parser.add_argument(
        "--sample-rate",
        type=float,
        default=None,
        help="Sample rate in Hz. If omitted, timestamp_sec/session_time_us is used.",
    )
    parser.add_argument(
        "--max-freq",
        type=float,
        default=DEFAULT_MAX_FREQ_HZ,
        help="Maximum peak search frequency in Hz. Defaults to 1000.",
    )
    parser.add_argument(
        "--min-freq",
        type=float,
        default=1.0,
        help="Minimum peak search frequency in Hz. Defaults to 1 to ignore DC drift.",
    )
    parser.add_argument(
        "--window-ms",
        type=float,
        default=DEFAULT_WINDOW_MS,
        help="FFT window width in milliseconds. Defaults to 20.",
    )
    parser.add_argument(
        "--hop-ms",
        type=float,
        default=None,
        help="Window step in milliseconds. Defaults to --window-ms.",
    )
    parser.add_argument(
        "--window",
        type=int,
        default=None,
        help="FFT window width in samples. Overrides --window-ms.",
    )
    parser.add_argument(
        "--hop",
        type=int,
        default=None,
        help="Window step in samples. Overrides --hop-ms.",
    )
    parser.add_argument(
        "-x",
        "--exclude-freq",
        action="append",
        type=float,
        default=[],
        help="Frequency to exclude from peak search. May be specified multiple times.",
    )
    parser.add_argument(
        "-w",
        "--exclude-width",
        type=float,
        default=5.0,
        help="Half width in Hz around each excluded frequency. Defaults to 5.",
    )
    return parser.parse_args()


def read_adc_csv(csv_path, column):
    values = []
    timestamps = []
    session_times = []

    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError("CSV has no header")
        if column not in reader.fieldnames:
            raise ValueError(
                f"column {column!r} not found. Available columns: "
                f"{', '.join(reader.fieldnames)}"
            )

        for row in reader:
            value = row.get(column, "")
            if value == "":
                continue
            values.append(float(value))

            timestamp = row.get("timestamp_sec", "")
            if timestamp != "":
                timestamps.append(float(timestamp))

            session_time = row.get("session_time_us", "")
            if session_time != "":
                session_times.append(float(session_time))

    if len(values) < 4:
        raise ValueError("not enough samples for FFT")

    return np.asarray(values, dtype=np.float64), timestamps, session_times


def estimate_sample_rate(sample_rate, timestamps, session_times, sample_count):
    if sample_rate is not None:
        if sample_rate <= 0:
            raise ValueError("--sample-rate must be positive")
        return sample_rate, "argument"

    if len(timestamps) == sample_count and timestamps[-1] > timestamps[0]:
        duration = timestamps[-1] - timestamps[0]
        return (sample_count - 1) / duration, "timestamp_sec"

    if len(session_times) == sample_count and session_times[-1] > session_times[0]:
        duration_us = session_times[-1] - session_times[0]
        return (sample_count - 1) * 1000000.0 / duration_us, "session_time_us"

    return DEFAULT_SAMPLE_RATE_HZ, "default"


def parabolic_peak(freqs, magnitudes, peak_index):
    if peak_index <= 0 or peak_index >= len(magnitudes) - 1:
        return freqs[peak_index], magnitudes[peak_index]

    left = magnitudes[peak_index - 1]
    center = magnitudes[peak_index]
    right = magnitudes[peak_index + 1]
    denominator = left - 2.0 * center + right
    if denominator == 0.0:
        return freqs[peak_index], center

    offset = 0.5 * (left - right) / denominator
    bin_width = freqs[1] - freqs[0]
    frequency = freqs[peak_index] + offset * bin_width
    magnitude = center - 0.25 * (left - right) * offset
    return frequency, magnitude


def build_search_mask(freqs, min_freq, max_freq, exclude_freqs, exclude_width):
    if exclude_width < 0:
        raise ValueError("--exclude-width must be non-negative")

    search_mask = (freqs >= min_freq) & (freqs <= max_freq)
    for excluded in exclude_freqs:
        if excluded < 0:
            raise ValueError("--exclude-freq must be non-negative")
        search_mask &= np.abs(freqs - excluded) > exclude_width

    return search_mask


def find_peak(signal, sample_rate, min_freq, max_freq, exclude_freqs,
              exclude_width):
    if min_freq < 0 or max_freq <= min_freq:
        raise ValueError("--max-freq must be greater than --min-freq")
    if max_freq > sample_rate / 2.0:
        raise ValueError("--max-freq exceeds Nyquist frequency")

    centered = signal - np.mean(signal)
    window = np.hanning(len(centered))
    windowed = centered * window
    spectrum = np.fft.rfft(windowed)
    freqs = np.fft.rfftfreq(len(windowed), d=1.0 / sample_rate)

    # Hann窓の振幅低下を補正し、片側振幅スペクトルとして扱う。
    magnitudes = np.abs(spectrum) * 2.0 / np.sum(window)
    search_mask = build_search_mask(
        freqs,
        min_freq,
        max_freq,
        exclude_freqs,
        exclude_width,
    )
    if not np.any(search_mask):
        raise ValueError("no FFT bins in the requested frequency range")

    search_indices = np.flatnonzero(search_mask)
    peak_index = search_indices[np.argmax(magnitudes[search_mask])]
    peak_freq, peak_magnitude = parabolic_peak(freqs, magnitudes, peak_index)
    return peak_freq, peak_magnitude, freqs[1] - freqs[0]


def window_samples(sample_rate, window_ms, hop_ms, window_samples_arg,
                   hop_samples_arg):
    if window_samples_arg is not None:
        window_size = window_samples_arg
    else:
        if window_ms <= 0:
            raise ValueError("--window-ms must be positive")
        window_size = int(round(sample_rate * window_ms / 1000.0))

    if hop_samples_arg is not None:
        hop_size = hop_samples_arg
    elif hop_ms is not None:
        if hop_ms <= 0:
            raise ValueError("--hop-ms must be positive")
        hop_size = int(round(sample_rate * hop_ms / 1000.0))
    elif window_samples_arg is not None:
        hop_size = window_size
    else:
        hop_size = int(round(sample_rate * window_ms / 1000.0))

    if window_size < 4:
        raise ValueError("window is too short for FFT")
    if hop_size < 1:
        raise ValueError("hop is too short")

    return window_size, hop_size


def analyze_windows(signal, sample_rate, window_size, hop_size, min_freq,
                    max_freq, exclude_freqs, exclude_width):
    rows = []
    start = 0
    window_index = 0

    while start + window_size <= len(signal):
        end = start + window_size
        peak_freq, peak_magnitude, bin_width = find_peak(
            signal[start:end],
            sample_rate,
            min_freq,
            max_freq,
            exclude_freqs,
            exclude_width,
        )
        rows.append({
            "window_index": window_index,
            "start_sample": start,
            "end_sample": end,
            "start_sec": start / sample_rate,
            "center_sec": (start + end - 1) * 0.5 / sample_rate,
            "end_sec": end / sample_rate,
            "peak_frequency_hz": peak_freq,
            "peak_magnitude": peak_magnitude,
        })

        start += hop_size
        window_index += 1

    if not rows:
        raise ValueError("not enough samples for the requested window width")

    return rows, bin_width


def write_peak_csv(output_path, rows):
    fieldnames = [
        "window_index",
        "start_sample",
        "end_sample",
        "start_sec",
        "center_sec",
        "end_sec",
        "peak_frequency_hz",
        "peak_magnitude",
    ]
    with open(output_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def main():
    args = parse_args()
    signal, timestamps, session_times = read_adc_csv(args.csv_path, args.column)
    sample_rate, sample_rate_source = estimate_sample_rate(
        args.sample_rate,
        timestamps,
        session_times,
        len(signal),
    )
    window_size, hop_size = window_samples(
        sample_rate,
        args.window_ms,
        args.hop_ms,
        args.window,
        args.hop,
    )
    rows, bin_width = analyze_windows(
        signal,
        sample_rate,
        window_size,
        hop_size,
        args.min_freq,
        args.max_freq,
        args.exclude_freq,
        args.exclude_width,
    )
    write_peak_csv(args.output, rows)
    strongest = max(rows, key=lambda row: row["peak_magnitude"])

    print(f"input: {args.csv_path}")
    print(f"output: {args.output}")
    print(f"column: {args.column}")
    print(f"samples: {len(signal)}")
    print(f"sample_rate_hz: {sample_rate:.6f} ({sample_rate_source})")
    print(f"window_ms: {window_size * 1000.0 / sample_rate:.6f}")
    print(f"window_samples: {window_size}")
    print(f"hop_ms: {hop_size * 1000.0 / sample_rate:.6f}")
    print(f"hop_samples: {hop_size}")
    print(f"windows: {len(rows)}")
    print(f"fft_bin_width_hz: {bin_width:.6f}")
    print(f"search_range_hz: {args.min_freq:.3f} - {args.max_freq:.3f}")
    if args.exclude_freq:
        excluded = ", ".join(f"{freq:.3f}" for freq in args.exclude_freq)
        print(f"excluded_freq_hz: {excluded} (+/- {args.exclude_width:.3f})")
    print(
        "strongest_window: "
        f"{strongest['window_index']} "
        f"center_sec={strongest['center_sec']:.6f} "
        f"peak_frequency_hz={strongest['peak_frequency_hz']:.6f} "
        f"peak_magnitude={strongest['peak_magnitude']:.9g}"
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
