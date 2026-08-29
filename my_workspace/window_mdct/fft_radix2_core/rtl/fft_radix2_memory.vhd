-------------------------------------------------------------------------------
-- Two-bank, true-dual-port complex memory for the pipelined FFT.
--
-- Each bank has exactly two physical ports after the mode muxes below:
--   port A: load OR compute-A OR result-read;
--   port B: compute-B.
-- During a stage, the source bank performs two reads while the destination
-- bank performs two writes.  The banks swap only after the final P2 result is
-- committed.  This explicit two-port template is intentionally more verbose
-- than independent logical read/write processes because it maps reliably to
-- FPGA true-dual-port block RAM instead of a replicated LUT memory.
--
-- One complex word packs Re[63:32], Im[31:0].  RAM contents are not reset;
-- validity and frame control guarantee that unwritten locations are not read.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.fft_radix2_pkg.all;

entity fft_radix2_memory is
  port (
    clk : in std_logic;

    -- Natural input is written to bit-reversed addresses in bank 0.
    load_we   : in std_logic;
    load_addr : in fft_addr_t;
    load_re   : in fft_data_t;
    load_im   : in fft_data_t;

    -- Two synchronous reads from the current source bank.
    compute_rd_en    : in  std_logic;
    compute_rd_bank  : in  std_logic;
    compute_addr_a   : in  fft_addr_t;
    compute_addr_b   : in  fft_addr_t;
    compute_rd_valid : out std_logic;
    compute_a_re     : out fft_data_t;
    compute_a_im     : out fft_data_t;
    compute_b_re     : out fft_data_t;
    compute_b_im     : out fft_data_t;

    -- Two synchronous writes to the current destination bank.
    compute_wr_en     : in std_logic;
    compute_wr_bank   : in std_logic;
    compute_wr_addr_a : in fft_addr_t;
    compute_wr_addr_b : in fft_addr_t;
    compute_wr_a_re   : in fft_data_t;
    compute_wr_a_im   : in fft_data_t;
    compute_wr_b_re   : in fft_data_t;
    compute_wr_b_im   : in fft_data_t;

    -- Result read shares physical port A.  rd_valid is delayed one clock.
    result_rd_en    : in  std_logic;
    result_rd_bank  : in  std_logic;
    result_rd_addr  : in  fft_addr_t;
    result_rd_valid : out std_logic;
    result_re       : out fft_data_t;
    result_im       : out fft_data_t
  );
end entity fft_radix2_memory;

architecture rtl of fft_radix2_memory is
  type ram_t is array (0 to FFT_MAX_N - 1) of fft_word_t;
  signal bank0, bank1 : ram_t;

  attribute ram_style : string;
  attribute ram_style of bank0 : signal is "block";
  attribute ram_style of bank1 : signal is "block";

  -- Physical-port controls after mutually-exclusive operating-mode muxing.
  signal b0_a_en, b0_a_we, b0_b_en, b0_b_we : std_logic;
  signal b1_a_en, b1_a_we, b1_b_en, b1_b_we : std_logic;
  signal b0_a_addr, b0_b_addr, b1_a_addr, b1_b_addr : fft_addr_t;
  signal b0_a_din, b0_b_din, b1_a_din, b1_b_din : fft_word_t;
  signal b0_a_q, b0_b_q, b1_a_q, b1_b_q : fft_word_t := (others => '0');

  -- Bank selectors follow the one-cycle synchronous RAM read.
  signal compute_bank_r, result_bank_r : std_logic := '0';
  signal compute_valid_r, result_valid_r : std_logic := '0';
  signal compute_a_word, compute_b_word, result_word : fft_word_t;
