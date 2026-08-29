-------------------------------------------------------------------------------
-- Full self-checking regression for mdct_core.
--
-- The testbench consumes the versioned Python/C++ boundary contract and checks
-- every accepted transaction at fold, pre, each FFT stage, FFT output, post and
-- final spectrum.  All fixed-point comparisons require exactly 0 LSB error.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use ieee.std_logic_textio.all;
library std;
use std.textio.all;
use std.env.all;
use work.mdct_pkg.all;

entity tb_mdct_core is
  generic (
    -- Simulations are launched from the repository root in the Linux guide.
    GOLDEN_DIR : string := "my_workspace/window_mdct/golden/radix2_q31_v1";
    RTL_OUT_FILE : string := "rtl_mdct_out_q31.txt"
  );
end entity tb_mdct_core;

architecture sim of tb_mdct_core is
  constant CLK_PERIOD : time := 10 ns;
  constant CASE_COUNT : natural := 3;
  constant FRAME_COUNT_PER_CASE : natural := 6;

  type integer_vector_t is array (natural range <>) of integer;
  constant FRAME_BLOCK : integer_vector_t(0 to FRAME_COUNT_PER_CASE - 1) :=
    (0, 0, 1, 2, 3, 0); -- LONG, LONG, START, SHORT, STOP, LONG
  constant FRAME_SHAPE : integer_vector_t(0 to FRAME_COUNT_PER_CASE - 1) :=
    (0, 1, 0, 1, 0, 1); -- deterministic SINE/KBD state coverage

  signal clk : std_logic := '0';
  signal rst_n : std_logic := '0';
  signal pcm_valid, pcm_ready : std_logic := '0';
  signal pcm_index : mdct_pcm_addr_t := (others => '0');
  signal pcm_data : mdct_pcm_t := (others => '0');
  signal start : std_logic := '0';
  signal block_type : mdct_block_t := MDCT_LONG;
  signal right_shape : mdct_shape_t := MDCT_SINE;
  signal clear_history : std_logic := '0';
  signal busy, done : std_logic;
  signal mdct_exp : unsigned(4 downto 0);

  signal spec_rd_en, spec_rd_valid : std_logic := '0';
  signal spec_rd_index : mdct_data_addr_t := (others => '0');
  signal spec_rd_data : mdct_data_t;

  signal trace_sub_index : unsigned(2 downto 0);
  signal trace_fold_valid : std_logic;
  signal trace_fold_index : mdct_data_addr_t;
  signal trace_fold_data : mdct_data_t;
  signal trace_pre_valid : std_logic;
  signal trace_pre_index : mdct_fft_addr_t;
  signal trace_pre_re, trace_pre_im : mdct_data_t;
  signal trace_fft_valid : std_logic;
  signal trace_fft_index : mdct_fft_addr_t;
  signal trace_fft_re, trace_fft_im : mdct_data_t;
  signal trace_post_valid : std_logic;
  signal trace_post_index : mdct_data_addr_t;
  signal trace_post_data : mdct_data_t;
  signal trace_fft_stage_valid, trace_fft_stage_last : std_logic;
  signal trace_fft_stage : unsigned(3 downto 0);
  signal trace_fft_addr_a, trace_fft_addr_b : mdct_fft_addr_t;
  signal trace_fft_a_re, trace_fft_a_im : mdct_data_t;
  signal trace_fft_b_re, trace_fft_b_im : mdct_data_t;
