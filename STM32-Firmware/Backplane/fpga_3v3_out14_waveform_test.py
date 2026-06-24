#!/usr/bin/env python3
"""
Drive simple FPGA PWM waveforms on selected out_3v3 pins over USB CDC.

Usage:
    python fpga_3v3_out14_waveform_test.py COM7
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
    gpio_stop_action,
    gpio_waveform_action,
    send_and_collect,
)

OUT_3V3_PINS = (2, 4, 6, 8, 10, 12, 14)
OUT_3V3_CHANNELS = tuple(16 + pin for pin in OUT_3V3_PINS)


def upload_event(port: serial.Serial, reader: FrameReader, seq: int, event_id: int, timestamp_us: int, actions: list[bytes], label: str) -> int:
    print(f"  - {timestamp_us / 1_000_000:.3f}s: {label}")
    payload = build_upload_event(event_id, timestamp_us, actions)
    expect_ack(send_and_collect(port, reader, MSG_UPLOAD_EVENT, seq, payload), f"upload event {event_id}")
    time.sleep(INTER_COMMAND_DELAY_S)
    return seq + 1


def run_waveform_test(port_name: str) -> int:
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

        waveform_actions = [
            gpio_waveform_action(channel, frequency_millihz=2000, duty_ppm=500_000)
            for channel in OUT_3V3_CHANNELS
        ]
        stop_actions = [
            gpio_stop_action(channel)
            for channel in OUT_3V3_CHANNELS
        ]
        pin_list = ", ".join(f"out_3v3[{pin}]" for pin in OUT_3V3_PINS)

        print(f"Uploading waveform schedule for {pin_list}:")
        seq = upload_event(
            port,
            reader,
            seq,
            1,
            500_000,
            waveform_actions,
            f"{pin_list}: 2 Hz, 50% PWM",
        )
        seq = upload_event(
            port,
            reader,
            seq,
            2,
            6_500_000,
            stop_actions,
            f"{pin_list}: stop",
        )

        expect_ack(send_and_collect(port, reader, MSG_GET_STATUS, seq, expected_frames=2), "status before start")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        print("Starting schedule...")
        expect_ack(send_and_collect(port, reader, MSG_START_SCHEDULE, seq, timeout_s=3.0), "start schedule")
        seq += 1

        print("Polling until the 6.5s waveform completes:")
        for _ in range(7):
            time.sleep(1.0)
            expect_ack(send_and_collect(port, reader, MSG_GET_STATUS, seq, expected_frames=2), f"status poll {seq}")
            seq += 1

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run simple FPGA waveforms on selected out_3v3 pins.")
    parser.add_argument("port", help="Serial port, e.g. COM7")
    args = parser.parse_args()
    return run_waveform_test(args.port)


if __name__ == "__main__":
    raise SystemExit(main())
