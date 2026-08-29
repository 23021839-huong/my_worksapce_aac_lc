-------------------------------------------------------------------------------
-- FDK-compatible post-rotation/reordering for radix-2 DCT-IV.
--
-- One frame of M=L/2 natural-order complex FFT bins is buffered.  After start,
-- L Q1.31 DCT-IV samples are emitted in the exact in-place order produced by
-- FDK dct_IV.  The post-rotation does not change the block exponent.
--
-- For i=1..M/2-1, w_i is SineTable1024[i*step], with physical
-- step=2 for L=1024 and step=16 for L=128:
--   high = z[M-i] * w_i
--   low  = (z[i].im + j*z[i].re) * w_i
--   y[2*i-1]       = high.re
--   y[2*i]         = low.im
--   y[L-1-2*i]     = -low.re
--   y[L-2*i]       = high.im
-- with y[0], y[L-1], and the two centre samples handled explicitly below.
-- The compact ROM stores every second SineTable1024 entry.  Its internal
-- address stride is therefore one for L=1024 and eight for L=128.
--
-- Every Q31xQ15 product follows FDK fMult (fMultDiv2 then wrapping <<1),
-- truncating each product before the wrapping complex add/subtract.  There is
-- no rounding, saturation, or implicit normalization in this block.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_dct4_post is
  port (
    clk, rst_n : in std_logic;

    -- Runtime DCT-IV length: 0=L128/FFT64, 1=L1024/FFT512.
    size_mode : in std_logic;

    -- Natural-order FFT-bin loader.  Addresses may be presented in any order;
    -- the caller is responsible for uniqueness and complete active-bin cover.
    bin_valid : in  std_logic;
    bin_ready : out std_logic;
    bin_index : in  unsigned(8 downto 0);
    bin_re    : in  mdct_data_t;
    bin_im    : in  mdct_data_t;

    -- start may be asserted with the last accepted bin or on a later clock.
    start : in  std_logic;
    busy  : out std_logic;
    done  : out std_logic;

    -- Natural FDK DCT-IV scalar order, stable while sample_ready is low.
    sample_valid : out std_logic;
    sample_ready : in  std_logic;
    sample_index : out unsigned(9 downto 0);
    sample_data  : out mdct_data_t
  );
end entity mdct_dct4_post;

architecture rtl of mdct_dct4_post is
  constant MODE_L128  : std_logic := '0';
  constant MODE_L1024 : std_logic := '1';
  constant SQRT_HALF_Q15 : mdct_coeff_t := signed'(x"5A82");

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

  type state_t is (LOAD_FRAME, FETCH_OPERAND, CALCULATE, HOLD_OUTPUT);
  type operation_t is (
    DIRECT_RE,
    DIRECT_NEG_IM,
    MIDDLE_SUB,
    MIDDLE_ADD,
    HIGH_REAL,
    HIGH_IMAG,
    LOW_IMAG,
    LOW_NEG_REAL
  );
  signal state_r : state_t := LOAD_FRAME;
  signal load_count_r : natural range 0 to 512 := 0;
  signal mode_r : std_logic := MODE_L1024;

  signal sample_number_r : natural range 0 to 1023 := 0;
  signal read_bin_index_s : unsigned(8 downto 0);
  signal rotation_index_s : unsigned(8 downto 0);
  signal operation_s : operation_t;

  signal coeff_cos_r, coeff_sin_r : mdct_coeff_t := (others => '0');
  signal operation_r : operation_t := DIRECT_RE;

  signal bin_ram_wr_en_s, bin_ram_rd_en_s, bin_ram_rd_valid_s : std_logic;
  signal bin_ram_re_s, bin_ram_im_s : mdct_data_t;

  signal rom_cos_s, rom_sin_s : signed(15 downto 0);
  signal bin_ready_s : std_logic;
  signal sample_valid_r : std_logic := '0';
  signal sample_index_r : unsigned(9 downto 0) := (others => '0');
  signal sample_data_r : mdct_data_t := (others => '0');
  signal done_r : std_logic := '0';
