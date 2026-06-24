#!/usr/bin/env python3
"""
Verify that a pump card consumes its queued actions from FPGA SYNC edges.

Usage:
    python pump_sync_scope_test.py COM7
"""

from __future__ import annotations

import argparse
import struct
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
    build_action,
    build_upload_event,
    expect_ack,
    get_card_inventory,
    gpio_pulse_action,
    gpio_stop_action,
    print_card_inventory,
    send_and_collect,
)

MODULE_PUMP_PERISTALTIC = 0x01
PUMP_SET_STATE = 0x01
CARD_TYPE_PUMP_PERISTALTIC = 0x01
CARD_TYPE_FPGA = 0x02


def find_first_card_slot(inventory, card_type: int) -> int | None:
    for entry in inventory:
        if entry.present and entry.card_type == card_type:
            return entry.slot
    return None


def pump_set_state_action(module_id: int, enable: bool, direction: bool, flow_nl_min: int) -> bytes:
    payload = struct.pack("<BBHI", int(enable), int(direction), 0, flow_nl_min)
    return build_action(MODULE_PUMP_PERISTALTIC, module_id, PUMP_SET_STATE, payload)


def upload_event(
    port: serial.Serial,
    reader: FrameReader,
    seq: int,
    event_id: int,
    timestamp_us: int,
    actions: list[bytes],
    label: str,
) -> int:
    print(f"  - {timestamp_us / 1_000_000:.3f}s: {label}")
    payload = build_upload_event(event_id, timestamp_us, actions)
    expect_ack(send_and_collect(port, reader, MSG_UPLOAD_EVENT, seq, payload), f"upload event {event_id}")
    time.sleep(INTER_COMMAND_DELAY_S)
    return seq + 1


def run_sync_test(port_name: str, local_pump: int) -> int:
    if local_pump < 0 or local_pump > 7:
        raise ValueError("local_pump must be in the range 0..7")

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

        inventory, seq = get_card_inventory(port, reader, seq)
        print_card_inventory(inventory)

        fpga_slot = find_first_card_slot(inventory, CARD_TYPE_FPGA)
        pump_slot = find_first_card_slot(inventory, CARD_TYPE_PUMP_PERISTALTIC)
        if fpga_slot is None:
            raise RuntimeError("no FPGA timing card found")
        if pump_slot is None:
            raise RuntimeError("no pump card found")

        pump_module_id = pump_slot * 8 + local_pump
        print(f"Using FPGA timing card in slot {fpga_slot}")
        print(f"Using pump card slot {pump_slot}, local pump {local_pump}, module_id {pump_module_id}")
        time.sleep(INTER_COMMAND_DELAY_S)

        expect_ack(send_and_collect(port, reader, MSG_CLEAR_SCHEDULE, seq), "clear before upload")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        print("Uploading synchronized pump/FPGA schedule:")
        seq = upload_event(
            port,
            reader,
            seq,
            1,
            500_000,
            [gpio_pulse_action(0), pump_set_state_action(pump_module_id, True, False, 200_000)],
            "TP0 pulse; pump enable forward at about 1.0 V DAC",
        )
        seq = upload_event(
            port,
            reader,
            seq,
            2,
            1_500_000,
            [gpio_pulse_action(1), pump_set_state_action(pump_module_id, True, True, 600_000)],
            "TP1 pulse; pump stays enabled, direction flips, about 3.0 V DAC",
        )
        seq = upload_event(
            port,
            reader,
            seq,
            3,
            2_500_000,
            [gpio_stop_action(0), pump_set_state_action(pump_module_id, False, True, 0)],
            "TP0 stop; pump disables, DAC returns to 0 V",
        )
        seq = upload_event(
            port,
            reader,
            seq,
            4,
            3_500_000,
            [gpio_stop_action(1)],
            "TP1 stop; no pump action, verifies no extra pump step is required",
        )

        expect_ack(send_and_collect(port, reader, MSG_GET_STATUS, seq, expected_frames=2), "status before start")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        print("Starting schedule...")
        expect_ack(send_and_collect(port, reader, MSG_START_SCHEDULE, seq, timeout_s=3.0), "start schedule")
        seq += 1

        print("Polling until the 3.5s pattern completes:")
        for _ in range(5):
            time.sleep(1.0)
            expect_ack(send_and_collect(port, reader, MSG_GET_STATUS, seq, expected_frames=2), f"status poll {seq}")
            seq += 1

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a pump SYNC scope test over USB CDC.")
    parser.add_argument("port", help="Serial port, e.g. COM7")
    parser.add_argument("--local-pump", type=int, default=0, help="Pump index on the discovered card, 0..7")
    args = parser.parse_args()
    return run_sync_test(args.port, args.local_pump)


if __name__ == "__main__":
    raise SystemExit(main())
