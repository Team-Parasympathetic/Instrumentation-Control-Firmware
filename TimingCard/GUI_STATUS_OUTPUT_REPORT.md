# Timing Card USB Interface and Status Output Report

## Scope

This report describes the current Backplane USB CDC protocol surface and the GUI changes needed to configure a Timing Card output as a hardware status output. The status output is implemented as a GPIO channel mode that mirrors the FPGA schedule `sync_state`, which toggles once per timing event edge.

The existing clock pin warning for `clk` on `PT18B / PCLKC0_1` is a hardware revision item. The firmware/FPGA code remains usable as-is for now.

## USB CDC Frame Format

All host-to-backplane and backplane-to-host messages use the existing framed binary protocol:

```text
[SOF][VERSION][MSG_TYPE][SEQ_L][SEQ_H][LEN_L][LEN_H][PAYLOAD...][CRC_L][CRC_H]
```

Fields:

- `SOF`: `0xA5`
- `VERSION`: `0x01`
- `SEQ`: little-endian `uint16`
- `LEN`: little-endian payload length
- `CRC`: CRC-16/CCITT-FALSE, init `0xFFFF`, polynomial `0x1021`, over `VERSION` through the last payload byte
- Maximum payload: 256 bytes

Every valid command returns either:

- `MSG_ACK` (`0x02`) with payload `[ack_seq:u16le][status:u8]`
- `MSG_ERROR` (`0x03`) with payload `[failed_seq:u16le][error_code:u8][detail:u8]`

For query commands, the backplane sends the ACK first, then a second response frame using the original query message type and sequence number.

## USB Message Types

Current command set:

```text
0x01 MSG_PING
0x10 MSG_CLEAR_SCHEDULE
0x11 MSG_UPLOAD_EVENT
0x12 MSG_START_SCHEDULE
0x13 MSG_STOP_SCHEDULE
0x14 MSG_GET_STATUS
0x15 MSG_GET_CARD_INVENTORY
```

Important GUI behavior:

- Treat `MSG_START_SCHEDULE` ACK as "schedule accepted and prepared", not as "first event has already occurred".
- The firmware now waits 2 seconds after schedule upload/preload before arming pump cards and the Timing Card.
- During this pending-start interval, additional schedule mutation commands are rejected as busy.
- The GUI should poll `MSG_GET_STATUS` after `MSG_START_SCHEDULE` and only show the schedule as actively running once `scheduler_state == 2`.

## Status Response

`MSG_GET_STATUS` returns ACK, then a `MSG_GET_STATUS` data frame with a 16-byte payload:

```text
offset  size  field
0       1     scheduler_state
1       1     last_error
2       2     event_count:u16le
4       4     last_event_id:u32le
8       8     current_time_us:u64le
```

Scheduler states:

```text
0 SCHED_IDLE
1 SCHED_LOADED
2 SCHED_RUNNING
3 SCHED_STOPPED
4 SCHED_ERROR
```

After `MSG_START_SCHEDULE`, the expected GUI-visible sequence is:

```text
START_SCHEDULE -> ACK OK
GET_STATUS     -> ACK OK + state SCHED_LOADED during preload delay
GET_STATUS     -> ACK OK + state SCHED_RUNNING after the 2 s delay expires
GET_STATUS     -> ACK OK + state SCHED_STOPPED when complete
```

If the delayed arm fails, status moves to `SCHED_ERROR` and `last_error` records the protocol error code.

## Card Inventory Response

`MSG_GET_CARD_INVENTORY` returns ACK, then a `MSG_GET_CARD_INVENTORY` data frame.

Payload:

```text
offset  size  field
0       1     slot_count
1       7     slot 0 entry
8       7     slot 1 entry
...
```

Each 7-byte slot entry:

```text
offset  size  field
0       1     present
1       1     card_type
2       1     firmware_major
3       1     firmware_minor
4       2     capabilities:u16le
6       1     max_local_events
```

Known card types:

```text
0x00 CARD_TYPE_NONE
0x01 CARD_TYPE_PUMP_PERISTALTIC
0x02 CARD_TYPE_FPGA_GPIO_SYNC
```

The Timing Card reports:

```text
card_type        = 0x02
firmware         = 1.0
capabilities     = 0x0007
max_local_events = 48
```

Timing Card capability bits:

```text
0x0001 CARD_CAP_FPGA_GPIO_3V3_16
0x0002 CARD_CAP_FPGA_GPIO_5V_16
0x0004 CARD_CAP_FPGA_SYNC_MASTER
```

GUI inventory parser note: the inventory payload is currently `1 + slot_count * 7` bytes. With 8 slots, expect 57 bytes.

## Schedule Upload Payload

`MSG_UPLOAD_EVENT` payload:

```text
offset  size  field
0       4     event_id:u32le
4       8     timestamp_us:u64le
12      1     action_count
13      N     action records
```

Each action record:

```text
offset  size  field
0       1     module_type
1       1     module_id
2       1     action_type
3       1     action_len
4       N     action payload
```

Limits:

- Maximum events per schedule: 48
- Maximum action bytes per event: 192
- Event timestamps must be strictly increasing
- Minimum event spacing: 10,000 us

