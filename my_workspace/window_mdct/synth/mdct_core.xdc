## 100 MHz production target, aligned with the standalone FFT core.
create_clock -name mdct_clk -period 10.000 -waveform {0.000 5.000} [get_ports clk]
set_clock_uncertainty 0.200 [get_clocks mdct_clk]

## rst_n is an asynchronous control input, not a timed data path.
set_false_path -from [get_ports rst_n]

## Standalone block-level interface budget.  Board-level constraints may
## replace these values when mdct_core is integrated into the encoder SoC.
set mdct_data_inputs [remove_from_collection [all_inputs] [get_ports {clk rst_n}]]
set_input_delay 2.000 -clock [get_clocks mdct_clk] $mdct_data_inputs
set_output_delay 2.000 -clock [get_clocks mdct_clk] [all_outputs]

