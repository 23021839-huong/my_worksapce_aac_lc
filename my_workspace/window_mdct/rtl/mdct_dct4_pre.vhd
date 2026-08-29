-------------------------------------------------------------------------------
-- FDK-compatible pre-rotation for the shared radix-2 implementation of DCT-IV.
--
-- This block buffers one already-folded, Q1.31 DCT-IV vector u[0..L-1].
-- The loader is address based, so the samples may arrive sequentially or in
-- arbitrary order.  The caller must write each active address exactly once;
-- this engine validates bounds/count, while mdct_core supplies natural order.
-- After start, the block emits M=L/2 complex samples in natural FFT order.
--
-- For FFT index m, define
--   t = 2*m                         , m < M/2
--   t = 2*(M-1-m)+1                , m >= M/2
--   a = u[L-1-t] + j*u[t]
--   c = a * (cos_t + j*sin_t)      , Q31xQ15 fMultDiv2 per product
--
-- The natural-order FFT input is
--   z[m].re = ASR(c.im, 1)
--   z[m].im = ASR(c.re, 1)         , m < M/2
--             -ASR(c.re, 1)        , m >= M/2
--
-- This is the exact rearrangement performed in FDK dct_IV before fft().  The
-- fMultDiv2 operations and the final ASR account for the fixed +2 exponent.
-- Add/subtract/negate use two's-complement wrap; no saturation or rounding is
-- introduced here.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_dct4_pre is
  port (
    clk, rst_n : in std_logic;

    -- Runtime DCT-IV length: 0=L128/FFT64, 1=L1024/FFT512.
    size_mode : in std_logic;

    -- Folded-vector loader.  Addresses may be presented in any order; the
    -- caller is responsible for uniqueness and complete active-address cover.
    fold_valid : in  std_logic;
    fold_ready : out std_logic;
    fold_index : in  unsigned(9 downto 0);
    fold_data  : in  mdct_data_t;

    -- start may be asserted with the last accepted input or on a later clock.
    start : in  std_logic;
    busy  : out std_logic;
    done  : out std_logic;

    -- Natural-order complex input stream for fft_radix2_core.
    fft_valid : out std_logic;
    fft_ready : in  std_logic;
    fft_index : out unsigned(8 downto 0);
    fft_re    : out mdct_data_t;
    fft_im    : out mdct_data_t;

    -- Pre-rotation's fixed contribution to the block exponent.
    scale_exp : out unsigned(3 downto 0)
  );
end entity mdct_dct4_pre;

architecture rtl of mdct_dct4_pre is
  constant MODE_L128  : std_logic := '0';
  constant MODE_L1024 : std_logic := '1';

  function dct_length(mode : std_logic) return positive is
  begin
    if mode = MODE_L1024 then
      return 1024;
    end if;
    return 128;
  end function;

  function fft_length(mode : std_logic) return positive is
  begin
    if mode = MODE_L1024 then
      return 512;
    end if;
    return 64;
  end function;

  type state_t is (LOAD_FRAME, FETCH_OPERANDS, CALCULATE, HOLD_OUTPUT);

  signal state_r : state_t := LOAD_FRAME;
  signal load_count_r : natural range 0 to 1024 := 0;
  signal mode_r : std_logic := MODE_L1024;

  signal fft_number_r : natural range 0 to 511 := 0;
  signal read_re_addr_s, read_im_addr_s : unsigned(9 downto 0);
  signal rotation_index_s : unsigned(8 downto 0);
  signal upper_half_s : std_logic;

  signal coeff_cos_r, coeff_sin_r : mdct_coeff_t := (others => '0');
  signal upper_half_r : std_logic := '0';

  signal fold_ram_wr_en_s, fold_ram_rd_en_s, fold_ram_rd_valid_s : std_logic;
  signal fold_ram_re_s, fold_ram_im_s : mdct_data_t;

  signal rom_cos_s, rom_sin_s : signed(15 downto 0);
  signal fold_ready_s : std_logic;
  signal fft_valid_r : std_logic := '0';
  signal fft_index_r : unsigned(8 downto 0) := (others => '0');
  signal fft_re_r, fft_im_r : mdct_data_t := (others => '0');
  signal done_r : std_logic := '0';
