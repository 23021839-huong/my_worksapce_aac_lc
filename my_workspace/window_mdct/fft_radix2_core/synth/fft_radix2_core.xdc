## 100 MHz production timing target for the PYNQ-Z2 XC7Z020.
create_clock -name fft_clk -period 10.000 -waveform {0.000 5.000} [get_ports clk]

## rst_n is an asynchronous control input, not a timed data path.
set_false_path -from [get_ports rst_n]