begin
  compute_rd_valid <= compute_valid_r;
  result_rd_valid <= result_valid_r;

  compute_a_word <= b0_a_q when compute_bank_r = '0' else b1_a_q;
  compute_b_word <= b0_b_q when compute_bank_r = '0' else b1_b_q;
  result_word <= b0_a_q when result_bank_r = '0' else b1_a_q;

  compute_a_re <= word_re(compute_a_word);
  compute_a_im <= word_im(compute_a_word);
  compute_b_re <= word_re(compute_b_word);
  compute_b_im <= word_im(compute_b_word);
  result_re <= word_re(result_word);
  result_im <= word_im(result_word);

  -----------------------------------------------------------------------------
  -- MODE MUXES: reduce load/compute/result requests to two ports per bank.
  -----------------------------------------------------------------------------
  process(all)
  begin
    b0_a_en <= '0'; b0_a_we <= '0';
    b0_b_en <= '0'; b0_b_we <= '0';
    b1_a_en <= '0'; b1_a_we <= '0';
    b1_b_en <= '0'; b1_b_we <= '0';
    b0_a_addr <= (others => '0'); b0_b_addr <= (others => '0');
    b1_a_addr <= (others => '0'); b1_b_addr <= (others => '0');
    b0_a_din <= (others => '0'); b0_b_din <= (others => '0');
    b1_a_din <= (others => '0'); b1_b_din <= (others => '0');

    -- Bank 0, port A: input load has first priority while in LOAD state.
    if load_we = '1' then
      b0_a_en <= '1'; b0_a_we <= '1';
      b0_a_addr <= load_addr;
      b0_a_din <= pack_complex(load_re, load_im);
    elsif compute_rd_en = '1' and compute_rd_bank = '0' then
      b0_a_en <= '1';
      b0_a_addr <= compute_addr_a;
    elsif compute_wr_en = '1' and compute_wr_bank = '0' then
      b0_a_en <= '1'; b0_a_we <= '1';
      b0_a_addr <= compute_wr_addr_a;
      b0_a_din <= pack_complex(compute_wr_a_re, compute_wr_a_im);
    elsif result_rd_en = '1' and result_rd_bank = '0' then
      b0_a_en <= '1';
      b0_a_addr <= result_rd_addr;
    end if;

    -- Bank 0, port B is dedicated to the second compute operand/result.
    if compute_rd_en = '1' and compute_rd_bank = '0' then
      b0_b_en <= '1';
      b0_b_addr <= compute_addr_b;
    elsif compute_wr_en = '1' and compute_wr_bank = '0' then
      b0_b_en <= '1'; b0_b_we <= '1';
      b0_b_addr <= compute_wr_addr_b;
      b0_b_din <= pack_complex(compute_wr_b_re, compute_wr_b_im);
    end if;

    -- Bank 1 uses the same physical-port arrangement, without input load.
    if compute_rd_en = '1' and compute_rd_bank = '1' then
      b1_a_en <= '1';
      b1_a_addr <= compute_addr_a;
    elsif compute_wr_en = '1' and compute_wr_bank = '1' then
      b1_a_en <= '1'; b1_a_we <= '1';
      b1_a_addr <= compute_wr_addr_a;
      b1_a_din <= pack_complex(compute_wr_a_re, compute_wr_a_im);
    elsif result_rd_en = '1' and result_rd_bank = '1' then
      b1_a_en <= '1';
      b1_a_addr <= result_rd_addr;
    end if;

    if compute_rd_en = '1' and compute_rd_bank = '1' then
      b1_b_en <= '1';
      b1_b_addr <= compute_addr_b;
    elsif compute_wr_en = '1' and compute_wr_bank = '1' then
      b1_b_en <= '1'; b1_b_we <= '1';
      b1_b_addr <= compute_wr_addr_b;
      b1_b_din <= pack_complex(compute_wr_b_re, compute_wr_b_im);
    end if;
  end process;

  -----------------------------------------------------------------------------
  -- BANK 0: canonical synchronous true-dual-port RAM template.
  -----------------------------------------------------------------------------
  process(clk)
  begin
    if rising_edge(clk) then
      if b0_a_en = '1' then
        if b0_a_we = '1' then
          bank0(to_integer(b0_a_addr)) <= b0_a_din;
        end if;
        b0_a_q <= bank0(to_integer(b0_a_addr));
      end if;
      if b0_b_en = '1' then
        if b0_b_we = '1' then
          bank0(to_integer(b0_b_addr)) <= b0_b_din;
        end if;
        b0_b_q <= bank0(to_integer(b0_b_addr));
      end if;
    end if;
  end process;

  -----------------------------------------------------------------------------
  -- BANK 1: identical synchronous true-dual-port RAM template.
  -----------------------------------------------------------------------------
  process(clk)
  begin
    if rising_edge(clk) then
      if b1_a_en = '1' then
        if b1_a_we = '1' then
          bank1(to_integer(b1_a_addr)) <= b1_a_din;
        end if;
        b1_a_q <= bank1(to_integer(b1_a_addr));
      end if;
      if b1_b_en = '1' then
        if b1_b_we = '1' then
          bank1(to_integer(b1_b_addr)) <= b1_b_din;
        end if;
        b1_b_q <= bank1(to_integer(b1_b_addr));
      end if;
    end if;
  end process;

  -----------------------------------------------------------------------------
  -- VALID/BANK PIPELINE and protocol assertions; no RAM reset is required.
  -----------------------------------------------------------------------------
  process(clk)
  begin
    if rising_edge(clk) then
      compute_valid_r <= compute_rd_en;
      result_valid_r <= result_rd_en;
      if compute_rd_en = '1' then
        compute_bank_r <= compute_rd_bank;
      end if;
      if result_rd_en = '1' then
        result_bank_r <= result_rd_bank;
      end if;

      assert not (load_we = '1' and
                  (compute_rd_en = '1' or compute_wr_en = '1' or
                   result_rd_en = '1'))
        report "fft_radix2_memory: load overlapped compute/result access"
        severity failure;
      assert not (result_rd_en = '1' and
                  (compute_rd_en = '1' or compute_wr_en = '1'))
        report "fft_radix2_memory: result read overlapped compute access"
        severity failure;
      assert not (compute_rd_en = '1' and compute_wr_en = '1' and
                  compute_rd_bank = compute_wr_bank)
        report "fft_radix2_memory: source and destination banks must differ"
        severity failure;

      if compute_rd_en = '1' then
        assert compute_addr_a /= compute_addr_b
          report "fft_radix2_memory: butterfly read addresses must differ"
          severity failure;
      end if;
      if compute_wr_en = '1' then
        assert compute_wr_addr_a /= compute_wr_addr_b
          report "fft_radix2_memory: butterfly write addresses must differ"
          severity failure;
      end if;
    end if;
  end process;
end architecture rtl;
