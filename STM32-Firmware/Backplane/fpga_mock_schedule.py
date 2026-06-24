#!/usr/bin/env python3
"""
Upload an FPGA-focused mock schedule to the STM32 backplane over USB CDC.

This exercises the real path:
    PC -> USB CDC -> backplane scheduler -> SPI register writes -> FPGA card

Usage:
    python fpga_mock_schedule.py COM7
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from dataclasses import dataclass

import serial

SOF = 0xA5
VERSION = 0x01
MAX_PAYLOAD = 256

MSG_PING = 0x01
MSG_ACK = 0x02
MSG_ERROR = 0x03
MSG_CLEAR_SCHEDULE = 0x10
MSG_UPLOAD_EVENT = 0x11
MSG_START_SCHEDULE = 0x12
MSG_STOP_SCHEDULE = 0x13
MSG_GET_STATUS = 0x14
MSG_GET_CARD_INVENTORY = 0x15

MODULE_GPIO_FPGA = 0x02
GPIO_SET_WAVEFORM = 0x01
GPIO_PULSE = 0x02
GPIO_STOP = 0x03
GPIO_MIRROR_SYNC = 0x04

CARD_TYPE_FPGA = 0x02
CARD_INVENTORY_ENTRY_SIZE = 7
CARD_INVENTORY_RETRIES = 3
CARD_INVENTORY_RETRY_DELAY_S = 0.2
INTER_COMMAND_DELAY_S = 0.05
START_SCHEDULE_TIMEOUT_S = 3.0
FPGA_PWM_CLOCK_HZ = 10_000_000
FPGA_PWM_FREQ_SCALE = 1_000
FPGA_PWM_DUTY_SCALE = 1_000_000
UINT32_MAX = 0xFFFFFFFF


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")

    body = struct.pack("<BBHH", VERSION, msg_type, seq, len(payload)) + payload
    return bytes([SOF]) + body + struct.pack("<H", crc16_ccitt_false(body))


@dataclass
class Frame:
    msg_type: int
    seq: int
    payload: bytes


@dataclass
class MockEvent:
    event_id: int
    timestamp_us: int
    description: str
    actions: list[bytes]


@dataclass
class CardInventoryEntry:
    slot: int
    present: int
    card_type: int
    firmware_major: int
    firmware_minor: int
    capabilities: int
    max_local_events: int


class FrameReader:
    def __init__(self) -> None:
        self.buffer = bytearray()

    def feed(self, data: bytes) -> list[Frame]:
        self.buffer.extend(data)
        frames: list[Frame] = []

        while True:
            try:
                sof_index = self.buffer.index(SOF)
            except ValueError:
                self.buffer.clear()
                break

            if sof_index:
                del self.buffer[:sof_index]

            if len(self.buffer) < 9:
                break

            version = self.buffer[1]
            msg_type = self.buffer[2]
            seq = struct.unpack_from("<H", self.buffer, 3)[0]
            payload_len = struct.unpack_from("<H", self.buffer, 5)[0]

            if version != VERSION or payload_len > MAX_PAYLOAD:
                del self.buffer[0]
                continue

            frame_len = 9 + payload_len
            if len(self.buffer) < frame_len:
                break

            payload = bytes(self.buffer[7: 7 + payload_len])
            crc_rx = struct.unpack_from("<H", self.buffer, 7 + payload_len)[0]
            crc_calc = crc16_ccitt_false(bytes(self.buffer[1: 7 + payload_len]))

            if crc_rx == crc_calc:
                frames.append(Frame(msg_type=msg_type, seq=seq, payload=payload))

            del self.buffer[:frame_len]

        return frames


def build_upload_event(event_id: int, timestamp_us: int, actions: list[bytes]) -> bytes:
    return struct.pack("<IQB", event_id, timestamp_us, len(actions)) + b"".join(actions)


def build_action(module_type: int, module_id: int, action_type: int, action_payload: bytes) -> bytes:
    return struct.pack("<BBBB", module_type, module_id, action_type, len(action_payload)) + action_payload


def gpio_phase_step_from_millihz(frequency_millihz: int) -> int:
    if frequency_millihz < 0:
        raise ValueError("frequency_millihz must be non-negative")

    denominator = FPGA_PWM_CLOCK_HZ * FPGA_PWM_FREQ_SCALE
    phase_step = ((frequency_millihz << 32) + (denominator // 2)) // denominator
    if phase_step > UINT32_MAX:
        raise ValueError("frequency_millihz is too high for a 32-bit FPGA phase step")

    return phase_step


def gpio_duty_threshold_from_ppm(duty_ppm: int) -> int:
    if duty_ppm < 0 or duty_ppm > FPGA_PWM_DUTY_SCALE:
        raise ValueError("duty_ppm must be in the range 0..1_000_000")

    if duty_ppm == FPGA_PWM_DUTY_SCALE:
        return UINT32_MAX

    return (duty_ppm << 32) // FPGA_PWM_DUTY_SCALE


def gpio_waveform_action(
    channel_id: int,
    frequency_millihz: int,
    duty_ppm: int,
    polarity: int = 0,
    idle_state: int = 0,
    num_periods: int = 0,
) -> bytes:
    phase_step = gpio_phase_step_from_millihz(frequency_millihz)
    duty_threshold = gpio_duty_threshold_from_ppm(duty_ppm)
    payload = struct.pack("<BBHIII", polarity, idle_state, 0, phase_step, duty_threshold, num_periods)
    return build_action(MODULE_GPIO_FPGA, channel_id, GPIO_SET_WAVEFORM, payload)


def gpio_pulse_action(channel_id: int) -> bytes:
    return build_action(MODULE_GPIO_FPGA, channel_id, GPIO_PULSE, b"")


def gpio_stop_action(channel_id: int) -> bytes:
    return build_action(MODULE_GPIO_FPGA, channel_id, GPIO_STOP, b"")


def decode_frame(frame: Frame) -> str:
    if frame.msg_type == MSG_ACK and len(frame.payload) == 3:
        ack_seq, status = struct.unpack("<HB", frame.payload)
        return f"ACK seq={frame.seq} ack_seq={ack_seq} status=0x{status:02X}"

    if frame.msg_type == MSG_ERROR and len(frame.payload) == 4:
        failed_seq, error_code, detail = struct.unpack("<HBB", frame.payload)
        return f"ERROR seq={frame.seq} failed_seq={failed_seq} code=0x{error_code:02X} detail=0x{detail:02X}"

    if frame.msg_type == MSG_GET_STATUS and len(frame.payload) == 16:
        state, last_error, event_count, last_event_id, current_time_us = struct.unpack("<BBHIQ", frame.payload)
        return (
            "STATUS "
            f"seq={frame.seq} state={state} last_error=0x{last_error:02X} "
            f"event_count={event_count} last_event_id={last_event_id} time_us={current_time_us}"
        )

    if frame.msg_type == MSG_GET_CARD_INVENTORY:
        inventory = parse_card_inventory_payload(frame.payload)
        if inventory is not None:
            summary = ", ".join(
                f"slot {entry.slot}: type=0x{entry.card_type:02X} fw={entry.firmware_major}.{entry.firmware_minor}"
                if entry.present
                else f"slot {entry.slot}: empty"
                for entry in inventory
            )
            return f"INVENTORY seq={frame.seq} {summary}"

    return f"FRAME type=0x{frame.msg_type:02X} seq={frame.seq} payload={frame.payload.hex()}"


def frame_is_ack(frame: Frame) -> bool:
    return frame.msg_type == MSG_ACK and len(frame.payload) == 3 and frame.payload[2] == 0x00


def parse_card_inventory_payload(payload: bytes) -> list[CardInventoryEntry] | None:
    if not payload:
        return None

    slot_count = payload[0]
    expected_len = 1 + (slot_count * CARD_INVENTORY_ENTRY_SIZE)
    if len(payload) != expected_len:
        return None

    inventory: list[CardInventoryEntry] = []
    offset = 1
    for slot in range(slot_count):
        present = payload[offset + 0]
        card_type = payload[offset + 1]
        firmware_major = payload[offset + 2]
        firmware_minor = payload[offset + 3]
        capabilities = struct.unpack_from("<H", payload, offset + 4)[0]
        max_local_events = payload[offset + 6]
        inventory.append(
            CardInventoryEntry(
                slot=slot,
                present=present,
                card_type=card_type,
                firmware_major=firmware_major,
                firmware_minor=firmware_minor,
                capabilities=capabilities,
                max_local_events=max_local_events,
            )
        )
        offset += CARD_INVENTORY_ENTRY_SIZE

    return inventory


def find_first_fpga_card_slot(inventory: list[CardInventoryEntry]) -> int | None:
    for entry in inventory:
        if entry.present and entry.card_type == CARD_TYPE_FPGA:
            return entry.slot
    return None


def print_card_inventory(inventory: list[CardInventoryEntry]) -> None:
    print("Discovered backplane cards:")
    for entry in inventory:
        if entry.present:
            print(
                "  "
                f"slot {entry.slot}: card_type=0x{entry.card_type:02X} "
                f"fw={entry.firmware_major}.{entry.firmware_minor} "
                f"caps=0x{entry.capabilities:04X} max_local_events={entry.max_local_events}"
            )
        else:
            print(f"  slot {entry.slot}: empty")


def send_and_collect(
    port: serial.Serial,
    reader: FrameReader,
    msg_type: int,
    seq: int,
    payload: bytes = b"",
    expected_frames: int = 1,
    timeout_s: float = 1.0,
) -> list[Frame]:
    port.write(build_frame(msg_type, seq, payload))
    deadline = time.time() + timeout_s
    frames: list[Frame] = []

    while time.time() < deadline and len(frames) < expected_frames:
        data = port.read(port.in_waiting or 1)
        if not data:
            continue

        for frame in reader.feed(data):
            if frame.seq == seq:
                frames.append(frame)
                print(decode_frame(frame))

    return frames


def expect_ack(frames: list[Frame], context: str) -> None:
    for frame in frames:
        if frame_is_ack(frame):
            return

    details = ", ".join(decode_frame(frame) for frame in frames) or "no response"
    raise RuntimeError(f"{context} failed: {details}")


def get_card_inventory(port: serial.Serial, reader: FrameReader, seq: int) -> tuple[list[CardInventoryEntry], int]:
    frames = send_and_collect(port, reader, MSG_GET_CARD_INVENTORY, seq, expected_frames=2)
    expect_ack(frames, "get card inventory")

    for frame in frames:
        if frame.msg_type == MSG_GET_CARD_INVENTORY:
            inventory = parse_card_inventory_payload(frame.payload)
            if inventory is None:
                raise RuntimeError("received malformed card inventory payload")
            return inventory, seq + 1

    raise RuntimeError("card inventory response missing after ACK")


def discover_fpga_card_slot(port: serial.Serial, reader: FrameReader, seq: int) -> tuple[int, int]:
    attempt = 0

    while attempt < CARD_INVENTORY_RETRIES:
        inventory, seq = get_card_inventory(port, reader, seq)
        print_card_inventory(inventory)

        slot = find_first_fpga_card_slot(inventory)
        if slot is not None:
            return slot, seq

        attempt += 1
        if attempt < CARD_INVENTORY_RETRIES:
            print(
                "No FPGA timing card found in inventory; "
                f"retrying discovery in {CARD_INVENTORY_RETRY_DELAY_S:.1f}s..."
            )
            time.sleep(CARD_INVENTORY_RETRY_DELAY_S)

    raise RuntimeError("no FPGA timing card was discovered by the backplane")


def build_mock_schedule() -> list[MockEvent]:
    return [
        MockEvent(
            event_id=1,
            timestamp_us=500_000,
            description="t=0.5s: channel 0 PULSE",
            actions=[gpio_pulse_action(0)],
        ),
        MockEvent(
            event_id=2,
            timestamp_us=1_500_000,
            description="t=1.5s: channel 0 STOP",
            actions=[gpio_stop_action(0)],
        ),
        MockEvent(
            event_id=3,
            timestamp_us=2_500_000,
            description="t=2.5s: channel 0 PWM, 1000 mHz, 50% duty",
            actions=[gpio_waveform_action(0, frequency_millihz=1000, duty_ppm=500_000)],
        ),
        MockEvent(
            event_id=4,
            timestamp_us=4_000_000,
            description="t=4.0s: channel 0 STOP",
            actions=[gpio_stop_action(0)],
        ),
        MockEvent(
            event_id=5,
            timestamp_us=5_000_000,
            description="t=5.0s: channel 1 PULSE",
            actions=[gpio_pulse_action(1)],
        ),
        MockEvent(
            event_id=6,
            timestamp_us=6_500_000,
            description="t=6.5s: channel 1 STOP",
            actions=[gpio_stop_action(1)],
        ),
    ]


def upload_mock_schedule(port_name: str) -> int:
    seq = 1
    reader = FrameReader()

    with serial.Serial(port_name, 115200, timeout=0.1) as port:
        time.sleep(0.2)

        expect_ack(send_and_collect(port, reader, MSG_PING, seq), "ping")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        expect_ack(send_and_collect(port, reader, MSG_CLEAR_SCHEDULE, seq), "initial clear")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        slot, seq = discover_fpga_card_slot(port, reader, seq)
        time.sleep(INTER_COMMAND_DELAY_S)

        print(f"Using discovered FPGA timing card slot {slot}")

        expect_ack(send_and_collect(port, reader, MSG_CLEAR_SCHEDULE, seq), "clear before upload")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        mock_events = build_mock_schedule()
        print("Uploading mock FPGA events:")
        for event in mock_events:
            print(f"  - {event.description}")
            payload = build_upload_event(event.event_id, event.timestamp_us, event.actions)
            expect_ack(send_and_collect(port, reader, MSG_UPLOAD_EVENT, seq, payload), f"upload event {event.event_id}")
            seq += 1
            time.sleep(INTER_COMMAND_DELAY_S)

        expect_ack(send_and_collect(port, reader, MSG_GET_STATUS, seq, expected_frames=2), "status before start")
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        expect_ack(
            send_and_collect(port, reader, MSG_START_SCHEDULE, seq, timeout_s=START_SCHEDULE_TIMEOUT_S),
            "start schedule",
        )
        seq += 1
        time.sleep(INTER_COMMAND_DELAY_S)

        print("Schedule started. Polling status while events execute...")
        for _ in range(8):
            time.sleep(1.0)
            expect_ack(send_and_collect(port, reader, MSG_GET_STATUS, seq, expected_frames=2), f"status poll seq {seq}")
            seq += 1
            time.sleep(INTER_COMMAND_DELAY_S)

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Upload an FPGA-card mock schedule over USB CDC.")
    parser.add_argument("port", help="Serial port, e.g. COM7")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    return upload_mock_schedule(args.port)


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except Exception as exc:  # pragma: no cover
        print(f"ERROR: {exc}")
        raise SystemExit(1)
