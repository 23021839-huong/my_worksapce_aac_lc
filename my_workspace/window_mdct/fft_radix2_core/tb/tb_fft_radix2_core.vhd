-------------------------------------------------------------------------------
-- Self-checking mixed FFT64/FFT512 regression testbench.
--
-- Checks:
--   * Q31 final output and exponent, exactly 0 LSB;
--   * every post-butterfly stage trace, exactly 0 LSB;
--   * synchronous result-read handshake;
--   * one-cycle done pulse and deterministic pipeline latency;
--   * mode changes across frames without global reset.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use ieee.std_logic_textio.all;
library std;
use std.textio.all;

entity tb_fft_radix2_core is
end entity tb_fft_radix2_core;

architecture sim of tb_fft_radix2_core is
  constant DATA_W  : positive := 32;
  constant CLK_PER : time := 10 ns;

  type frame_size_array_t is array (natural range <>) of positive;
  constant FRAME_N : frame_size_array_t(0 to 11) :=
    (512, 64, 64, 64, 64, 64, 64, 64, 64, 512, 512, 512);

  signal clk, rst_n : std_logic := '0';
  signal size_mode : std_logic := '0';
  signal load_ready, ld_en : std_logic := '0';
  signal ld_idx : unsigned(8 downto 0) := (others => '0');
  signal ld_re, ld_im : signed(DATA_W - 1 downto 0) := (others => '0');
  signal start, busy, done : std_logic := '0';
  signal rd_en, rd_valid : std_logic := '0';
  signal rd_idx : unsigned(8 downto 0) := (others => '0');
  signal rd_re, rd_im : signed(DATA_W - 1 downto 0);
  signal scale_exp : unsigned(3 downto 0);

  signal trace_valid, trace_last : std_logic;
  signal trace_stage : unsigned(3 downto 0);
  signal trace_addr_a, trace_addr_b : unsigned(8 downto 0);
  signal trace_a_re, trace_a_im, trace_b_re, trace_b_im :
    signed(DATA_W - 1 downto 0);
