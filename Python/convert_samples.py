#!/usr/bin/env python3

import argparse
import csv
import sys
import struct
from pathlib import Path
from datetime import datetime, timezone

EPOCH_OFFSET = 1767225600
RECORD_FORMAT = "<IIII"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)


def convert_file(input_file, output_file, make_plot=False, plot_file=None):
    data = Path(input_file).read_bytes()

    if len(data) % RECORD_SIZE != 0:
        raise ValueError(
            f"File size ({len(data)} bytes) is not a multiple "
            f"of the record size ({RECORD_SIZE} bytes). "
            "Check the binary format."
        )

    record_count = len(data) // RECORD_SIZE

    times = []
    iso_times = []
    vbat_values = []
    rtc_temp_values = []
    temp_values = []

    with open(output_file, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)

        writer.writerow(
            [
                "Unix time",
                "ISO8601 time",
                "vbat_mv",
                "RTCtemp_c",
                "temp_c",
            ]
        )

        for offset in range(0, len(data), RECORD_SIZE):
            timestamp, vbat, rtc_temp, temp = struct.unpack_from(
                RECORD_FORMAT, data, offset
            )

            unix_time = (timestamp * 2) + EPOCH_OFFSET

            iso8601_time = (
                datetime.fromtimestamp(unix_time, tz=timezone.utc)
                .isoformat()
                .replace("+00:00", "Z")
            )

            vbat_mv = vbat / 10
            rtc_temp_c = rtc_temp / 10
            temp_c = temp / 10

            writer.writerow(
                [
                    unix_time,
                    iso8601_time,
                    vbat_mv,
                    rtc_temp_c,
                    temp_c,
                ]
            )

            if make_plot:
                times.append(datetime.fromtimestamp(unix_time, tz=timezone.utc))
                iso_times.append(iso8601_time)
                vbat_values.append(vbat_mv)
                rtc_temp_values.append(rtc_temp_c)
                temp_values.append(temp_c)

    print(f"Converted {record_count} samples")
    print(f"CSV output: {output_file}")

    if make_plot:
        create_plot(times, vbat_values, rtc_temp_values, temp_values, plot_file)


def create_plot(times, vbat_values, temp_values, plot_file):
    try:
        import plotly.graph_objects as go
        from plotly.subplots import make_subplots
    except ImportError:
        raise RuntimeError(
            "Plotly is required to create plots. " "Install it with: pip install plotly"
        )

    fig = make_subplots(
        rows=2,
        cols=1,
        shared_xaxes=True,
        vertical_spacing=0.08,
        subplot_titles=("Battery Voltage", "Temperature"),
    )

    fig.add_trace(
        go.Scatter(
            x=times,
            y=vbat_values,
            mode="lines",
            name="Battery Voltage",
            line=dict(width=1),
        ),
        row=1,
        col=1,
    )

    fig.add_trace(
        go.Scatter(
            x=times,
            y=temp_values,
            mode="lines",
            name="Temperature",
            line=dict(width=1),
        ),
        row=2,
        col=1,
    )

    fig.update_yaxes(
        title_text="Battery voltage (mV)",
        row=1,
        col=1,
    )

    fig.update_yaxes(
        title_text="Temperature (°C)",
        row=2,
        col=1,
    )

    fig.update_xaxes(
        title_text="Time (UTC)",
        row=2,
        col=1,
    )

    fig.update_layout(
        template="plotly_white",
        title="Sensor Time Series",
        hovermode="x unified",
        height=800,
    )

    fig.write_html(
        plot_file,
        include_plotlyjs=True,
        full_html=True,
    )

    print(f"Plot output: {plot_file}")


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Convert binary storeSample records to CSV, "
            "optionally generating an interactive Plotly plot."
        )
    )

    parser.add_argument(
        "binary_file",
        help="Input binary file",
    )

    parser.add_argument(
        "-o",
        "--output",
        help="Output CSV file (default: input filename with .csv extension)",
    )

    parser.add_argument(
        "--plot",
        action="store_true",
        help="Generate an interactive Plotly HTML time-series plot",
    )

    parser.add_argument(
        "--plot-output",
        help=(
            "Output HTML plot filename "
            "(default: input filename with .html extension)"
        ),
    )

    args = parser.parse_args()

    input_path = Path(args.binary_file)
    output_path = args.output or str(input_path.with_suffix(".csv"))
    plot_path = args.plot_output or str(input_path.with_suffix(".html"))

    try:
        convert_file(
            input_path,
            output_path,
            make_plot=args.plot,
            plot_file=plot_path,
        )
    except (OSError, ValueError, OverflowError, RuntimeError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
