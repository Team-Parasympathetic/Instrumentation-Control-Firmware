module timingcard_gpio_bank #(
    parameter integer DUTY_WIDTH = 16
) (
    input  wire                  clk,
    input  wire                  clear,
    input  wire                  preload_valid,
    input  wire [4:0]            preload_channel,
    input  wire [4:0]            preload_control,
    input  wire [31:0]           preload_phase_step,
    input  wire [DUTY_WIDTH-1:0] preload_duty_threshold,
    input  wire                  preload_phase_step_zero,
    input  wire                  preload_duty_threshold_full,
    input  wire                  commit_event,
    input  wire                  sync_state,
    output wire [31:0]           channel_level_bits
);

    integer i;

    reg [31:0]           pending_mask;
    reg [4:0]            pending_control [0:31];
    reg [31:0]           pending_phase_step [0:31];
    reg [DUTY_WIDTH-1:0] pending_duty_threshold [0:31];
    reg                  pending_phase_step_zero [0:31];
    reg                  pending_duty_threshold_full [0:31];

    wire [31:0] commit_mask;

    assign commit_mask = commit_event ? pending_mask : 32'd0;

    generate
        genvar chan_index;
        for (chan_index = 0; chan_index < 32; chan_index = chan_index + 1) begin : gen_pwm_channels
            timingcard_pwm_channel #(
                .DUTY_WIDTH(DUTY_WIDTH)
            ) u_pwm_channel (
                .clk                     (clk),
                .clear                   (clear),
                .load                    (commit_mask[chan_index]),
                .next_control            (pending_control[chan_index]),
                .next_phase_step         (pending_phase_step[chan_index]),
                .next_duty_threshold     (pending_duty_threshold[chan_index]),
                .next_phase_step_zero    (pending_phase_step_zero[chan_index]),
                .next_duty_threshold_full(pending_duty_threshold_full[chan_index]),
                .sync_state              (sync_state),
                .level                   (channel_level_bits[chan_index])
            );
        end
    endgenerate

    initial begin
        pending_mask = 32'd0;
        for (i = 0; i < 32; i = i + 1) begin
            pending_control[i] = 5'd0;
            pending_phase_step[i] = 32'd0;
            pending_duty_threshold[i] = {DUTY_WIDTH{1'b0}};
            pending_phase_step_zero[i] = 1'b1;
            pending_duty_threshold_full[i] = 1'b0;
        end
    end

    always @(posedge clk) begin
        if (clear) begin
            pending_mask <= 32'd0;
        end else begin
            if (commit_event) begin
                pending_mask <= 32'd0;
            end

            if (preload_valid) begin
                pending_control[preload_channel] <= preload_control;
                pending_phase_step[preload_channel] <= preload_phase_step;
                pending_duty_threshold[preload_channel] <= preload_duty_threshold;
                pending_phase_step_zero[preload_channel] <= preload_phase_step_zero;
                pending_duty_threshold_full[preload_channel] <= preload_duty_threshold_full;
                pending_mask[preload_channel] <= 1'b1;
            end
        end
    end

endmodule