begin
  clk <= not clk after CLK_PER / 2;

  dut : entity work.fft_radix2_core
    generic map (
      DATA_W => DATA_W,
      ENABLE_TRACE => true
    )
    port map (
      clk => clk,
      rst_n => rst_n,
      size_mode => size_mode,
      load_ready => load_ready,
      ld_en => ld_en,
      ld_idx => ld_idx,
      ld_re => ld_re,
      ld_im => ld_im,
      start => start,
      busy => busy,
      done => done,
      rd_en => rd_en,
      rd_idx => rd_idx,
      rd_valid => rd_valid,
      rd_re => rd_re,
      rd_im => rd_im,
      scale_exp => scale_exp,
      trace_valid => trace_valid,
      trace_last => trace_last,
      trace_stage => trace_stage,
      trace_addr_a => trace_addr_a,
      trace_addr_b => trace_addr_b,
      trace_a_re => trace_a_re,
      trace_a_im => trace_a_im,
      trace_b_re => trace_b_re,
      trace_b_im => trace_b_im
    );

  stimulus : process
    file fin    : text open read_mode is "vectors/input_fft.txt";
    file fout   : text open read_mode is "vectors/output_fft.txt";
    file fstage : text open read_mode is "vectors/output_fft_stage.txt";
    file frtl   : text open write_mode is "vectors/output_fft_rtl.txt";

    variable line_in, line_out, line_stage, line_rtl : line;
    variable mode_i, frame_i, index_i : integer;
    variable expected_mode, expected_exp, expected_ldn : integer;
    variable expected_run_cycles, run_cycles : integer;
    variable stage_mode, stage_frame, stage_i, stage_pair : integer;
    variable stage_addr_a, stage_addr_b : integer;
    variable expected_re, expected_im : std_logic_vector(DATA_W - 1 downto 0);
    variable expected_a_re, expected_a_im : std_logic_vector(DATA_W - 1 downto 0);
    variable expected_b_re, expected_b_im : std_logic_vector(DATA_W - 1 downto 0);
    variable input_re, input_im : std_logic_vector(DATA_W - 1 downto 0);
    variable errors : integer := 0;
  begin
    rst_n <= '0';
    wait for 5 * CLK_PER;
    wait until rising_edge(clk);
    rst_n <= '1';
    wait until rising_edge(clk);

    for frame in FRAME_N'range loop
      if FRAME_N(frame) = 512 then
        expected_mode := 1;
        expected_ldn := 9;
        expected_exp := 8;
        size_mode <= '1';
      else
        expected_mode := 0;
        expected_ldn := 6;
        expected_exp := 5;
        size_mode <= '0';
      end if;
      expected_run_cycles := expected_ldn * (FRAME_N(frame) / 2 + 5);

      assert load_ready = '1'
        report "DUT is not ready before frame load"
        severity failure;

      -------------------------------------------------------------------------
      -- Load one natural-order Q31 complex sample per clock.
      -------------------------------------------------------------------------
      for index in 0 to FRAME_N(frame) - 1 loop
        readline(fin, line_in);
        read(line_in, mode_i);
        read(line_in, frame_i);
        read(line_in, index_i);
        hread(line_in, input_re);
        hread(line_in, input_im);
        assert mode_i = expected_mode and frame_i = frame and index_i = index
          report "input vector metadata mismatch"
          severity failure;
        ld_en <= '1';
        ld_idx <= to_unsigned(index, ld_idx'length);
        ld_re <= signed(input_re);
        ld_im <= signed(input_im);
        wait until rising_edge(clk);
      end loop;
      ld_en <= '0';
      wait until rising_edge(clk);

      -------------------------------------------------------------------------
      -- Start and consume the continuous post-butterfly trace while busy.
      -------------------------------------------------------------------------
      start <= '1';
      wait until rising_edge(clk);
      start <= '0';
      run_cycles := 0;
      wait for 1 ns;
      assert busy = '1' report "DUT did not enter RUN" severity failure;

      while done /= '1' loop
        wait until rising_edge(clk);
        run_cycles := run_cycles + 1;
        wait for 1 ns;
        if trace_valid = '1' then
          readline(fstage, line_stage);
          read(line_stage, stage_mode);
          read(line_stage, stage_frame);
          read(line_stage, stage_i);
          read(line_stage, stage_pair);
          read(line_stage, stage_addr_a);
          read(line_stage, stage_addr_b);
          hread(line_stage, expected_a_re);
          hread(line_stage, expected_a_im);
          hread(line_stage, expected_b_re);
          hread(line_stage, expected_b_im);

          assert stage_mode = expected_mode and stage_frame = frame and
                 stage_i = to_integer(trace_stage) and
                 stage_addr_a = to_integer(trace_addr_a) and
                 stage_addr_b = to_integer(trace_addr_b)
            report "stage trace metadata mismatch"
            severity failure;
          assert trace_a_re = signed(expected_a_re) and
                 trace_a_im = signed(expected_a_im) and
                 trace_b_re = signed(expected_b_re) and
                 trace_b_im = signed(expected_b_im)
            report "stage trace arithmetic mismatch at frame=" &
                   integer'image(frame) & " stage=" & integer'image(stage_i) &
                   " pair=" & integer'image(stage_pair)
            severity error;
          if trace_a_re /= signed(expected_a_re) or
             trace_a_im /= signed(expected_a_im) or
             trace_b_re /= signed(expected_b_re) or
             trace_b_im /= signed(expected_b_im) then
            errors := errors + 1;
          end if;
          if stage_pair = FRAME_N(frame) / 2 - 1 then
            assert trace_last = '1'
              report "trace_last missing on final pair of stage"
              severity error;
          else
            assert trace_last = '0'
              report "trace_last asserted before final pair of stage"
              severity error;
          end if;
        end if;
      end loop;

      assert run_cycles = expected_run_cycles
        report "pipeline latency mismatch: got " & integer'image(run_cycles) &
               ", expected " & integer'image(expected_run_cycles)
        severity error;
      if run_cycles /= expected_run_cycles then errors := errors + 1; end if;
      assert busy = '0' report "busy and done overlap" severity error;
      assert to_integer(scale_exp) = expected_exp
        report "scale_exp mismatch"
        severity error;
      if to_integer(scale_exp) /= expected_exp then errors := errors + 1; end if;

      -------------------------------------------------------------------------
      -- Read natural-order output through the one-cycle synchronous port.
      -------------------------------------------------------------------------
      for index in 0 to FRAME_N(frame) - 1 loop
        readline(fout, line_out);
        read(line_out, mode_i);
        read(line_out, frame_i);
        read(line_out, index_i);
        hread(line_out, expected_re);
        hread(line_out, expected_im);
        read(line_out, expected_exp);
        assert mode_i = expected_mode and frame_i = frame and index_i = index
          report "output vector metadata mismatch"
          severity failure;

        rd_en <= '1';
        rd_idx <= to_unsigned(index, rd_idx'length);
        wait until rising_edge(clk);
        wait for 1 ns;
        assert rd_valid = '1' report "missing synchronous rd_valid" severity failure;

        write(line_rtl, expected_mode); write(line_rtl, string'(" "));
        write(line_rtl, frame); write(line_rtl, string'(" "));
        write(line_rtl, index); write(line_rtl, string'(" "));
        hwrite(line_rtl, std_logic_vector(rd_re)); write(line_rtl, string'(" "));
        hwrite(line_rtl, std_logic_vector(rd_im)); write(line_rtl, string'(" "));
        write(line_rtl, to_integer(scale_exp));
        writeline(frtl, line_rtl);

        if rd_re /= signed(expected_re) or rd_im /= signed(expected_im) then
          report "final FFT mismatch frame=" & integer'image(frame) &
                 " idx=" & integer'image(index)
            severity error;
          errors := errors + 1;
        end if;
      end loop;
      rd_en <= '0';
      wait until rising_edge(clk);
      wait for 1 ns;
      assert rd_valid = '0' report "rd_valid did not clear" severity error;
      assert done = '0' report "done must be one clock wide" severity error;
      assert load_ready = '1' report "DUT did not re-arm for next frame" severity error;
    end loop;

    assert endfile(fin) and endfile(fout) and endfile(fstage)
      report "unexpected extra vector records"
      severity failure;
    if errors = 0 then
      report "PASS: Q31 FFT64/FFT512 matched final and every stage trace"
        severity note;
    else
      report "FAIL: fft_radix2_core errors=" & integer'image(errors)
        severity failure;
    end if;
    std.env.stop;
    wait;
  end process;
end architecture sim;