begin
  bin_ready <= bin_ready_s;
  sample_valid <= sample_valid_r;
  sample_index <= sample_index_r;
  sample_data <= sample_data_r;
  done <= done_r;
  busy <= '0' when state_r = LOAD_FRAME else '1';

  -- As in the pre block, the size is fixed by the first accepted transaction.
  bin_ready_s <= '1' when
      state_r = LOAD_FRAME and
      load_count_r < fft_length(size_mode) and load_count_r = 0
    else '1' when
      state_r = LOAD_FRAME and
      load_count_r < fft_length(mode_r) and load_count_r /= 0
    else '0';

  bin_ram_wr_en_s <= bin_valid and bin_ready_s;
  bin_ram_rd_en_s <= '1' when state_r = FETCH_OPERAND else '0';

  u_bin_memory : entity work.mdct_fft_cache_memory
    port map (
      clk => clk,
      wr_en => bin_ram_wr_en_s,
      wr_addr => bin_index,
      wr_re => bin_re,
      wr_im => bin_im,
      rd_en => bin_ram_rd_en_s,
      rd_addr => read_bin_index_s,
      rd_valid => bin_ram_rd_valid_s,
      rd_re => bin_ram_re_s,
      rd_im => bin_ram_im_s
    );

  -----------------------------------------------------------------------------
  -- Invert the FDK in-place post-rotation stores to determine, for each output
  -- number n, which FFT bin and which member of the complex product are used.
  -----------------------------------------------------------------------------
  output_map : process(mode_r, sample_number_r)
    variable n_v, i_v, bin_v, m_len_v, l_len_v : natural;
  begin
    n_v := sample_number_r;
    m_len_v := fft_length(mode_r);
    l_len_v := dct_length(mode_r);
    i_v := 0;
    bin_v := 0;
    operation_s <= DIRECT_RE;

    if n_v = 0 then
      bin_v := 0;
      operation_s <= DIRECT_RE;
    elsif n_v = l_len_v - 1 then
      bin_v := 0;
      operation_s <= DIRECT_NEG_IM;
    elsif n_v = m_len_v - 1 then
      bin_v := m_len_v / 2;
      operation_s <= MIDDLE_SUB;
    elsif n_v = m_len_v then
      bin_v := m_len_v / 2;
      operation_s <= MIDDLE_ADD;
    elsif n_v < m_len_v - 1 then
      if (n_v mod 2) = 1 then
        i_v := (n_v + 1) / 2;
        bin_v := m_len_v - i_v;
        operation_s <= HIGH_REAL;
      else
        i_v := n_v / 2;
        bin_v := i_v;
        operation_s <= LOW_IMAG;
      end if;
    else
      if (n_v mod 2) = 0 then
        i_v := (l_len_v - n_v) / 2;
        bin_v := m_len_v - i_v;
        operation_s <= HIGH_IMAG;
      else
        i_v := (l_len_v - 1 - n_v) / 2;
        bin_v := i_v;
        operation_s <= LOW_NEG_REAL;
      end if;
    end if;

    read_bin_index_s <= to_unsigned(bin_v, read_bin_index_s'length);
    rotation_index_s <= to_unsigned(i_v, rotation_index_s'length);
  end process output_map;

  u_post_rotation_rom : entity work.mdct_rotation_rom
    generic map (
      ROM_KIND => 1
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
  -- FETCH_OPERAND issues the explicit BRAM read and captures ROM metadata.
  -- CALCULATE implements one
  -- of the eight FDK store cases.  HOLD_OUTPUT provides normal ready/valid
  -- backpressure and pulses done only when sample L-1 is accepted.
  -----------------------------------------------------------------------------
  p_sequence : process(clk, rst_n)
    variable accepted_v, accepted_last_v : boolean;
    variable target_m_v, input_index_v : natural;
    variable rr_v, is_v, rs_v, ir_v : mdct_data_t;
    variable complex_re_v, complex_im_v : mdct_data_t;
    variable middle_re_v, middle_im_v : mdct_data_t;
    variable result_v : mdct_data_t;
  begin
    if rst_n = '0' then
      state_r <= LOAD_FRAME;
      load_count_r <= 0;
      mode_r <= MODE_L1024;
      sample_number_r <= 0;
      coeff_cos_r <= (others => '0');
      coeff_sin_r <= (others => '0');
      operation_r <= DIRECT_RE;
      sample_valid_r <= '0';
      sample_index_r <= (others => '0');
      sample_data_r <= (others => '0');
      done_r <= '0';
    elsif rising_edge(clk) then
      done_r <= '0';

      case state_r is
        when LOAD_FRAME =>
          sample_valid_r <= '0';
          accepted_v := bin_valid = '1' and bin_ready_s = '1';
          accepted_last_v := false;

          if load_count_r = 0 then
            target_m_v := fft_length(size_mode);
          else
            target_m_v := fft_length(mode_r);
          end if;

          if accepted_v then
            input_index_v := to_integer(bin_index);
            assert input_index_v < target_m_v
              report "mdct_dct4_post: bin_index outside active transform"
              severity failure;

            if input_index_v < target_m_v then
              if load_count_r = 0 then
                mode_r <= size_mode;
              else
                assert size_mode = mode_r
                  report "mdct_dct4_post: size_mode changed while loading"
                  severity failure;
              end if;

              load_count_r <= load_count_r + 1;
              accepted_last_v := load_count_r + 1 = target_m_v;
            end if;
          end if;

          if start = '1' then
            if load_count_r = target_m_v or accepted_last_v then
              if load_count_r /= 0 then
                assert size_mode = mode_r
                  report "mdct_dct4_post: size_mode differs at start"
                  severity failure;
              end if;
              sample_number_r <= 0;
              state_r <= FETCH_OPERAND;
            else
              assert false
                report "mdct_dct4_post: start ignored before complete frame"
                severity warning;
            end if;
          end if;

        when FETCH_OPERAND =>
          coeff_cos_r <= rom_cos_s;
          coeff_sin_r <= rom_sin_s;
          operation_r <= operation_s;
          state_r <= CALCULATE;

        when CALCULATE =>
          assert bin_ram_rd_valid_s = '1'
            report "mdct_dct4_post: missing FFT-cache response"
            severity failure;
          result_v := (others => '0');

          case operation_r is
            when DIRECT_RE =>
              result_v := bin_ram_re_s;

            when DIRECT_NEG_IM =>
              result_v := mdct_neg_q31(bin_ram_im_s);

            when MIDDLE_SUB | MIDDLE_ADD =>
              middle_re_v := mdct_mul_q31_q15(bin_ram_re_s,
                                               SQRT_HALF_Q15);
              middle_im_v := mdct_mul_q31_q15(bin_ram_im_s,
                                               SQRT_HALF_Q15);
              if operation_r = MIDDLE_SUB then
                result_v := mdct_sub_q31(middle_re_v, middle_im_v);
              else
                result_v := mdct_add_q31(middle_re_v, middle_im_v);
              end if;

            when HIGH_REAL | HIGH_IMAG | LOW_IMAG | LOW_NEG_REAL =>
              -- Four individually truncated FDK fMult products.  These terms
              -- serve both z*w (HIGH) and (Im+j*Re)*w (LOW).
              rr_v := mdct_mul_q31_q15(bin_ram_re_s, coeff_cos_r);
              is_v := mdct_mul_q31_q15(bin_ram_im_s, coeff_sin_r);
              rs_v := mdct_mul_q31_q15(bin_ram_re_s, coeff_sin_r);
              ir_v := mdct_mul_q31_q15(bin_ram_im_s, coeff_cos_r);

              if operation_r = HIGH_REAL or operation_r = HIGH_IMAG then
                complex_re_v := mdct_sub_q31(rr_v, is_v);
                complex_im_v := mdct_add_q31(rs_v, ir_v);
              else
                complex_re_v := mdct_sub_q31(ir_v, rs_v);
                complex_im_v := mdct_add_q31(is_v, rr_v);
              end if;

              case operation_r is
                when HIGH_REAL =>
                  result_v := complex_re_v;
                when HIGH_IMAG | LOW_IMAG =>
                  result_v := complex_im_v;
                when LOW_NEG_REAL =>
                  result_v := mdct_neg_q31(complex_re_v);
                when others =>
                  result_v := (others => '0');
              end case;
          end case;

          sample_data_r <= result_v;
          sample_index_r <= to_unsigned(sample_number_r,
                                        sample_index_r'length);
          sample_valid_r <= '1';
          state_r <= HOLD_OUTPUT;

        when HOLD_OUTPUT =>
          if sample_ready = '1' then
            sample_valid_r <= '0';
            if sample_number_r = dct_length(mode_r) - 1 then
              done_r <= '1';
              load_count_r <= 0;
              sample_number_r <= 0;
              state_r <= LOAD_FRAME;
            else
              sample_number_r <= sample_number_r + 1;
              state_r <= FETCH_OPERAND;
            end if;
          end if;
      end case;
    end if;
  end process p_sequence;
end architecture rtl;