begin
  clk <= not clk after CLK_PERIOD / 2;

  dut : entity work.mdct_core
    generic map (
      ENABLE_TRACE => true
    )
    port map (
      clk => clk,
      rst_n => rst_n,
      pcm_valid => pcm_valid,
      pcm_ready => pcm_ready,
      pcm_index => pcm_index,
      pcm_data => pcm_data,
      start => start,
      block_type => block_type,
      right_shape => right_shape,
      clear_history => clear_history,
      busy => busy,
      done => done,
      mdct_exp => mdct_exp,
      spec_rd_en => spec_rd_en,
      spec_rd_index => spec_rd_index,
      spec_rd_valid => spec_rd_valid,
      spec_rd_data => spec_rd_data,
      trace_sub_index => trace_sub_index,
      trace_fold_valid => trace_fold_valid,
      trace_fold_index => trace_fold_index,
      trace_fold_data => trace_fold_data,
      trace_pre_valid => trace_pre_valid,
      trace_pre_index => trace_pre_index,
      trace_pre_re => trace_pre_re,
      trace_pre_im => trace_pre_im,
      trace_fft_valid => trace_fft_valid,
      trace_fft_index => trace_fft_index,
      trace_fft_re => trace_fft_re,
      trace_fft_im => trace_fft_im,
      trace_post_valid => trace_post_valid,
      trace_post_index => trace_post_index,
      trace_post_data => trace_post_data,
      trace_fft_stage_valid => trace_fft_stage_valid,
      trace_fft_stage_last => trace_fft_stage_last,
      trace_fft_stage => trace_fft_stage,
      trace_fft_addr_a => trace_fft_addr_a,
      trace_fft_addr_b => trace_fft_addr_b,
      trace_fft_a_re => trace_fft_a_re,
      trace_fft_a_im => trace_fft_a_im,
      trace_fft_b_re => trace_fft_b_re,
      trace_fft_b_im => trace_fft_b_im
    );

  stimulus : process
    file f_pcm : text open read_mode is GOLDEN_DIR & "/pcm_in_s16.txt";
    file f_fold : text open read_mode is GOLDEN_DIR & "/fold_q31.txt";
    file f_pre : text open read_mode is GOLDEN_DIR & "/pre_q31.txt";
    file f_stage : text open read_mode is GOLDEN_DIR & "/fft_stage_q31.txt";
    file f_post : text open read_mode is GOLDEN_DIR & "/post_q31.txt";
    file f_final : text open read_mode is GOLDEN_DIR & "/mdct_out_q31.txt";
    file f_rtl : text open write_mode is RTL_OUT_FILE;

    type complex_expected_t is record
      re_value : std_logic_vector(31 downto 0);
      im_value : std_logic_vector(31 downto 0);
    end record;
    type stage_expected_t is array (0 to 9, 0 to 511) of complex_expected_t;
    variable expected_stage : stage_expected_t;

    variable line_v, rtl_line_v : line;
    variable case_v, frame_v, sub_v, index_v, exp_v : integer;
    variable stage_v, fft_exp_v, cumulative_exp_v : integer;
    variable pcm_hex_v : std_logic_vector(15 downto 0);
    variable re_hex_v, im_hex_v, data_hex_v : std_logic_vector(31 downto 0);
    variable active_m_v, active_ldn_v, active_l_v : natural;
    variable loaded_sub_v : integer;
    variable fold_count_v, pre_count_v, fft_count_v, post_count_v : natural;
    variable fft_stage_count_v, expected_stage_count_v : natural;
    variable cycles_v, errors_v : natural := 0;

    procedure check_meta(
      constant got_case, got_frame, got_sub : in integer;
      constant want_case, want_frame, want_sub : in integer;
      constant boundary_name : in string) is
    begin
      assert got_case = want_case and got_frame = want_frame and
             got_sub = want_sub
        report boundary_name & " metadata mismatch"
        severity failure;
    end procedure;
  begin
    rst_n <= '0';
    -- Reset must suppress both input acceptance and registered read responses,
    -- even if an upstream block has not yet lowered its valid/enable signals.
    pcm_valid <= '1';
    pcm_index <= (others => '0');
    spec_rd_en <= '1';
    wait for 5 * CLK_PERIOD;
    assert pcm_ready = '0'
      report "pcm_ready must be low during reset"
      severity failure;
    assert spec_rd_valid = '0'
      report "spec_rd_valid must be low during reset"
      severity failure;
    pcm_valid <= '0';
    spec_rd_en <= '0';
    wait until rising_edge(clk);
    rst_n <= '1';
    wait until rising_edge(clk);

    for case_id in 0 to CASE_COUNT - 1 loop
      -- Reset only the persistent window history; datapath reset is unnecessary.
      clear_history <= '1';
      wait until rising_edge(clk);
      clear_history <= '0';

      for frame_id in 0 to FRAME_COUNT_PER_CASE - 1 loop
        -----------------------------------------------------------------------
        -- Load one complete 2048-sample snapshot.
        -----------------------------------------------------------------------
        for sample_id in 0 to MDCT_SNAPSHOT_N - 1 loop
          readline(f_pcm, line_v);
          read(line_v, case_v);
          read(line_v, frame_v);
          read(line_v, index_v);
          hread(line_v, pcm_hex_v);
          assert case_v = case_id and frame_v = frame_id and index_v = sample_id
            report "PCM vector metadata mismatch"
            severity failure;
          pcm_valid <= '1';
          pcm_index <= to_unsigned(sample_id, pcm_index'length);
          pcm_data <= signed(pcm_hex_v);
          -- ready covers the complete indexed transaction; allow combinational
          -- ready to settle after presenting the next index.
          wait for 1 ns;
          assert pcm_ready = '1'
            report "mdct_core deasserted pcm_ready during contiguous load"
            severity failure;
          wait until rising_edge(clk);
        end loop;
        pcm_valid <= '0';
        wait until rising_edge(clk);

        block_type <= to_unsigned(FRAME_BLOCK(frame_id), block_type'length);
        if FRAME_SHAPE(frame_id) = 1 then
          right_shape <= MDCT_KBD;
        else
          right_shape <= MDCT_SINE;
        end if;
        start <= '1';
        wait until rising_edge(clk);
        start <= '0';

        loaded_sub_v := -1;
        fold_count_v := 0;
        pre_count_v := 0;
        fft_count_v := 0;
        post_count_v := 0;
        fft_stage_count_v := 0;
        cycles_v := 0;

        if FRAME_BLOCK(frame_id) = 2 then
          expected_stage_count_v := 8 * 6 * 32;
        else
          expected_stage_count_v := 9 * 256;
        end if;

        -----------------------------------------------------------------------
        -- Monitor every internal boundary until the whole frame completes.
        -----------------------------------------------------------------------
        loop
          wait until rising_edge(clk);
          wait for 1 ns;
          cycles_v := cycles_v + 1;
          assert cycles_v < 100000
            report "mdct_core frame timeout"
            severity failure;

          if trace_fold_valid = '1' then
            -- At the first folded sample of each sub-transform, preload its
            -- complete expected FFT RAM image.  TextIO takes no simulation
            -- time, and later butterfly addresses become direct lookups.
            if to_integer(trace_sub_index) /= loaded_sub_v then
              loaded_sub_v := to_integer(trace_sub_index);
              if FRAME_BLOCK(frame_id) = 2 then
                active_l_v := 128;
                active_m_v := 64;
                active_ldn_v := 6;
              else
                active_l_v := 1024;
                active_m_v := 512;
                active_ldn_v := 9;
              end if;

              for expected_stage_id in 0 to active_ldn_v loop
                for expected_index_id in 0 to active_m_v - 1 loop
                  readline(f_stage, line_v);
                  read(line_v, case_v);
                  read(line_v, frame_v);
                  read(line_v, sub_v);
                  read(line_v, stage_v);
                  read(line_v, index_v);
                  hread(line_v, re_hex_v);
                  hread(line_v, im_hex_v);
                  read(line_v, fft_exp_v);
                  read(line_v, cumulative_exp_v);
                  check_meta(case_v, frame_v, sub_v,
                             case_id, frame_id, loaded_sub_v, "FFT stage");
                  assert stage_v = expected_stage_id and
                         index_v = expected_index_id
                    report "FFT stage vector order mismatch"
                    severity failure;
                  expected_stage(expected_stage_id, expected_index_id).re_value :=
                    re_hex_v;
                  expected_stage(expected_stage_id, expected_index_id).im_value :=
                    im_hex_v;
                end loop;
              end loop;
            end if;

            readline(f_fold, line_v);
            read(line_v, case_v);
            read(line_v, frame_v);
            read(line_v, sub_v);
            read(line_v, index_v);
            hread(line_v, data_hex_v);
            read(line_v, exp_v);
            check_meta(case_v, frame_v, sub_v,
                       case_id, frame_id, to_integer(trace_sub_index), "fold");
            assert index_v = to_integer(trace_fold_index) and exp_v = 2 and
                   trace_fold_data = signed(data_hex_v)
              report "fold mismatch case=" & integer'image(case_id) &
                     " frame=" & integer'image(frame_id) &
                     " sub=" & integer'image(sub_v) &
                     " index=" & integer'image(index_v)
              severity error;
            if trace_fold_data /= signed(data_hex_v) then
              errors_v := errors_v + 1;
            end if;
            fold_count_v := fold_count_v + 1;
          end if;

          if trace_pre_valid = '1' then
            readline(f_pre, line_v);
            read(line_v, case_v);
            read(line_v, frame_v);
            read(line_v, sub_v);
            read(line_v, index_v);
            hread(line_v, re_hex_v);
            hread(line_v, im_hex_v);
            read(line_v, exp_v);
            check_meta(case_v, frame_v, sub_v,
                       case_id, frame_id, to_integer(trace_sub_index), "pre");
            assert index_v = to_integer(trace_pre_index) and exp_v = 4 and
                   trace_pre_re = signed(re_hex_v) and
                   trace_pre_im = signed(im_hex_v)
              report "DCT-IV pre mismatch case=" & integer'image(case_id) &
                     " frame=" & integer'image(frame_id) &
                     " sub=" & integer'image(sub_v) &
                     " index=" & integer'image(index_v)
              severity error;
            if trace_pre_re /= signed(re_hex_v) or
               trace_pre_im /= signed(im_hex_v) then
              errors_v := errors_v + 1;
            end if;
            pre_count_v := pre_count_v + 1;
          end if;

          if trace_fft_stage_valid = '1' then
            stage_v := to_integer(trace_fft_stage);
            assert trace_fft_a_re = signed(expected_stage(
                     stage_v, to_integer(trace_fft_addr_a)).re_value) and
                   trace_fft_a_im = signed(expected_stage(
                     stage_v, to_integer(trace_fft_addr_a)).im_value) and
                   trace_fft_b_re = signed(expected_stage(
                     stage_v, to_integer(trace_fft_addr_b)).re_value) and
                   trace_fft_b_im = signed(expected_stage(
                     stage_v, to_integer(trace_fft_addr_b)).im_value)
              report "FFT internal stage mismatch case=" & integer'image(case_id) &
                     " frame=" & integer'image(frame_id) &
                     " sub=" & integer'image(to_integer(trace_sub_index)) &
                     " stage=" & integer'image(stage_v)
              severity error;
            if trace_fft_a_re /= signed(expected_stage(
                 stage_v, to_integer(trace_fft_addr_a)).re_value) or
               trace_fft_a_im /= signed(expected_stage(
                 stage_v, to_integer(trace_fft_addr_a)).im_value) or
               trace_fft_b_re /= signed(expected_stage(
                 stage_v, to_integer(trace_fft_addr_b)).re_value) or
               trace_fft_b_im /= signed(expected_stage(
                 stage_v, to_integer(trace_fft_addr_b)).im_value) then
              errors_v := errors_v + 1;
            end if;
            fft_stage_count_v := fft_stage_count_v + 1;
          end if;

          if trace_fft_valid = '1' then
            index_v := to_integer(trace_fft_index);
            if FRAME_BLOCK(frame_id) = 2 then active_ldn_v := 6;
            else active_ldn_v := 9;
            end if;
            assert trace_fft_re = signed(expected_stage(
                     active_ldn_v, index_v).re_value) and
                   trace_fft_im = signed(expected_stage(
                     active_ldn_v, index_v).im_value)
              report "FFT final mismatch case=" & integer'image(case_id) &
                     " frame=" & integer'image(frame_id) &
                     " sub=" & integer'image(to_integer(trace_sub_index)) &
                     " index=" & integer'image(index_v)
              severity error;
            if trace_fft_re /= signed(expected_stage(active_ldn_v, index_v).re_value) or
               trace_fft_im /= signed(expected_stage(active_ldn_v, index_v).im_value) then
              errors_v := errors_v + 1;
            end if;
            fft_count_v := fft_count_v + 1;
          end if;

          if trace_post_valid = '1' then
            readline(f_post, line_v);
            read(line_v, case_v);
            read(line_v, frame_v);
            read(line_v, sub_v);
            read(line_v, index_v);
            hread(line_v, data_hex_v);
            read(line_v, exp_v);
            check_meta(case_v, frame_v, sub_v,
                       case_id, frame_id, to_integer(trace_sub_index), "post");
            assert index_v = to_integer(trace_post_index) and
                   trace_post_data = signed(data_hex_v) and
                   exp_v = mdct_output_exp(to_unsigned(
                     FRAME_BLOCK(frame_id), block_type'length))
              report "DCT-IV post mismatch case=" & integer'image(case_id) &
                     " frame=" & integer'image(frame_id) &
                     " sub=" & integer'image(sub_v) &
                     " index=" & integer'image(index_v)
              severity error;
            if trace_post_data /= signed(data_hex_v) then
              errors_v := errors_v + 1;
            end if;
            post_count_v := post_count_v + 1;
          end if;

          exit when done = '1';
        end loop;

        assert fold_count_v = 1024 and pre_count_v = 512 and
               fft_count_v = 512 and post_count_v = 1024
          report "boundary transaction count mismatch"
          severity failure;
        assert fft_stage_count_v = expected_stage_count_v
          report "FFT trace transaction count mismatch"
          severity failure;
        assert to_integer(mdct_exp) = mdct_output_exp(to_unsigned(
                 FRAME_BLOCK(frame_id), block_type'length))
          report "final MDCT exponent mismatch"
          severity failure;

        -----------------------------------------------------------------------
        -- Read and compare the complete natural-order 1024-bin spectrum.
        -----------------------------------------------------------------------
        for output_id in 0 to MDCT_FRAME_LEN - 1 loop
          readline(f_final, line_v);
          read(line_v, case_v);
          read(line_v, frame_v);
          read(line_v, index_v);
          hread(line_v, data_hex_v);
          read(line_v, exp_v);
          assert case_v = case_id and frame_v = frame_id and
                 index_v = output_id
            report "final vector metadata mismatch"
            severity failure;

          spec_rd_en <= '1';
          spec_rd_index <= to_unsigned(output_id, spec_rd_index'length);
          wait until rising_edge(clk);
          wait for 1 ns;
          assert spec_rd_valid = '1'
            report "missing synchronous spectrum rd_valid"
            severity failure;
          assert spec_rd_data = signed(data_hex_v)
            report "final MDCT mismatch case=" & integer'image(case_id) &
                   " frame=" & integer'image(frame_id) &
                   " index=" & integer'image(output_id)
            severity error;
          if spec_rd_data /= signed(data_hex_v) then errors_v := errors_v + 1; end if;

          -- Dump the value read from RTL, independently of the golden value.
          -- Format: case frame output_index rtl_hex rtl_exponent.
          write(rtl_line_v, case_id);
          write(rtl_line_v, string'(" "));
          write(rtl_line_v, frame_id);
          write(rtl_line_v, string'(" "));
          write(rtl_line_v, output_id);
          write(rtl_line_v, string'(" "));
          hwrite(rtl_line_v, std_logic_vector(spec_rd_data));
          write(rtl_line_v, string'(" "));
          write(rtl_line_v, to_integer(mdct_exp));
          writeline(f_rtl, rtl_line_v);
        end loop;
        spec_rd_en <= '0';
        wait until rising_edge(clk);
        wait for 1 ns;
        assert spec_rd_valid = '0'
          report "spectrum rd_valid did not clear"
          severity failure;
      end loop;
    end loop;

    assert endfile(f_pcm) and endfile(f_fold) and endfile(f_pre) and
           endfile(f_stage) and endfile(f_post) and endfile(f_final)
      report "one or more golden files contain unread records"
      severity failure;
    assert errors_v = 0
      report "FAIL: mdct_core mismatches=" & integer'image(errors_v)
      severity failure;

    report "PASS: mdct_core matches fold/pre/every FFT stage/post/final at 0 LSB"
      severity note;
    finish;
    wait;
  end process;
end architecture sim;
