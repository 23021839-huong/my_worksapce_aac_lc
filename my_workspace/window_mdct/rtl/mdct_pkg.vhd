-------------------------------------------------------------------------------
-- Shared declarations for the AAC-LC MDCT wrapper around fft_radix2_core.
--
-- Locked numerical profile:
--   PCM          : signed Q1.15, 16 bit
--   data         : signed Q1.31, 32 bit
--   coefficients : signed Q1.15, 16 bit
--   arithmetic   : FDK generic Q15-table path; truncate and wrap, no saturation
--   frame        : AAC-LC 1024 only (one long DCT-IV or eight short DCT-IVs)
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package mdct_pkg is
  constant MDCT_PCM_W       : positive := 16;
  constant MDCT_DATA_W      : positive := 32;
  constant MDCT_COEFF_W     : positive := 16;
  constant MDCT_FRAME_LEN   : positive := 1024;
  constant MDCT_SHORT_LEN   : positive := 128;
  constant MDCT_SNAPSHOT_N  : positive := 2048;
  constant MDCT_SHORT_COUNT : positive := 8;

  -- The values intentionally match FDK-AAC's WINDOW_SEQUENCE enumeration.
  subtype mdct_block_t is unsigned(1 downto 0);
  constant MDCT_LONG  : mdct_block_t := "00";
  constant MDCT_START : mdct_block_t := "01";
  constant MDCT_SHORT : mdct_block_t := "10";
  constant MDCT_STOP  : mdct_block_t := "11";

  subtype mdct_shape_t is std_logic;
  constant MDCT_SINE : mdct_shape_t := '0';
  constant MDCT_KBD  : mdct_shape_t := '1';

  -- size_mode is deliberately identical to fft_radix2_core: 0=64, 1=512.
  constant MDCT_FFT64  : std_logic := '0';
  constant MDCT_FFT512 : std_logic := '1';

  subtype mdct_pcm_t        is signed(MDCT_PCM_W - 1 downto 0);
  subtype mdct_data_t       is signed(MDCT_DATA_W - 1 downto 0);
  subtype mdct_coeff_t      is signed(MDCT_COEFF_W - 1 downto 0);
  subtype mdct_pcm_addr_t   is unsigned(10 downto 0); -- 0..2047
  subtype mdct_data_addr_t  is unsigned(9 downto 0);  -- 0..1023
  subtype mdct_fft_addr_t   is unsigned(8 downto 0);  -- 0..511

  -- Two's-complement modular arithmetic used by the FDK transform datapath.
  function mdct_wrap_q31(value : signed) return mdct_data_t;
  function mdct_add_q31(a, b : mdct_data_t) return mdct_data_t;
  function mdct_sub_q31(a, b : mdct_data_t) return mdct_data_t;
  function mdct_neg_q31(a : mdct_data_t) return mdct_data_t;

  -- FDK generic x86 Q31 x Q15 primitives.  Each product is truncated before
  -- the surrounding complex add/sub, exactly like fixmul.h/cplx_mul.h.
  function mdct_mul_div2_q31_q15(a : mdct_data_t; b : mdct_coeff_t)
    return mdct_data_t;
  function mdct_mul_q31_q15(a : mdct_data_t; b : mdct_coeff_t)
    return mdct_data_t;

  -- Window/fold helpers for PCM Q15.
  function mdct_pcm_shift15(a : mdct_pcm_t) return mdct_data_t;
  function mdct_mul_q15_q15(a, b : mdct_pcm_t) return mdct_data_t;

  -- AAC-LC 1024 block geometry.
  function mdct_is_short(block_type : mdct_block_t) return boolean;
  function mdct_transform_len(block_type : mdct_block_t) return positive;
  function mdct_right_slope_len(block_type : mdct_block_t) return positive;
  function mdct_num_subtransforms(block_type : mdct_block_t) return positive;
  function mdct_fft_size_mode(block_type : mdct_block_t) return std_logic;
  function mdct_output_exp(block_type : mdct_block_t) return natural;
  function mdct_sub_input_base(block_type : mdct_block_t; sub_index : natural)
    return natural;
  function mdct_sub_output_base(block_type : mdct_block_t; sub_index : natural)
    return natural;
end package mdct_pkg;

