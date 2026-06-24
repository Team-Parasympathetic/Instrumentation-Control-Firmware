#!/usr/bin/env python3
"""
Upload a tiny FPGA GPIO scope pattern over the normal USB CDC schedule path.

Usage:
    python fpga_gpio_scope_test.py COM7
"""

from __future__ import annotations

import argparse
import time

import serial

from fpga_mock_schedule import (
    INTER_COMMAND_DELAY_S,
    MSG_CLEAR_SCHEDULE,
    MSG_GET_STATUS,
    MSG_PING,
    MSG_START_SCHEDULE,
    MSG_UPLOAD_EVENT,
    FrameReader,
    build_upload_event,
    discover_fpga_card_slot,
    expect_ack,
    gpio_pulse_action,
    gpio_stop_action,
    gpio_waveform_action,
    send_and_collect,
)


def upload_event(port: serial.Serial, reader: FrameReader, seq: int, event_id: int, timestamp_us: int, actions: list[bytes], label: str) -> int:
    print(f"  - {timestamp_us / 1_000_000:.3f}s: {label}")
    payload = build_upload_event(event_id, timestamp_us, actions)
    frames = send_and_collect(port, reader, MSG_UPLOAD_EVENT, seq, payload)
    expect_ack(frames, f"upload event {event_id}")
    time.sleep(INTER_COMMAND_DELAY_S)
    return seq + 1


def run_scope_test(port_name: str) -> int:
    seq = 1
    reader = FrameReader()

    with serial.Serial(port_name, 115200, timeout=0.1) as port:
        time.sleep(0.2)

        expect_ack(send_and_collect(port, reader, MSG_PING, seq), "ping")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        expect_ack(send_and_collect(port, reader, MSG_CLEAR_SCHEDULE, seq), "clear schedule")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        slot, seq = discover_fpga_card_slot(port, reader, seq)
        print(f"Using FPGA timing card in slot {slot}")
        time.sleep(INTER_COMMAND_DELAY_S)

        expect_ack(send_and_collect(port, reader, MSG_CLEAR_SCHEDULE, seq), "clear before upload")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        print("Uploading GPIO scope schedule:")
        seq = upload_event(port, reader, seq, 1, 500_000, [gpio_pulse_action(0)], "TP0/CH0 pulse")
        seq = upload_event(port, reader, seq, 2, 1_000_000, [gpio_pulse_action(1)], "TP1/CH1 pulse")
        seq = upload_event(port, reader, seq, 3, 1_500_000, [gpio_pulse_action(2)], "TP2/CH2 pulse")
        seq = upload_event(port, reader, seq, 4, 2_000_000, [gpio_stop_action(0)], "TP0/CH0 stop")
        seq = upload_event(port, reader, seq, 5, 2_500_000, [gpio_stop_action(1)], "TP1/CH1 stop")
        seq = upload_event(port, reader, seq, 6, 3_000_000, [gpio_stop_action(2)], "TP2/CH2 stop")
        seq = upload_event(
            port,
            reader,
            seq,
            7,
            3_500_000,
            [gpio_waveform_action(3, frequency_millihz=2000, duty_ppm=500_000)],
            "TP3/CH3 PWM 2 Hz, 50%",
        )
        seq = upload_event(port, reader, seq, 8, 6_500_000, [gpio_stop_action(3)], "TP3/CH3 stop low")

        expect_ack(send_and_collect(port, reader, MSG_GET_STATUS, seq, expected_frames=2), "status before start")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        print("Starting schedule...")
        expect_ack(send_and_collect(port, reader, MSG_START_SCHEDULE, seq, timeout_s=3.0), "start schedule")
        seq += 1

        print("Polling until the 6.5s pattern completes:")
        for _ in range(8):
            time.sleep(1.0)
            expect_ack(send_and_collect(port, reader, MSG_GET_STATUS, seq, expected_frames=2), f"status poll {seq}")
            seq += 1

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a simple FPGA GPIO scope pattern over USB CDC.")
    parser.add_argument("port", help="Serial port, e.g. COM7")
    args = parser.parse_args()
    return run_scope_test(args.port)


if __name__ == "__main__":
    raise SystemExit(main())
