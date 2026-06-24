module timingcard_action_queue #(
    parameter [15:0]  MAX_EVENTS = 16'd48,
    parameter integer INDEX_WIDTH = 6,
    parameter integer DUTY_WIDTH = 16
) (
    input  wire                   clk,
    input  wire                   clear,
    input  wire                   write_event_index,
    input  wire                   write_meta,
    input  wire                   write_phase_step,
    input  wire                   write_duty_threshold,
    input  wire                   push,
    input  wire [31:0]            write_data,
    input  wire [INDEX_WIDTH-1:0] head_index,
    output wire [31:0]            stage_event_index,
    output wire [31:0]            stage_meta,
    output reg  [31:0]            stage_phase_step,
    output reg  [31:0]            stage_duty_threshold,
    output reg  [INDEX_WIDTH-1:0] count,
    output reg                    push_seen,
    output wire [31:0]            last_event_index,
    output reg  [31:0]            checksum,
    output wire                   head_valid,
    output wire [INDEX_WIDTH-1:0] head_event_index,
    output wire [4:0]             head_channel,
    output wire [4:0]             head_control,
    output wire [31:0]            head_phase_step,
    output wire [DUTY_WIDTH-1:0]  head_duty_threshold,
    output wire                   head_phase_step_zero,
    output wire                   head_duty_threshold_full
);

    localparam [INDEX_WIDTH-1:0] MAX_COUNT = MAX_EVENTS[INDEX_WIDTH-1:0];
    localparam integer EVENT_LSB = 0;
    localparam integer EVENT_MSB = INDEX_WIDTH - 1;
    localparam integer CH_LSB = INDEX_WIDTH;
    localparam integer CH_MSB = INDEX_WIDTH + 4;
    localparam integer CTRL_LSB = INDEX_WIDTH + 5;
    localparam integer CTRL_MSB = INDEX_WIDTH + 9;
    localparam integer PHASE_LSB = INDEX_WIDTH + 10;
    localparam integer PHASE_MSB = INDEX_WIDTH + 41;
    localparam integer DUTY_LSB = INDEX_WIDTH + 42;
    localparam integer DUTY_MSB = INDEX_WIDTH + 41 + DUTY_WIDTH;
    localparam integer PHASE_ZERO_BIT = INDEX_WIDTH + 42 + DUTY_WIDTH;
    localparam integer DUTY_FULL_BIT = INDEX_WIDTH + 43 + DUTY_WIDTH;
    localparam integer ENTRY_WIDTH = INDEX_WIDTH + DUTY_WIDTH + 44;

    reg [ENTRY_WIDTH-1:0] action_entry [0:MAX_EVENTS-1];

    reg [INDEX_WIDTH-1:0] stage_event_index_reg;
    reg [4:0]             stage_channel;
    reg [4:0]             stage_control;
    reg                   stage_phase_step_zero;
    reg                   stage_duty_threshold_full;
    reg [INDEX_WIDTH-1:0] last_event_index_reg;

    wire [ENTRY_WIDTH-1:0] head_entry;
    wire [31:0] checksum_after_event;
    wire [31:0] checksum_after_meta;
    wire [31:0] checksum_after_phase;
    wire [31:0] checksum_after_duty;

    assign head_valid = (head_index < count);
    assign head_entry = head_valid ? action_entry[head_index] : {ENTRY_WIDTH{1'b0}};
    assign head_event_index = head_entry[EVENT_MSB:EVENT_LSB];
    assign head_channel = head_entry[CH_MSB:CH_LSB];
    assign head_control = head_entry[CTRL_MSB:CTRL_LSB];
    assign head_phase_step = head_entry[PHASE_MSB:PHASE_LSB];
    assign head_duty_threshold = head_entry[DUTY_MSB:DUTY_LSB];
    assign head_phase_step_zero = head_entry[PHASE_ZERO_BIT];
    assign head_duty_threshold_full = head_entry[DUTY_FULL_BIT];
    assign stage_event_index = {{(32-INDEX_WIDTH){1'b0}}, stage_event_index_reg};
    assign stage_meta = {16'd0, 3'd0, stage_control, 3'd0, stage_channel};
    assign last_event_index = {{(32-INDEX_WIDTH){1'b0}}, last_event_index_reg};

    function [31:0] checksum_mix;
        input [31:0] checksum_in;
        input [31:0] value;
        begin
            checksum_mix = {checksum_in[30:0], checksum_in[31]} ^ value;
        end
    endfunction

    assign checksum_after_event = checksum_mix(checksum, stage_event_index);
    assign checksum_after_meta = checksum_mix(checksum_after_event, stage_meta);
    assign checksum_after_phase = checksum_mix(checksum_after_meta, stage_phase_step);
    assign checksum_after_duty = checksum_mix(checksum_after_phase, stage_duty_threshold);

    initial begin
        stage_event_index_reg = {INDEX_WIDTH{1'b0}};
        stage_channel = 5'd0;
        stage_control = 5'd0;
        stage_phase_step = 32'd0;
        stage_duty_threshold = 32'd0;
        stage_phase_step_zero = 1'b1;
        stage_duty_threshold_full = 1'b0;
        count = {INDEX_WIDTH{1'b0}};
        push_seen = 1'b0;
        last_event_index_reg = {INDEX_WIDTH{1'b0}};
        checksum = 32'd0;
    end

    always @(posedge clk) begin
        if (clear) begin
            stage_event_index_reg <= {INDEX_WIDTH{1'b0}};
            stage_channel <= 5'd0;
            stage_control <= 5'd0;
            stage_phase_step <= 32'd0;
            stage_duty_threshold <= 32'd0;
            stage_phase_step_zero <= 1'b1;
            stage_duty_threshold_full <= 1'b0;
            count <= {INDEX_WIDTH{1'b0}};
            push_seen <= 1'b0;
            last_event_index_reg <= {INDEX_WIDTH{1'b0}};
            checksum <= 32'd0;
        end else begin
            if (write_event_index) begin
                stage_event_index_reg <= write_data[INDEX_WIDTH-1:0];
            end

            if (write_meta) begin
                stage_channel <= write_data[4:0];
                stage_control <= write_data[12:8];
            end

            if (write_phase_step) begin
                stage_phase_step <= write_data;
                stage_phase_step_zero <= (write_data == 32'd0);
            end

            if (write_duty_threshold) begin
                stage_duty_threshold <= write_data;
                stage_duty_threshold_full <= (write_data == 32'hFFFFFFFF);
            end

            if (push && (count < MAX_COUNT)) begin
                action_entry[count] <= {
                    stage_duty_threshold_full,
                    stage_phase_step_zero,
                    stage_duty_threshold[31 -: DUTY_WIDTH],
                    stage_phase_step,
                    stage_control,
                    stage_channel,
                    stage_event_index_reg
                };
                count <= count + {{INDEX_WIDTH-1{1'b0}}, 1'b1};
                push_seen <= 1'b1;
                last_event_index_reg <= stage_event_index_reg;
                checksum <= checksum_after_duty;
            end
        end
    end

endmodule