begin
  fold_ready <= fold_ready_s;
  fft_valid <= fft_valid_r;
  fft_index <= fft_index_r;
  fft_re <= fft_re_r;
  fft_im <= fft_im_r;
  done <= done_r;
  busy <= '0' when state_r = LOAD_FRAME else '1';
  scale_exp <= to_unsigned(2, 4);

  -- Once loading has begun, mode_r fixes the frame length.  This prevents a
  -- mid-frame size_mode change from altering the ready threshold.
  fold_ready_s <= '1' when
      state_r = LOAD_FRAME and
      load_count_r < dct_length(size_mode) and load_count_r = 0
    else '1' when
      state_r = LOAD_FRAME and
      load_count_r < dct_length(mode_r) and load_count_r /= 0
    else '0';

  fold_ram_wr_en_s <= fold_valid and fold_ready_s;
  fold_ram_rd_en_s <= '1' when state_r = FETCH_OPERANDS else '0';

  -- Explicit true-dual-port work RAM: port A is write/read-A and port B is
  -- read-B.  Load and compute phases do not overlap, so this maps to one BRAM
  -- instead of 1024 scalar registers plus a large read multiplexer.
  u_fold_memory : entity work.mdct_work_memory
    port map (
      clk => clk,
      wr_en => fold_ram_wr_en_s,
      wr_addr => fold_index,
      wr_data => fold_data,
      rd_en => fold_ram_rd_en_s,
      rd_addr_a => read_re_addr_s,
      rd_addr_b => read_im_addr_s,
      rd_valid => fold_ram_rd_valid_s,
      rd_data_a => fold_ram_re_s,
      rd_data_b => fold_ram_im_s
    );

  -----------------------------------------------------------------------------
  -- Address/control mapping from one natural FFT number to the two folded
  -- samples and one FDK WindowSlope coefficient required by the datapath.
  -----------------------------------------------------------------------------
  address_map : process(mode_r, fft_number_r)
    variable m_v, m_len_v, l_len_v, t_v : natural;
  begin
    m_v := fft_number_r;
    m_len_v := fft_length(mode_r);
    l_len_v := dct_length(mode_r);

    if m_v < m_len_v / 2 then
      t_v := 2 * m_v;
      upper_half_s <= '0';
    else
      t_v := 2 * (m_len_v - 1 - m_v) + 1;
      upper_half_s <= '1';
    end if;

    rotation_index_s <= to_unsigned(t_v, rotation_index_s'length);
    read_re_addr_s <= to_unsigned(l_len_v - 1 - t_v,
                                  read_re_addr_s'length);
    read_im_addr_s <= to_unsigned(t_v, read_im_addr_s'length);
  end process address_map;

  u_pre_rotation_rom : entity work.mdct_rotation_rom
    generic map (
      ROM_KIND => 0
    )
    port map (
      size_mode => mode_r,
      rot_index => rotation_index_s,
      cos_q15 => rom_cos_s,
      sin_q15 => rom_sin_s
    );

  -----------------------------------------------------------------------------
  -- CONTROL + DATAPATH
  --
  -- FETCH_OPERANDS issues the explicit BRAM read and registers ROM metadata.
  -- CALCULATE contains four Q31xQ15 multipliers and wrapping add/subtract.
  -- HOLD_OUTPUT guarantees all output fields remain stable during backpressure.
  -----------------------------------------------------------------------------
  p_sequence : process(clk, rst_n)
    variable accepted_v, accepted_last_v : boolean;
    variable target_l_v, input_index_v : natural;
    variable rr_v, is_v, rs_v, ir_v : mdct_data_t;
    variable complex_re_v, complex_im_v : mdct_data_t;
    variable shifted_re_v : mdct_data_t;
  begin
    if rst_n = '0' then
      state_r <= LOAD_FRAME;
      load_count_r <= 0;
      mode_r <= MODE_L1024;
      fft_number_r <= 0;
      coeff_cos_r <= (others => '0');
      coeff_sin_r <= (others => '0');
      upper_half_r <= '0';
      fft_valid_r <= '0';
      fft_index_r <= (others => '0');
      fft_re_r <= (others => '0');
      fft_im_r <= (others => '0');
      done_r <= '0';
    elsif rising_edge(clk) then
      done_r <= '0';

      case state_r is
        when LOAD_FRAME =>
          fft_valid_r <= '0';
          accepted_v := fold_valid = '1' and fold_ready_s = '1';
          accepted_last_v := false;

          if load_count_r = 0 then
            target_l_v := dct_length(size_mode);
          else
            target_l_v := dct_length(mode_r);
          end if;

          if accepted_v then
            input_index_v := to_integer(fold_index);
            assert input_index_v < target_l_v
              report "mdct_dct4_pre: fold_index outside active transform"
              severity failure;

            if input_index_v < target_l_v then
              if load_count_r = 0 then
                mode_r <= size_mode;
              else
                assert size_mode = mode_r
                  report "mdct_dct4_pre: size_mode changed while loading"
                  severity failure;
              end if;

              load_count_r <= load_count_r + 1;
              accepted_last_v := load_count_r + 1 = target_l_v;
            end if;
          end if;

          if start = '1' then
            if load_count_r = target_l_v or accepted_last_v then
              if load_count_r /= 0 then
                assert size_mode = mode_r
                  report "mdct_dct4_pre: size_mode differs at start"
                  severity failure;
              end if;
              fft_number_r <= 0;
              state_r <= FETCH_OPERANDS;
            else
              assert false
                report "mdct_dct4_pre: start ignored before complete frame"
                severity warning;
            end if;
          end if;

        when FETCH_OPERANDS =>
          -- The memory entity registers both reads on this edge.
          coeff_cos_r <= rom_cos_s;
          coeff_sin_r <= rom_sin_s;
          upper_half_r <= upper_half_s;
          state_r <= CALCULATE;

        when CALCULATE =>
          assert fold_ram_rd_valid_s = '1'
            report "mdct_dct4_pre: missing folded-RAM response"
            severity failure;
          -- Exact generic-FDK cplxMultDiv2: truncate every product before the
          -- wrapping complex add/subtract.
          rr_v := mdct_mul_div2_q31_q15(fold_ram_re_s, coeff_cos_r);
          is_v := mdct_mul_div2_q31_q15(fold_ram_im_s, coeff_sin_r);
          rs_v := mdct_mul_div2_q31_q15(fold_ram_re_s, coeff_sin_r);
          ir_v := mdct_mul_div2_q31_q15(fold_ram_im_s, coeff_cos_r);
          complex_re_v := mdct_sub_q31(rr_v, is_v);
          complex_im_v := mdct_add_q31(rs_v, ir_v);

          fft_re_r <= shift_right(complex_im_v, 1);
          shifted_re_v := shift_right(complex_re_v, 1);
          if upper_half_r = '1' then
            fft_im_r <= mdct_neg_q31(shifted_re_v);
          else
            fft_im_r <= shifted_re_v;
          end if;
          fft_index_r <= to_unsigned(fft_number_r, fft_index_r'length);
          fft_valid_r <= '1';
          state_r <= HOLD_OUTPUT;

        when HOLD_OUTPUT =>
          if fft_ready = '1' then
            fft_valid_r <= '0';
            if fft_number_r = fft_length(mode_r) - 1 then
              -- done denotes acceptance, not merely presentation, of the last
              -- complex sample.
              done_r <= '1';
              load_count_r <= 0;
              fft_number_r <= 0;
              state_r <= LOAD_FRAME;
            else
              fft_number_r <= fft_number_r + 1;
              state_r <= FETCH_OPERANDS;
            end if;
          end if;
      end case;
    end if;
  end process p_sequence;
end architecture rtl;
