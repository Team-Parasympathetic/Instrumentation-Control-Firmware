`include "fpga_timing_regs.vh"

module timingcard_schedule_store #(
    parameter [15:0]  MAX_EVENTS = 16'd48,
    parameter integer INDEX_WIDTH = 6,
    parameter integer TIMER_WIDTH = 48,
    parameter integer DUTY_WIDTH = 16
) (
    input  wire        clk,
    input  wire [15:0] write_addr,
    input  wire [15:0] write_addr_async,
    input  wire [31:0] write_data_async,
    input  wire        write_toggle_async,
    input  wire [15:0] read_addr,
    output reg  [31:0] read_data,
    output reg         read_valid,
    output reg         write_valid,
    output wire [31:0] channel_level_bits,
    output reg         sync_state
);

    localparam [2:0] RUN_IDLE = 3'd0;
    localparam [2:0] RUN_LOAD = 3'd1;
    localparam [2:0] RUN_PREP = 3'd2;
    localparam [2:0] RUN_WAIT = 3'd3;
    localparam [2:0] RUN_FINISH = 3'd4;
    localparam [INDEX_WIDTH-1:0] MAX_COUNT = MAX_EVENTS[INDEX_WIDTH-1:0];
    localparam [INDEX_WIDTH-1:0] ONE_INDEX = {{INDEX_WIDTH-1{1'b0}}, 1'b1};
    localparam [TIMER_WIDTH-1:0] ZERO_TIMER = {TIMER_WIDTH{1'b0}};
    localparam [TIMER_WIDTH-1:0] ONE_TIMER = {{TIMER_WIDTH-1{1'b0}}, 1'b1};
    localparam [TIMER_WIDTH-1:0] TWO_TIMER = {{TIMER_WIDTH-2{1'b0}}, 2'd2};
    localparam [15:0] GPIO_WINDOW_END = 16'h0500;

    reg [31:0] card_error_flags;
    reg        card_control_run;

    reg [2:0]             run_state;
    reg [INDEX_WIDTH-1:0] event_head;
    reg [INDEX_WIDTH-1:0] action_head;
    reg [INDEX_WIDTH-1:0] global_event_index;
    reg [TIMER_WIDTH-1:0] event_countdown;
    reg                   run_done;

    wire        cmd_valid;
    wire [15:0] cmd_addr;
    wire [31:0] cmd_data;

    wire wr_card_error_flags;
    wire wr_card_control;
    wire wr_event_time_lo;
    wire wr_event_time_hi;
    wire wr_event_push;
    wire wr_action_event_index;
    wire wr_action_meta;
    wire wr_action_phase_step;
    wire wr_action_duty_threshold;
    wire wr_action_push;

    wire queue_clear;
    wire cmd_clear_queue;
    wire cmd_start_run;

    wire [31:0] event_stage_time_lo;
    wire [31:0] event_stage_time_hi;
    wire [INDEX_WIDTH-1:0] event_count;
    wire        event_push_seen;
    wire [31:0] last_event_time_lo;
    wire [31:0] last_event_time_hi;
    wire [31:0] event_checksum;
    wire [TIMER_WIDTH-1:0] event_head_delta_ticks;

    wire [31:0] action_stage_event_index;
    wire [31:0] action_stage_meta;
    wire [31:0] action_stage_phase_step;
    wire [31:0] action_stage_duty_threshold;
    wire [INDEX_WIDTH-1:0] action_count;
    wire        action_push_seen;
    wire [31:0] last_action_event_index;
    wire [31:0] action_checksum;
    wire        action_head_valid;
    wire [INDEX_WIDTH-1:0] action_head_event_index;
    wire [4:0]  action_head_channel;
    wire [4:0]  action_head_control;
    wire [31:0] action_head_phase_step;
    wire [DUTY_WIDTH-1:0] action_head_duty_threshold;
    wire        action_head_phase_step_zero;
    wire        action_head_duty_threshold_full;

    wire        run_active;
    wire        action_matches_event;
    wire        preload_action;
    wire        event_due;
    wire [INDEX_WIDTH-1:0] event_head_next;

    wire        read_gpio_hit;
    wire [15:0] read_gpio_offset;
    wire [4:0]  read_gpio_index;
    wire [3:0]  read_gpio_field;
    wire        read_gpio_status_field;

    assign wr_card_error_flags = cmd_valid && (cmd_addr == `CARD_REG_ERROR_FLAGS);
    assign wr_card_control = cmd_valid && (cmd_addr == `CARD_REG_CONTROL);
    assign wr_event_time_lo = cmd_valid && (cmd_addr == `FPGA_EVENT_REG_TIME_LO);
    assign wr_event_time_hi = cmd_valid && (cmd_addr == `FPGA_EVENT_REG_TIME_HI);
    assign wr_event_push = cmd_valid && (cmd_addr == `FPGA_EVENT_REG_PUSH);
    assign wr_action_event_index = cmd_valid && (cmd_addr == `FPGA_ACTION_REG_EVENT_INDEX);
    assign wr_action_meta = cmd_valid && (cmd_addr == `FPGA_ACTION_REG_META);
    assign wr_action_phase_step = cmd_valid && (cmd_addr == `FPGA_ACTION_REG_PHASE_STEP);
    assign wr_action_duty_threshold = cmd_valid && (cmd_addr == `FPGA_ACTION_REG_DUTY_THRESHOLD);
    assign wr_action_push = cmd_valid && (cmd_addr == `FPGA_ACTION_REG_PUSH);

    assign cmd_clear_queue = wr_card_control && cmd_data[1];
    assign cmd_start_run = wr_card_control && cmd_data[0] && !cmd_data[1];
    assign queue_clear = cmd_clear_queue;

    assign run_active = (run_state != RUN_IDLE);
    assign action_matches_event = action_head_valid &&
                                  (action_head_event_index == global_event_index);
    assign preload_action = (run_state == RUN_PREP) && action_matches_event;
    assign event_due = (run_state == RUN_WAIT) && (event_countdown == ZERO_TIMER);
    assign event_head_next = event_head + ONE_INDEX;

    assign read_gpio_hit = (read_addr >= `FPGA_GPIO_BASE) && (read_addr < GPIO_WINDOW_END);
    assign read_gpio_offset = read_addr - `FPGA_GPIO_BASE;
    assign read_gpio_index = read_gpio_offset[8:4];
    assign read_gpio_field = read_addr[3:0];
    assign read_gpio_status_field = (read_gpio_field == 4'hC);

    timingcard_spi_write_cdc u_write_cdc (
        .clk               (clk),
        .write_addr_async  (write_addr_async),
        .write_data_async  (write_data_async),
        .write_toggle_async(write_toggle_async),
        .cmd_valid         (cmd_valid),
        .cmd_addr          (cmd_addr),
        .cmd_data          (cmd_data)
    );

    timingcard_event_queue #(
        .MAX_EVENTS (MAX_EVENTS),
        .INDEX_WIDTH(INDEX_WIDTH),
        .TIMER_WIDTH(TIMER_WIDTH)
    ) u_event_queue (
        .clk             (clk),
        .clear           (queue_clear),
        .write_time_lo   (wr_event_time_lo),
        .write_time_hi   (wr_event_time_hi),
        .push            (wr_event_push),
        .write_data      (cmd_data),
        .head_index      (event_head),
        .head_delta_ticks(event_head_delta_ticks),
        .stage_time_lo   (event_stage_time_lo),
        .stage_time_hi   (event_stage_time_hi),
        .count           (event_count),
        .push_seen       (event_push_seen),
        .last_time_lo    (last_event_time_lo),
        .last_time_hi    (last_event_time_hi),
        .checksum        (event_checksum)
    );

    timingcard_action_queue #(
        .MAX_EVENTS (MAX_EVENTS),
        .INDEX_WIDTH(INDEX_WIDTH),
        .DUTY_WIDTH (DUTY_WIDTH)
    ) u_action_queue (
        .clk                     (clk),
        .clear                   (queue_clear),
        .write_event_index       (wr_action_event_index),
        .write_meta              (wr_action_meta),
        .write_phase_step        (wr_action_phase_step),
        .write_duty_threshold    (wr_action_duty_threshold),
        .push                    (wr_action_push),
        .write_data              (cmd_data),
        .head_index              (action_head),
        .stage_event_index       (action_stage_event_index),
        .stage_meta              (action_stage_meta),
        .stage_phase_step        (action_stage_phase_step),
        .stage_duty_threshold    (action_stage_duty_threshold),
        .count                   (action_count),
        .push_seen               (action_push_seen),
        .last_event_index        (last_action_event_index),
        .checksum                (action_checksum),
        .head_valid              (action_head_valid),
        .head_event_index        (action_head_event_index),
        .head_channel            (action_head_channel),
        .head_control            (action_head_control),
        .head_phase_step         (action_head_phase_step),
        .head_duty_threshold     (action_head_duty_threshold),
        .head_phase_step_zero    (action_head_phase_step_zero),
        .head_duty_threshold_full(action_head_duty_threshold_full)
    );

    timingcard_gpio_bank #(
        .DUTY_WIDTH(DUTY_WIDTH)
    ) u_gpio_bank (
        .clk                         (clk),
        .clear                       (queue_clear),
        .preload_valid               (preload_action),
        .preload_channel             (action_head_channel),
        .preload_control             (action_head_control),
        .preload_phase_step          (action_head_phase_step),
        .preload_duty_threshold      (action_head_duty_threshold),
        .preload_phase_step_zero     (action_head_phase_step_zero),
        .preload_duty_threshold_full (action_head_duty_threshold_full),
        .commit_event                (event_due),
        .sync_state                  (sync_state),
        .channel_level_bits          (channel_level_bits)
    );

    initial begin
        card_error_flags = 32'd0;
        card_control_run = 1'b0;
        run_state = RUN_IDLE;
        event_head = {INDEX_WIDTH{1'b0}};
        action_head = {INDEX_WIDTH{1'b0}};
        global_event_index = {INDEX_WIDTH{1'b0}};
        event_countdown = ZERO_TIMER;
        run_done = 1'b0;
        sync_state = 1'b0;
    end

    always @* begin
        read_data = 32'd0;
        read_valid = 1'b1;

        case (read_addr)
            `CARD_REG_STATUS: begin
                read_data[0] = run_active;
                read_data[1] = run_done;
                read_data[2] = (event_count != {INDEX_WIDTH{1'b0}});
                read_data[3] = sync_state;
            end

            `CARD_REG_ERROR_FLAGS: begin
                read_data = card_error_flags;
            end

            `CARD_REG_CONTROL: begin
                read_data = {31'd0, card_control_run};
            end

            `FPGA_EVENT_REG_TIME_LO: begin
                read_data = event_stage_time_lo;
            end

            `FPGA_EVENT_REG_TIME_HI: begin
                read_data = event_stage_time_hi;
            end

            `FPGA_EVENT_REG_COUNT: begin
                read_data = {{(32-INDEX_WIDTH){1'b0}}, event_count};
            end

            `FPGA_EVENT_REG_STATUS: begin
                read_data[0] = (event_count >= MAX_COUNT);
                read_data[1] = event_push_seen;
                read_data[2] = run_active;
                read_data[3] = run_done;
            end

            `FPGA_EVENT_REG_LAST_TIME_LO: begin
                read_data = last_event_time_lo;
            end

            `FPGA_EVENT_REG_LAST_TIME_HI: begin
                read_data = last_event_time_hi;
            end

            `FPGA_EVENT_REG_CHECKSUM: begin
                read_data = event_checksum;
            end

            `FPGA_ACTION_REG_EVENT_INDEX: begin
                read_data = action_stage_event_index;
            end

            `FPGA_ACTION_REG_META: begin
                read_data = action_stage_meta;
            end

            `FPGA_ACTION_REG_PHASE_STEP: begin
                read_data = action_stage_phase_step;
            end

            `FPGA_ACTION_REG_DUTY_THRESHOLD: begin
                read_data = action_stage_duty_threshold;
            end

            `FPGA_ACTION_REG_COUNT: begin
                read_data = {{(32-INDEX_WIDTH){1'b0}}, action_count};
            end

            `FPGA_ACTION_REG_STATUS: begin
                read_data[0] = (action_count >= MAX_COUNT);
                read_data[1] = action_push_seen;
                read_data[2] = run_active;
                read_data[3] = run_done;
            end

            `FPGA_ACTION_REG_LAST_EVENT_INDEX: begin
                read_data = last_action_event_index;
            end

            `FPGA_ACTION_REG_CHECKSUM: begin
                read_data = action_checksum;
            end

            default: begin
                if (read_gpio_hit) begin
                    if (read_gpio_status_field) begin
                        read_valid = 1'b1;
                        read_data = {31'd0, channel_level_bits[read_gpio_index]};
                    end else begin
                        read_valid = 1'b0;
                    end
                end else begin
                    read_valid = 1'b0;
                end
            end
        endcase
    end

    always @* begin
        write_valid = 1'b1;

        case (write_addr)
            `CARD_REG_ERROR_FLAGS,
            `CARD_REG_CONTROL,
            `FPGA_EVENT_REG_TIME_LO,
            `FPGA_EVENT_REG_TIME_HI,
            `FPGA_EVENT_REG_PUSH,
            `FPGA_ACTION_REG_EVENT_INDEX,
            `FPGA_ACTION_REG_META,
            `FPGA_ACTION_REG_PHASE_STEP,
            `FPGA_ACTION_REG_DUTY_THRESHOLD,
            `FPGA_ACTION_REG_PUSH: begin
            end

            default: begin
                write_valid = 1'b0;
            end
        endcase
    end

    always @(posedge clk) begin
        if (wr_card_error_flags) begin
            card_error_flags <= cmd_data;
        end

        if (cmd_clear_queue) begin
            card_control_run <= 1'b0;
            run_state <= RUN_IDLE;
            event_head <= {INDEX_WIDTH{1'b0}};
            action_head <= {INDEX_WIDTH{1'b0}};
            global_event_index <= {INDEX_WIDTH{1'b0}};
            event_countdown <= ZERO_TIMER;
            run_done <= 1'b0;
            sync_state <= 1'b0;
        end else if (cmd_start_run) begin
            card_control_run <= (event_count != {INDEX_WIDTH{1'b0}});
            run_state <= (event_count != {INDEX_WIDTH{1'b0}}) ? RUN_LOAD : RUN_IDLE;
            event_head <= {INDEX_WIDTH{1'b0}};
            action_head <= {INDEX_WIDTH{1'b0}};
            global_event_index <= {INDEX_WIDTH{1'b0}};
            event_countdown <= ZERO_TIMER;
            run_done <= (event_count == {INDEX_WIDTH{1'b0}});
            sync_state <= 1'b0;
        end else if (wr_card_control) begin
            card_control_run <= 1'b0;
            run_state <= RUN_IDLE;
        end else begin
            case (run_state)
                RUN_LOAD: begin
                    // RUN_LOAD plus the final RUN_PREP cycle consume two clocks before WAIT can commit.
                    if (event_head_delta_ticks <= ONE_TIMER) begin
                        event_countdown <= ZERO_TIMER;
                    end else begin
                        event_countdown <= event_head_delta_ticks - TWO_TIMER;
                    end
                    run_state <= RUN_PREP;
                end

                RUN_PREP: begin
                    if (event_countdown != ZERO_TIMER) begin
                        event_countdown <= event_countdown - ONE_TIMER;
                    end

                    if (action_matches_event) begin
                        action_head <= action_head + ONE_INDEX;
                    end else begin
                        run_state <= RUN_WAIT;
                    end
                end

                RUN_WAIT: begin
                    if (event_head >= event_count) begin
                        card_control_run <= 1'b0;
                        run_state <= RUN_IDLE;
                        run_done <= 1'b1;
                    end else if (event_due) begin
                        sync_state <= ~sync_state;
                        event_head <= event_head_next;
                        global_event_index <= global_event_index + ONE_INDEX;

                        if (event_head_next >= event_count) begin
                            run_state <= RUN_FINISH;
                        end else begin
                            run_state <= RUN_LOAD;
                        end
                    end else if (event_countdown != ZERO_TIMER) begin
                        event_countdown <= event_countdown - ONE_TIMER;
                    end
                end

                RUN_FINISH: begin
                    card_control_run <= 1'b0;
                    run_state <= RUN_IDLE;
                    run_done <= 1'b1;
                end

                default: begin
                end
            endcase
        end
    end

endmodule
