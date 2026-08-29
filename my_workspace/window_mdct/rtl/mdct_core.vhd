-------------------------------------------------------------------------------
-- Complete transform-only AAC-LC MDCT core, structurally integrated with the
-- shared pure-radix-2 fft_radix2_core.
--
-- Processing chain for each sub-transform:
--
--   PCM snapshot -> window/fold -> DCT-IV pre -> FFT64/512 -> DCT-IV post
--
-- LONG/START/STOP execute one L=1024 transform.  SHORT executes eight L=128
-- transforms over snapshot bases 448+128*q and stores them consecutively.
-- The block-switch module connects to block_type/right_shape; the upstream
-- sample buffer supplies the complete 2048-sample snapshot.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_core is
  generic (
    ENABLE_TRACE : boolean := false
  );
  port (
    clk, rst_n : in std_logic;

    -- Natural-order 2048-sample snapshot loader.
    pcm_valid : in  std_logic;
    pcm_ready : out std_logic;
    pcm_index : in  mdct_pcm_addr_t;
    pcm_data  : in  mdct_pcm_t;

    -- Frame command.  right_shape is the shape selected by FDK block-switch.
    start         : in std_logic;
    block_type    : in mdct_block_t;
    right_shape   : in mdct_shape_t;
    clear_history : in std_logic;

    busy : out std_logic;
    done : out std_logic;
    mdct_exp : out unsigned(4 downto 0);

    -- Synchronous natural-order spectrum read port, 1024 values for all modes.
    spec_rd_en    : in  std_logic;
    spec_rd_index : in  mdct_data_addr_t;
    spec_rd_valid : out std_logic;
    spec_rd_data  : out mdct_data_t;

    -- Boundary traces are zeroed when ENABLE_TRACE=false.  They expose accepted
    -- transactions, so a testbench can check every boundary without probing
    -- internal memories or changing the production datapath.
    trace_sub_index : out unsigned(2 downto 0);
    trace_fold_valid : out std_logic;
    trace_fold_index : out mdct_data_addr_t;
    trace_fold_data  : out mdct_data_t;
    trace_pre_valid : out std_logic;
    trace_pre_index : out mdct_fft_addr_t;
    trace_pre_re, trace_pre_im : out mdct_data_t;
    trace_fft_valid : out std_logic;
    trace_fft_index : out mdct_fft_addr_t;
    trace_fft_re, trace_fft_im : out mdct_data_t;
    trace_post_valid : out std_logic;
    trace_post_index : out mdct_data_addr_t;
    trace_post_data  : out mdct_data_t;

    -- Optional post-butterfly trace passed through from fft_radix2_core.
    trace_fft_stage_valid : out std_logic;
    trace_fft_stage_last  : out std_logic;
    trace_fft_stage       : out unsigned(3 downto 0);
    trace_fft_addr_a, trace_fft_addr_b : out mdct_fft_addr_t;
    trace_fft_a_re, trace_fft_a_im : out mdct_data_t;
    trace_fft_b_re, trace_fft_b_im : out mdct_data_t
  );
end entity mdct_core;