## Timing Card GPIO Action Types

For Timing Card GPIO actions:

```text
module_type = 0x02 MODULE_GPIO_FPGA
module_id   = output channel, 0..31
```

Channel mapping:

```text
0..15   out_5v[0..15]
16..31  out_3v3[0..15]
```

Current GPIO action types:

```text
0x01 GPIO_SET_WAVEFORM
0x02 GPIO_PULSE
0x03 GPIO_STOP
0x04 GPIO_MIRROR_SYNC
```

`GPIO_SET_WAVEFORM` payload is 16 bytes:

```text
offset  size  field
0       1     polarity_invert, 0 or 1
1       1     idle_high, 0 or 1
2       2     reserved, write 0
4       4     phase_step:u32le
8       4     duty_threshold:u32le
12      4     reserved, write 0
```

`GPIO_PULSE`, `GPIO_STOP`, and `GPIO_MIRROR_SYNC` all have zero-length payloads.

`GPIO_PULSE` drives the selected channel high until a later `GPIO_STOP` action drives it back to its idle state. For block-based GUI output, emit `GPIO_PULSE` at the block start timestamp and `GPIO_STOP` at the block stop timestamp. The old externally-visible `GPIO_FORCE_LOW` and `GPIO_FORCE_HIGH` action records are retired; internally, the FPGA still uses force modes as implementation primitives.

## Configuring Hardware Status Output

The new status-output mode is:

```text
action_type = 0x04 GPIO_MIRROR_SYNC
action_len  = 0
payload     = empty
```

To configure a Timing Card output as the status output, the GUI should add one GPIO action to the schedule:

```text
module_type = 0x02 MODULE_GPIO_FPGA
module_id   = selected output channel, 0..31
action_type = 0x04 GPIO_MIRROR_SYNC
action_len  = 0
```

Recommended placement:

- Put the `GPIO_MIRROR_SYNC` action in the first uploaded timing event.
- Do not schedule any other action on that same output channel unless the user intentionally wants to stop using it as the status output.
- Do not generate the old status pattern by alternating force-high/force-low records; those external action records are obsolete.

Runtime behavior:

- `sync_state` resets to 0 when the FPGA queue is cleared.
- `sync_state` resets to 0 when a run is started.
- `sync_state` toggles on every FPGA event edge.
- The dedicated `sync` pin always outputs `sync_state`.
- Any GPIO channel configured as `GPIO_MIRROR_SYNC` mirrors `sync_state`.
- Schedule actions are preloaded before their event edge and committed on the event edge, so putting the mirror action in the first event is sufficient for the selected output to start mirroring at that first transition.

Current limitation:

- The USB schedule action exposes only non-inverted `GPIO_MIRROR_SYNC`.
- The FPGA mode bits can represent inverted mirror-sync internally, but the current GUI/backplane action does not provide a payload or flag for that. If inverted status output is needed later, add a small payload flag or a second action type.

## GUI Implementation Checklist

1. Update schedule action enums to include `GPIO_PULSE = 0x02`, `GPIO_STOP = 0x03`, and `GPIO_MIRROR_SYNC = 0x04`; remove `GPIO_FORCE_LOW` and `GPIO_FORCE_HIGH` from the GUI-facing API.
2. Allow the status-output control to select one Timing Card GPIO channel `0..31`.
3. Emit one zero-payload `GPIO_MIRROR_SYNC` action on that channel in the first timing event.
4. Prevent normal waveform/pulse/stop actions from reusing that channel while status output is enabled.
5. Treat `START_SCHEDULE` ACK as accepted/preparing.
6. Poll `GET_STATUS` until `scheduler_state == SCHED_RUNNING` before showing active execution.
7. Parse card inventory as 7 bytes per slot; Timing Card is `card_type = 0x02`, capabilities `0x0007`.
8. Continue normal request/response sequencing: wait for ACK or ERROR for each command before sending the next schedule command.

## Source References

- USB frame and message IDs: `STM32-Firmware/Backplane/Core/Inc/protocol.h`
- ACK/error frame payloads: `STM32-Firmware/Backplane/Core/Src/protocol.c`
- Start delay and query response behavior: `STM32-Firmware/Backplane/Core/Src/app.c`
- Schedule and GPIO action validation: `STM32-Firmware/Backplane/Core/Src/scheduler.c`
- GPIO action to FPGA register mapping: `STM32-Firmware/Backplane/Core/Src/backplane_fpga_bus.c`
- Timing Card register/mode constants: `STM32-Firmware/Backplane/Core/Inc/fpga_timing_regs.h`
- FPGA sync toggle and mirror-sync implementation: `TimingCard/timingcard_schedule_store.v`, `TimingCard/timingcard_pwm_channel.v`

## Hardware Revision Note

Current board routing places `clk` on `PT18B / PCLKC0_1`. That pin is clock-related, but for a single-ended MachXO2 clock input the direct primary-clock route is on the `PCLKT` side of the pair. For the next PCB revision, move the 10 MHz FPGA clock to `PT18A / PCLKT0_1` or another valid single-ended `PCLKT` primary clock input.