package body mdct_pkg is
  function mdct_wrap_q31(value : signed) return mdct_data_t is
    variable result : mdct_data_t;
  begin
    assert value'length >= MDCT_DATA_W
      report "mdct_wrap_q31: source must be at least 32 bits"
      severity failure;
    result := signed(value(MDCT_DATA_W - 1 downto 0));
    return result;
  end function;

  function mdct_add_q31(a, b : mdct_data_t) return mdct_data_t is
    variable wide : signed(MDCT_DATA_W downto 0);
  begin
    wide := resize(a, wide'length) + resize(b, wide'length);
    return mdct_wrap_q31(wide);
  end function;

  function mdct_sub_q31(a, b : mdct_data_t) return mdct_data_t is
    variable wide : signed(MDCT_DATA_W downto 0);
  begin
    wide := resize(a, wide'length) - resize(b, wide'length);
    return mdct_wrap_q31(wide);
  end function;

  function mdct_neg_q31(a : mdct_data_t) return mdct_data_t is
    variable wide : signed(MDCT_DATA_W downto 0);
  begin
    wide := -resize(a, wide'length);
    return mdct_wrap_q31(wide);
  end function;

  function mdct_mul_div2_q31_q15(a : mdct_data_t; b : mdct_coeff_t)
    return mdct_data_t is
    variable product : signed(MDCT_DATA_W + MDCT_COEFF_W - 1 downto 0);
    variable shifted : signed(product'range);
  begin
    product := a * b;
    shifted := shift_right(product, MDCT_COEFF_W);
    return mdct_wrap_q31(shifted);
  end function;

  function mdct_mul_q31_q15(a : mdct_data_t; b : mdct_coeff_t)
    return mdct_data_t is
    variable half : mdct_data_t;
    variable wide : signed(MDCT_DATA_W downto 0);
  begin
    half := mdct_mul_div2_q31_q15(a, b);
    wide := shift_left(resize(half, wide'length), 1);
    return mdct_wrap_q31(wide);
  end function;

  function mdct_pcm_shift15(a : mdct_pcm_t) return mdct_data_t is
  begin
    return shift_left(resize(a, MDCT_DATA_W), 15);
  end function;

  function mdct_mul_q15_q15(a, b : mdct_pcm_t) return mdct_data_t is
    variable product : signed(2 * MDCT_PCM_W - 1 downto 0);
  begin
    product := a * b;
    return mdct_data_t(product);
  end function;

  function mdct_is_short(block_type : mdct_block_t) return boolean is
  begin
    return block_type = MDCT_SHORT;
  end function;

  function mdct_transform_len(block_type : mdct_block_t) return positive is
  begin
    if mdct_is_short(block_type) then return MDCT_SHORT_LEN; end if;
    return MDCT_FRAME_LEN;
  end function;

  function mdct_right_slope_len(block_type : mdct_block_t) return positive is
  begin
    if block_type = MDCT_START or block_type = MDCT_SHORT then
      return MDCT_SHORT_LEN;
    end if;
    return MDCT_FRAME_LEN;
  end function;

  function mdct_num_subtransforms(block_type : mdct_block_t) return positive is
  begin
    if mdct_is_short(block_type) then return MDCT_SHORT_COUNT; end if;
    return 1;
  end function;

  function mdct_fft_size_mode(block_type : mdct_block_t) return std_logic is
  begin
    if mdct_is_short(block_type) then return MDCT_FFT64; end if;
    return MDCT_FFT512;
  end function;

  function mdct_output_exp(block_type : mdct_block_t) return natural is
  begin
    if mdct_is_short(block_type) then return 9; end if;
    return 12;
  end function;

  function mdct_sub_input_base(block_type : mdct_block_t; sub_index : natural)
    return natural is
  begin
    if mdct_is_short(block_type) then
      assert sub_index < MDCT_SHORT_COUNT
        report "mdct_sub_input_base: short sub-index outside 0..7"
        severity failure;
      return 448 + MDCT_SHORT_LEN * sub_index;
    end if;
    return 0;
  end function;

  function mdct_sub_output_base(block_type : mdct_block_t; sub_index : natural)
    return natural is
  begin
    if mdct_is_short(block_type) then
      assert sub_index < MDCT_SHORT_COUNT
        report "mdct_sub_output_base: short sub-index outside 0..7"
        severity failure;
      return MDCT_SHORT_LEN * sub_index;
    end if;
    return 0;
  end function;
end package body mdct_pkg;