architecture rtl of mdct_core is
  -----------------------------------------------------------------------------
  -- Control-plane and active-sub-transform geometry.
  -----------------------------------------------------------------------------
  signal load_accept_s : std_logic;
  signal fold_start_s, pre_start_s, fft_start_s, post_start_s : std_logic;
  signal fold_done_s, pre_done_s, fft_done_s, post_done_s : std_logic;
  signal post_load_enable_s, post_load_done_s : std_logic;
  signal active_size_s : std_logic;
  signal active_input_base_s : mdct_pcm_addr_t;
  signal active_output_base_s : mdct_data_addr_t;
  signal active_fl_s, active_fr_s : unsigned(10 downto 0);
  signal active_left_shape_s, active_right_shape_s : mdct_shape_t;
  signal active_sub_s : unsigned(2 downto 0);

  -----------------------------------------------------------------------------
  -- PCM memory and window/fold stream.
  -----------------------------------------------------------------------------
  signal pcm_rd_en_s, pcm_rd_valid_s : std_logic;
  signal pcm_rd_addr_a_s, pcm_rd_addr_b_s : mdct_pcm_addr_t;
  signal pcm_rd_data_a_s, pcm_rd_data_b_s : mdct_pcm_t;
  signal fold_busy_s, fold_valid_s, fold_last_s : std_logic;
  signal fold_index_s : mdct_data_addr_t;
  signal fold_data_s : mdct_data_t;
  signal fold_exp_s : unsigned(3 downto 0);

  -----------------------------------------------------------------------------
  -- DCT-IV pre stream directly loads the natural-order FFT input.
  -----------------------------------------------------------------------------
  signal pre_fold_ready_s, pre_busy_s : std_logic;
  signal pre_fft_valid_s, pre_fft_ready_s : std_logic;
  signal pre_fft_index_s : mdct_fft_addr_t;
  signal pre_fft_re_s, pre_fft_im_s : mdct_data_t;
  signal pre_scale_s : unsigned(3 downto 0);

  -----------------------------------------------------------------------------
  -- FFT status, result read port and stage trace.
  -----------------------------------------------------------------------------
  signal fft_load_ready_s, fft_busy_s : std_logic;
  signal fft_rd_en_s, fft_rd_valid_s : std_logic;
  signal fft_rd_index_s : mdct_fft_addr_t;
  signal fft_rd_re_s, fft_rd_im_s : mdct_data_t;
  signal fft_scale_s : unsigned(3 downto 0);
  signal fft_trace_valid_s, fft_trace_last_s : std_logic;
  signal fft_trace_stage_s : unsigned(3 downto 0);
  signal fft_trace_addr_a_s, fft_trace_addr_b_s : mdct_fft_addr_t;
  signal fft_trace_a_re_s, fft_trace_a_im_s : mdct_data_t;
  signal fft_trace_b_re_s, fft_trace_b_im_s : mdct_data_t;

  -----------------------------------------------------------------------------
  -- FFT-to-post loader and final scalar stream.
  -----------------------------------------------------------------------------
  signal post_bin_valid_s, post_bin_ready_s : std_logic;
  signal post_bin_index_s : mdct_fft_addr_t;
  signal post_busy_s, post_sample_valid_s, post_sample_ready_s : std_logic;
  signal post_sample_index_s : mdct_data_addr_t;
  signal post_sample_data_s : mdct_data_t;
  signal post_issue_count_r : natural range 0 to 512 := 0;
  signal post_accept_count_r : natural range 0 to 512 := 0;
  signal post_tag_index_r : mdct_fft_addr_t := (others => '0');
  signal post_load_done_r : std_logic := '0';

  signal spectrum_wr_addr_s : mdct_data_addr_t;
  signal spectrum_rd_valid_s : std_logic;
  signal active_fft_n_s : natural range 64 to 512;
