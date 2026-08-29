-------------------------------------------------------------------------------
-- Top-level shared FFT64/FFT512 for the AAC-LC MDCT backend.
--
-- Profile      : FDK_AACLC_1024_Q31Q15_RAD2_V1
-- Algorithm    : forward radix-2 DIT, exp(-j*2*pi*k*n/N)
-- Data/coeff   : signed Q1.31 / signed Q1.15
-- Input/output : natural order / natural order
-- Scaling      : stage masks 101111 (N=64), 101111111 (N=512)
--
-- The file is intentionally structural.  Five focused blocks implement:
--   control -> address generator -> ping-pong memory -> twiddle ROM -> datapath.
-- This separation makes protocol, addressing, storage and arithmetic readable
-- and independently testable.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.fft_radix2_pkg.all;

entity fft_radix2_core is
  generic (
    DATA_W       : positive := 32;
    ENABLE_TRACE : boolean := false
  );
  port (
    clk, rst_n : in std_logic;

    -- Runtime transform size: 0=FFT64, 1=FFT512.
    size_mode : in std_logic;

    -- Input loader.  ld_idx is natural order and must be contiguous from zero.
    load_ready : out std_logic;
    ld_en      : in  std_logic;
    ld_idx     : in  unsigned(8 downto 0);
    ld_re      : in  signed(DATA_W - 1 downto 0);
    ld_im      : in  signed(DATA_W - 1 downto 0);
    start      : in  std_logic;

    -- Frame status.  done is a one-clock pulse after the final RAM write.
    busy : out std_logic;
    done : out std_logic;

    -- Synchronous natural-order result read port.
    rd_en    : in  std_logic;
    rd_idx   : in  unsigned(8 downto 0);
    rd_valid : out std_logic;
    rd_re    : out signed(DATA_W - 1 downto 0);
    rd_im    : out signed(DATA_W - 1 downto 0);

    -- Fixed number of right shifts performed by the FFT: 5 or 8.
    scale_exp : out unsigned(3 downto 0);

    -- Optional post-butterfly trace.  Synthesis sets ENABLE_TRACE=false so the
    -- debug cone is constant-folded away; the testbench enables it.
    trace_valid  : out std_logic;
    trace_last   : out std_logic;
    trace_stage  : out unsigned(3 downto 0);
    trace_addr_a : out unsigned(8 downto 0);
    trace_addr_b : out unsigned(8 downto 0);
    trace_a_re   : out signed(DATA_W - 1 downto 0);
    trace_a_im   : out signed(DATA_W - 1 downto 0);
    trace_b_re   : out signed(DATA_W - 1 downto 0);
    trace_b_im   : out signed(DATA_W - 1 downto 0)
  );
end entity fft_radix2_core;

architecture rtl of fft_radix2_core is
  -----------------------------------------------------------------------------
  -- Control-plane signals.
  -----------------------------------------------------------------------------
  signal load_ready_s, load_accept_s : std_logic;
  signal busy_s, done_s, stage_start_s : std_logic;
  signal stage_index_s : unsigned(3 downto 0);
  signal active_n_s : unsigned(9 downto 0);
  signal source_bank_s, dest_bank_s, result_bank_s : std_logic;
  signal scale_exp_s : unsigned(3 downto 0);

  -----------------------------------------------------------------------------
  -- Address-generator issue interface.  One transaction is issued per clock.
  -----------------------------------------------------------------------------
  signal issue_valid_s, issue_last_s : std_logic;
  signal issue_addr_a_s, issue_addr_b_s : fft_addr_t;
  signal issue_phase_s : unsigned(7 downto 0);

  -----------------------------------------------------------------------------
  -- Twiddle and one-cycle issue tag aligned to the synchronous memory output.
  -----------------------------------------------------------------------------
  signal tw_cos_s, tw_sin_s : fft_coeff_t;
  signal tw_cos_neg_s : std_logic;
  signal tag_valid_r, tag_last_r : std_logic := '0';
  signal tag_stage_r : unsigned(3 downto 0) := (others => '0');
  signal tag_addr_a_r, tag_addr_b_r : fft_addr_t := (others => '0');
  signal tag_phase_r : unsigned(7 downto 0) := (others => '0');
  signal tag_cos_r, tag_sin_r : fft_coeff_t := (others => '0');
  signal tag_cos_neg_r : std_logic := '0';

  -----------------------------------------------------------------------------
  -- Memory read and pipelined butterfly output/writeback interfaces.
  -----------------------------------------------------------------------------
  signal mem_rd_valid_s : std_logic;
  signal mem_a_re_s, mem_a_im_s, mem_b_re_s, mem_b_im_s : fft_data_t;
  signal bf_valid_s, bf_last_s : std_logic;
  signal bf_stage_s : unsigned(3 downto 0);
  signal bf_addr_a_s, bf_addr_b_s : fft_addr_t;
  signal bf_a_re_s, bf_a_im_s, bf_b_re_s, bf_b_im_s : fft_data_t;

  signal load_addr_s : fft_addr_t;
  signal result_re_s, result_im_s : fft_data_t;
