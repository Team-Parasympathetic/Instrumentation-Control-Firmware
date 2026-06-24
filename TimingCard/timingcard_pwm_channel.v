`include "fpga_timing_regs.vh"

module timingcard_pwm_channel #(
    parameter integer DUTY_WIDTH = 16
) (
    input  wire                    clk,
    input  wire                    clear,
    input  wire                    load,
    input  wire [4:0]              next_control,
    input  wire [31:0]             next_phase_step,
    input  wire [DUTY_WIDTH-1:0]   next_duty_threshold,
    input  wire                    next_phase_step_zero,
    input  wire                    next_duty_threshold_full,
    input  wire                    sync_state,
    output wire                    level
);

    reg [4:0]            current_control;
    reg [31:0]           current_phase_step;
    reg [DUTY_WIDTH-1:0] current_duty_threshold;
    reg                  current_phase_step_zero;
    reg                  current_duty_threshold_full;
    reg [31:0]           phase;

    wire [2:0] current_mode;
    wire [32:0] phase_sum;
    wire       pwm_level_raw;
    wire       pwm_level;
    wire       mirror_sync_level;

    assign current_mode = current_control[2:0];
    assign phase_sum = {1'b0, phase} + {1'b0, current_phase_step};
    assign pwm_level_raw = current_phase_step_zero ? 1'b0 :
                           (current_duty_threshold == {DUTY_WIDTH{1'b0}}) ? 1'b0 :
                           current_duty_threshold_full ? 1'b1 :
                           (phase[31 -: DUTY_WIDTH] < current_duty_threshold);
    assign pwm_level = current_control[3] ? ~pwm_level_raw : pwm_level_raw;
    assign mirror_sync_level = current_control[3] ? ~sync_state : sync_state;
    assign level = (current_mode == `FPGA_CHANNEL_MODE_STOP)        ? current_control[4] :
                   (current_mode == `FPGA_CHANNEL_MODE_FORCE_LOW)   ? 1'b0 :
                   (current_mode == `FPGA_CHANNEL_MODE_FORCE_HIGH)  ? 1'b1 :
                   (current_mode == `FPGA_CHANNEL_MODE_PWM)         ? pwm_level :
                   (current_mode == `FPGA_CHANNEL_MODE_MIRROR_SYNC) ? mirror_sync_level :
                   1'b0;

    initial begin
        current_control = 5'd0;
        current_phase_step = 32'd0;
        current_duty_threshold = {DUTY_WIDTH{1'b0}};
        current_phase_step_zero = 1'b1;
        current_duty_threshold_full = 1'b0;
        phase = 32'd0;
    end

    always @(posedge clk) begin
        if (clear) begin
            current_control <= 5'd0;
            current_phase_step <= 32'd0;
            current_duty_threshold <= {DUTY_WIDTH{1'b0}};
            current_phase_step_zero <= 1'b1;
            current_duty_threshold_full <= 1'b0;
            phase <= 32'd0;
        end else if (load) begin
            current_control <= next_control;
            current_phase_step <= next_phase_step;
            current_duty_threshold <= next_duty_threshold;
            current_phase_step_zero <= next_phase_step_zero;
            current_duty_threshold_full <= next_duty_threshold_full;
            phase <= 32'd0;
        end else if (current_mode == `FPGA_CHANNEL_MODE_PWM) begin
            phase <= phase_sum[31:0];
        end
    end

endmodule