begin
  post_load_done_s <= post_load_done_r;
  pre_fft_ready_s <= fft_load_ready_s;
  post_sample_ready_s <= '1';
  active_fft_n_s <= 512 when active_size_s = MDCT_FFT512 else 64;

  -----------------------------------------------------------------------------
  -- CONTROL
  -----------------------------------------------------------------------------
  u_control : entity work.mdct_control
    port map (
      clk => clk,
      rst_n => rst_n,
      sample_valid => pcm_valid,
      sample_index => pcm_index,
      sample_ready => pcm_ready,
      load_accept => load_accept_s,
      start => start,
      block_type => block_type,
      right_shape => right_shape,
      clear_history => clear_history,
      busy => busy,
      done => done,
      fold_done => fold_done_s,
      pre_done => pre_done_s,
      fft_done => fft_done_s,
      post_load_done => post_load_done_s,
      post_done => post_done_s,
      fold_start => fold_start_s,
      pre_start => pre_start_s,
      fft_start => fft_start_s,
      post_start => post_start_s,
      post_load_enable => post_load_enable_s,
      active_size_mode => active_size_s,
      active_input_base => active_input_base_s,
      active_output_base => active_output_base_s,
      active_left_slope_len => active_fl_s,
      active_right_slope_len => active_fr_s,
      active_left_shape => active_left_shape_s,
      active_right_shape => active_right_shape_s,
      active_sub_index => active_sub_s,
      output_exp => mdct_exp
    );

  -----------------------------------------------------------------------------
  -- MEMORY: input PCM snapshot and final spectrum are separate BRAM resources.
  -----------------------------------------------------------------------------
  u_pcm_memory : entity work.mdct_pcm_memory
    port map (
      clk => clk,
      load_we => load_accept_s,
      load_addr => pcm_index,
      load_data => pcm_data,
      rd_en => pcm_rd_en_s,
      rd_addr_a => pcm_rd_addr_a_s,
      rd_addr_b => pcm_rd_addr_b_s,
      rd_valid => pcm_rd_valid_s,
      rd_data_a => pcm_rd_data_a_s,
      rd_data_b => pcm_rd_data_b_s
    );

  spectrum_wr_addr_s <= to_unsigned(
    to_integer(active_output_base_s) + to_integer(post_sample_index_s),
    spectrum_wr_addr_s'length) when post_sample_valid_s = '1' else
    (others => '0');

  u_spectrum_memory : entity work.mdct_spectrum_memory
    port map (
      clk => clk,
      wr_en => post_sample_valid_s and post_sample_ready_s,
      wr_addr => spectrum_wr_addr_s,
      wr_data => post_sample_data_s,
      -- Holding the memory read-enable low during reset clears its registered
      -- valid flag on the first reset clock without resetting the BRAM array.
      rd_en => spec_rd_en and rst_n,
      rd_addr => spec_rd_index,
      rd_valid => spectrum_rd_valid_s,
      rd_data => spec_rd_data
    );

  -- Suppress a stale registered response immediately when reset is asserted.
  spec_rd_valid <= spectrum_rd_valid_s and rst_n;

  -----------------------------------------------------------------------------
  -- DATAPATH 1: combined analysis-window and TDAC fold.
  -----------------------------------------------------------------------------
  u_window_fold : entity work.mdct_window_fold
    port map (
      clk => clk,
      rst_n => rst_n,
      start => fold_start_s,
      size_mode => active_size_s,
      input_base => active_input_base_s,
      left_slope_len => active_fl_s,
      right_slope_len => active_fr_s,
      left_shape => active_left_shape_s,
      right_shape => active_right_shape_s,
      busy => fold_busy_s,
      done => fold_done_s,
      pcm_rd_en => pcm_rd_en_s,
      pcm_rd_addr_a => pcm_rd_addr_a_s,
      pcm_rd_addr_b => pcm_rd_addr_b_s,
      pcm_rd_valid => pcm_rd_valid_s,
      pcm_rd_data_a => pcm_rd_data_a_s,
      pcm_rd_data_b => pcm_rd_data_b_s,
      out_valid => fold_valid_s,
      out_last => fold_last_s,
      out_index => fold_index_s,
      out_data => fold_data_s,
      out_exp => fold_exp_s
    );

  -----------------------------------------------------------------------------
  -- DATAPATH 2: DCT-IV pre-rotation and packing.
  -----------------------------------------------------------------------------
  u_dct4_pre : entity work.mdct_dct4_pre
    port map (
      clk => clk,
      rst_n => rst_n,
      size_mode => active_size_s,
      fold_valid => fold_valid_s,
      fold_ready => pre_fold_ready_s,
      fold_index => fold_index_s,
      fold_data => fold_data_s,
      start => pre_start_s,
      busy => pre_busy_s,
      done => pre_done_s,
      fft_valid => pre_fft_valid_s,
      fft_ready => pre_fft_ready_s,
      fft_index => pre_fft_index_s,
      fft_re => pre_fft_re_s,
      fft_im => pre_fft_im_s,
      scale_exp => pre_scale_s
    );

  assert not (fold_valid_s = '1' and pre_fold_ready_s = '0')
    report "mdct_core: pre-loader backpressured the non-stallable fold stream"
    severity failure;

  -----------------------------------------------------------------------------
  -- DATAPATH 3: shared optimized pure-radix-2 FFT IP.
  -----------------------------------------------------------------------------
  u_fft : entity work.fft_radix2_core
    generic map (
      DATA_W => MDCT_DATA_W,
      ENABLE_TRACE => ENABLE_TRACE
    )
    port map (
      clk => clk,
      rst_n => rst_n,
      size_mode => active_size_s,
      load_ready => fft_load_ready_s,
      ld_en => pre_fft_valid_s,
      ld_idx => pre_fft_index_s,
      ld_re => pre_fft_re_s,
      ld_im => pre_fft_im_s,
      start => fft_start_s,
      busy => fft_busy_s,
      done => fft_done_s,
      rd_en => fft_rd_en_s,
      rd_idx => fft_rd_index_s,
      rd_valid => fft_rd_valid_s,
      rd_re => fft_rd_re_s,
      rd_im => fft_rd_im_s,
      scale_exp => fft_scale_s,
      trace_valid => fft_trace_valid_s,
      trace_last => fft_trace_last_s,
      trace_stage => fft_trace_stage_s,
      trace_addr_a => fft_trace_addr_a_s,
      trace_addr_b => fft_trace_addr_b_s,
      trace_a_re => fft_trace_a_re_s,
      trace_a_im => fft_trace_a_im_s,
      trace_b_re => fft_trace_b_re_s,
      trace_b_im => fft_trace_b_im_s
    );

  -----------------------------------------------------------------------------
  -- One-result-per-clock FFT reader.  mdct_dct4_post guarantees bin_ready
  -- throughout its load phase; assertions turn any contract violation into a
  -- deterministic failure instead of silently dropping a synchronous response.
  -----------------------------------------------------------------------------
  fft_rd_en_s <= '1' when post_load_enable_s = '1' and
                          post_issue_count_r < active_fft_n_s and
                          post_bin_ready_s = '1' else '0';
  fft_rd_index_s <= to_unsigned(post_issue_count_r, fft_rd_index_s'length)
                    when post_issue_count_r < active_fft_n_s else
                    (others => '0');
  post_bin_valid_s <= fft_rd_valid_s and post_load_enable_s;
  post_bin_index_s <= post_tag_index_r;

  process(clk, rst_n)
  begin
    if rst_n = '0' then
      post_issue_count_r <= 0;
      post_accept_count_r <= 0;
      post_tag_index_r <= (others => '0');
      post_load_done_r <= '0';
    elsif rising_edge(clk) then
      post_load_done_r <= '0';

      if fft_done_s = '1' then
        post_issue_count_r <= 0;
        post_accept_count_r <= 0;
      end if;

      if fft_rd_en_s = '1' then
        post_tag_index_r <= to_unsigned(post_issue_count_r,
                                        post_tag_index_r'length);
        post_issue_count_r <= post_issue_count_r + 1;
      end if;

      if fft_rd_valid_s = '1' and post_load_enable_s = '1' then
        assert post_bin_ready_s = '1'
          report "mdct_core: post loader deasserted ready with FFT response pending"
          severity failure;
        if post_bin_ready_s = '1' then
          assert to_integer(post_tag_index_r) = post_accept_count_r
            report "mdct_core: FFT-to-post index sequence mismatch"
            severity failure;
          if post_accept_count_r = active_fft_n_s - 1 then
            post_load_done_r <= '1';
          end if;
          post_accept_count_r <= post_accept_count_r + 1;
        end if;
      end if;
    end if;
  end process;

  -----------------------------------------------------------------------------
  -- DATAPATH 4: FDK DCT-IV post-rotation, reorder and scalar output.
  -----------------------------------------------------------------------------
  u_dct4_post : entity work.mdct_dct4_post
    port map (
      clk => clk,
      rst_n => rst_n,
      size_mode => active_size_s,
      bin_valid => post_bin_valid_s,
      bin_ready => post_bin_ready_s,
      bin_index => post_bin_index_s,
      bin_re => fft_rd_re_s,
      bin_im => fft_rd_im_s,
      start => post_start_s,
      busy => post_busy_s,
      done => post_done_s,
      sample_valid => post_sample_valid_s,
      sample_ready => post_sample_ready_s,
      sample_index => post_sample_index_s,
      sample_data => post_sample_data_s
    );

  -----------------------------------------------------------------------------
  -- Numerical-contract assertions.
  -----------------------------------------------------------------------------
  process(clk)
  begin
    if rising_edge(clk) and rst_n = '1' then
      if fold_valid_s = '1' then
        assert to_integer(fold_exp_s) = 2
          report "mdct_core: fold exponent must be 2"
          severity failure;
      end if;
      if pre_done_s = '1' then
        assert to_integer(pre_scale_s) = 2
          report "mdct_core: DCT-IV pre exponent must be 2"
          severity failure;
      end if;
      if fft_done_s = '1' then
        if active_size_s = MDCT_FFT512 then
          assert to_integer(fft_scale_s) = 8
            report "mdct_core: FFT512 exponent must be 8"
            severity failure;
        else
          assert to_integer(fft_scale_s) = 5
            report "mdct_core: FFT64 exponent must be 5"
            severity failure;
        end if;
      end if;
    end if;
  end process;

  -----------------------------------------------------------------------------
  -- Trace fanout.  Only accepted ready/valid transfers are reported.
  -----------------------------------------------------------------------------
  trace_sub_index <= active_sub_s when ENABLE_TRACE else (others => '0');
  trace_fold_valid <= fold_valid_s when ENABLE_TRACE else '0';
  trace_fold_index <= fold_index_s when ENABLE_TRACE else (others => '0');
  trace_fold_data <= fold_data_s when ENABLE_TRACE else (others => '0');
  trace_pre_valid <= (pre_fft_valid_s and pre_fft_ready_s) when ENABLE_TRACE else '0';
  trace_pre_index <= pre_fft_index_s when ENABLE_TRACE else (others => '0');
  trace_pre_re <= pre_fft_re_s when ENABLE_TRACE else (others => '0');
  trace_pre_im <= pre_fft_im_s when ENABLE_TRACE else (others => '0');
  trace_fft_valid <= (fft_rd_valid_s and post_load_enable_s) when ENABLE_TRACE else '0';
  trace_fft_index <= post_tag_index_r when ENABLE_TRACE else (others => '0');
  trace_fft_re <= fft_rd_re_s when ENABLE_TRACE else (others => '0');
  trace_fft_im <= fft_rd_im_s when ENABLE_TRACE else (others => '0');
  trace_post_valid <= (post_sample_valid_s and post_sample_ready_s)
                      when ENABLE_TRACE else '0';
  trace_post_index <= post_sample_index_s when ENABLE_TRACE else (others => '0');
  trace_post_data <= post_sample_data_s when ENABLE_TRACE else (others => '0');

  trace_fft_stage_valid <= fft_trace_valid_s when ENABLE_TRACE else '0';
  trace_fft_stage_last <= fft_trace_last_s when ENABLE_TRACE else '0';
  trace_fft_stage <= fft_trace_stage_s when ENABLE_TRACE else (others => '0');
  trace_fft_addr_a <= fft_trace_addr_a_s when ENABLE_TRACE else (others => '0');
  trace_fft_addr_b <= fft_trace_addr_b_s when ENABLE_TRACE else (others => '0');
  trace_fft_a_re <= fft_trace_a_re_s when ENABLE_TRACE else (others => '0');
  trace_fft_a_im <= fft_trace_a_im_s when ENABLE_TRACE else (others => '0');
  trace_fft_b_re <= fft_trace_b_re_s when ENABLE_TRACE else (others => '0');
  trace_fft_b_im <= fft_trace_b_im_s when ENABLE_TRACE else (others => '0');
end architecture rtl;