begin
  assert DATA_W = FFT_DATA_W
    report "fft_radix2_core: DATA_W is fixed to 32 by the FDK Q31 profile"
    severity failure;

  load_ready <= load_ready_s;
  busy <= busy_s;
  done <= done_s;
  scale_exp <= scale_exp_s;
  rd_re <= resize(result_re_s, DATA_W);
  rd_im <= resize(result_im_s, DATA_W);

  -----------------------------------------------------------------------------
  -- CONTROL: validates frame loading, sequences stages and swaps ping-pong RAM.
  -----------------------------------------------------------------------------
  u_control : entity work.fft_radix2_control
    port map (
      clk => clk,
      rst_n => rst_n,
      size_mode => size_mode,
      ld_en => ld_en,
      ld_idx => ld_idx,
      start => start,
      pipeline_out_valid => bf_valid_s,
      pipeline_out_last => bf_last_s,
      load_ready => load_ready_s,
      load_accept => load_accept_s,
      busy => busy_s,
      done => done_s,
      stage_start => stage_start_s,
      stage_index => stage_index_s,
      active_n => active_n_s,
      source_bank => source_bank_s,
      dest_bank => dest_bank_s,
      result_bank => result_bank_s,
      scale_exp => scale_exp_s
    );

  -----------------------------------------------------------------------------
  -- ADDRESS GENERATOR: no per-butterfly division/modulo or general multiply.
  -----------------------------------------------------------------------------
  u_addr_gen : entity work.fft_radix2_addr_gen
    port map (
      clk => clk,
      rst_n => rst_n,
      stage_start => stage_start_s,
      stage_index => stage_index_s,
      active_n => active_n_s,
      issue_advance => issue_valid_s,
      issue_valid => issue_valid_s,
      issue_last => issue_last_s,
      addr_a => issue_addr_a_s,
      addr_b => issue_addr_b_s,
      phase_index => issue_phase_s
    );

  -----------------------------------------------------------------------------
  -- TWIDDLE: 65 Q15 octant entries reconstruct the complete 0..pi phase set.
  -----------------------------------------------------------------------------
  u_twiddle : entity work.fft_radix2_twiddle_rom
    port map (
      phase_index => issue_phase_s,
      cos_mag => tw_cos_s,
      sin_mag => tw_sin_s,
      cos_neg => tw_cos_neg_s
    );

  -----------------------------------------------------------------------------
  -- INPUT ORDERING: DIT receives bit-reversed storage and returns natural bins.
  -----------------------------------------------------------------------------
  load_addr_s <= bit_reverse_index(ld_idx, 9) when size_mode = MODE_FFT512 else
                 bit_reverse_index(ld_idx, 6);

  -----------------------------------------------------------------------------
  -- ISSUE TAG: RAM data appears one cycle after issue; metadata follows it.
  -----------------------------------------------------------------------------
  process(clk, rst_n)
  begin
    if rst_n = '0' then
      tag_valid_r <= '0';
      tag_last_r <= '0';
    elsif rising_edge(clk) then
      tag_valid_r <= issue_valid_s;
      if issue_valid_s = '1' then
        tag_last_r <= issue_last_s;
        tag_stage_r <= stage_index_s;
        tag_addr_a_r <= issue_addr_a_s;
        tag_addr_b_r <= issue_addr_b_s;
        tag_phase_r <= issue_phase_s;
        tag_cos_r <= tw_cos_s;
        tag_sin_r <= tw_sin_s;
        tag_cos_neg_r <= tw_cos_neg_s;
      end if;
      assert mem_rd_valid_s = tag_valid_r
        report "fft_radix2_core: RAM data/tag pipeline misalignment"
        severity failure;
    end if;
  end process;

  -----------------------------------------------------------------------------
  -- MEMORY: source and destination banks are physically separated per stage.
  -----------------------------------------------------------------------------
  u_memory : entity work.fft_radix2_memory
    port map (
      clk => clk,
      load_we => load_accept_s,
      load_addr => load_addr_s,
      load_re => resize(ld_re, FFT_DATA_W),
      load_im => resize(ld_im, FFT_DATA_W),
      compute_rd_en => issue_valid_s,
      compute_rd_bank => source_bank_s,
      compute_addr_a => issue_addr_a_s,
      compute_addr_b => issue_addr_b_s,
      compute_rd_valid => mem_rd_valid_s,
      compute_a_re => mem_a_re_s,
      compute_a_im => mem_a_im_s,
      compute_b_re => mem_b_re_s,
      compute_b_im => mem_b_im_s,
      compute_wr_en => bf_valid_s,
      compute_wr_bank => dest_bank_s,
      compute_wr_addr_a => bf_addr_a_s,
      compute_wr_addr_b => bf_addr_b_s,
      compute_wr_a_re => bf_a_re_s,
      compute_wr_a_im => bf_a_im_s,
      compute_wr_b_re => bf_b_re_s,
      compute_wr_b_im => bf_b_im_s,
      result_rd_en => rd_en,
      result_rd_bank => result_bank_s,
      result_rd_addr => rd_idx,
      result_rd_valid => rd_valid,
      result_re => result_re_s,
      result_im => result_im_s
    );

  -----------------------------------------------------------------------------
  -- DATAPATH: four real products in parallel; one butterfly enters each clock.
  -----------------------------------------------------------------------------
  u_butterfly : entity work.fft_radix2_butterfly
    port map (
      clk => clk,
      rst_n => rst_n,
      in_valid => mem_rd_valid_s,
      in_last => tag_last_r,
      in_stage => tag_stage_r,
      in_addr_a => tag_addr_a_r,
      in_addr_b => tag_addr_b_r,
      in_phase => tag_phase_r,
      in_cos_mag => tag_cos_r,
      in_sin_mag => tag_sin_r,
      in_cos_neg => tag_cos_neg_r,
      in_a_re => mem_a_re_s,
      in_a_im => mem_a_im_s,
      in_b_re => mem_b_re_s,
      in_b_im => mem_b_im_s,
      out_valid => bf_valid_s,
      out_last => bf_last_s,
      out_stage => bf_stage_s,
      out_addr_a => bf_addr_a_s,
      out_addr_b => bf_addr_b_s,
      out_a_re => bf_a_re_s,
      out_a_im => bf_a_im_s,
      out_b_re => bf_b_re_s,
      out_b_im => bf_b_im_s
    );

  -----------------------------------------------------------------------------
  -- Optional verification trace.  Default synthesis configuration removes it.
  -----------------------------------------------------------------------------
  gen_trace : if ENABLE_TRACE generate
    trace_valid <= bf_valid_s;
    trace_last <= bf_last_s;
    trace_stage <= bf_stage_s;
    trace_addr_a <= bf_addr_a_s;
    trace_addr_b <= bf_addr_b_s;
    trace_a_re <= resize(bf_a_re_s, DATA_W);
    trace_a_im <= resize(bf_a_im_s, DATA_W);
    trace_b_re <= resize(bf_b_re_s, DATA_W);
    trace_b_im <= resize(bf_b_im_s, DATA_W);
  end generate;

  gen_no_trace : if not ENABLE_TRACE generate
    trace_valid <= '0';
    trace_last <= '0';
    trace_stage <= (others => '0');
    trace_addr_a <= (others => '0');
    trace_addr_b <= (others => '0');
    trace_a_re <= (others => '0');
    trace_a_im <= (others => '0');
    trace_b_re <= (others => '0');
    trace_b_im <= (others => '0');
  end generate;

  process(clk)
  begin
    if rising_edge(clk) then
      if rd_en = '1' then
        assert busy_s = '0'
          report "fft_radix2_core: result read requested while busy"
          severity error;
        assert to_integer(rd_idx) < to_integer(active_n_s)
          report "fft_radix2_core: result index outside active transform"
          severity error;
        assert ld_en = '0'
          report "fft_radix2_core: simultaneous result read and new-frame load"
          severity error;
      end if;
    end if;
  end process;
end architecture rtl;
