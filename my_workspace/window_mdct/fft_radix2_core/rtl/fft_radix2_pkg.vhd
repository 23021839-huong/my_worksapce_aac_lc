-------------------------------------------------------------------------------
-- Shared declarations for the FDK-compatible radix-2 FFT.
--
-- Numerical profile:
--   data        : signed Q1.31 (32 bit)
--   coefficient : signed Q1.15 (16 bit)
--   multiply    : Q31xQ15 fMultDiv2 = arithmetic_shift(product, 16)
--   overflow    : two's-complement wrap; no saturation
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package fft_radix2_pkg is
  constant FFT_DATA_W  : positive := 32;
  constant FFT_COEFF_W : positive := 16;
  constant FFT_MAX_N   : positive := 512;
  constant FFT_MAX_LDN : positive := 9;

  constant MODE_FFT64  : std_logic := '0';
  constant MODE_FFT512 : std_logic := '1';

  subtype fft_data_t  is signed(FFT_DATA_W - 1 downto 0);
  subtype fft_coeff_t is signed(FFT_COEFF_W - 1 downto 0);
  subtype fft_addr_t  is unsigned(8 downto 0);
  subtype fft_word_t  is std_logic_vector(2 * FFT_DATA_W - 1 downto 0);

  function bit_reverse_index(index_in : fft_addr_t; ldn : natural)
    return fft_addr_t;
  function wrap_q31(value : signed) return fft_data_t;
  function mul_div2_q31_q15(a : fft_data_t; b : fft_coeff_t)
    return fft_data_t;
  function pack_complex(re_value, im_value : fft_data_t) return fft_word_t;
  function word_re(word : fft_word_t) return fft_data_t;
  function word_im(word : fft_word_t) return fft_data_t;
end package fft_radix2_pkg;

package body fft_radix2_pkg is
  function bit_reverse_index(index_in : fft_addr_t; ldn : natural)
    return fft_addr_t is
    variable result : fft_addr_t := (others => '0');
  begin
    assert ldn >= 1 and ldn <= FFT_MAX_LDN
      report "bit_reverse_index: ldn outside supported range"
      severity failure;
    for bit_index in 0 to ldn - 1 loop
      result(ldn - 1 - bit_index) := index_in(bit_index);
    end loop;
    return result;
  end function;

  -- numeric_std.resize() preserves the original sign bit when shrinking a
  -- signed vector.  That is not modulo wrap, therefore the low 32 bits are
  -- selected explicitly here.
  function wrap_q31(value : signed) return fft_data_t is
    variable result : fft_data_t;
  begin
    assert value'length >= FFT_DATA_W
      report "wrap_q31: source must be at least 32 bits"
      severity failure;
    result := signed(value(FFT_DATA_W - 1 downto 0));
    return result;
  end function;

  -- Exact generic-x86 FDK operation for FIXP_DBL x FIXP_SGL fMultDiv2.
  -- There is deliberately no rounding bias before the arithmetic shift.
  function mul_div2_q31_q15(a : fft_data_t; b : fft_coeff_t)
    return fft_data_t is
    variable product : signed(FFT_DATA_W + FFT_COEFF_W - 1 downto 0);
    variable shifted : signed(product'range);
  begin
    product := a * b;
    shifted := shift_right(product, FFT_COEFF_W);
    return wrap_q31(shifted);
  end function;

  function pack_complex(re_value, im_value : fft_data_t) return fft_word_t is
    variable result : fft_word_t;
  begin
    result(2 * FFT_DATA_W - 1 downto FFT_DATA_W) :=
      std_logic_vector(re_value);
    result(FFT_DATA_W - 1 downto 0) := std_logic_vector(im_value);
    return result;
  end function;

  function word_re(word : fft_word_t) return fft_data_t is
  begin
    return signed(word(2 * FFT_DATA_W - 1 downto FFT_DATA_W));
  end function;

  function word_im(word : fft_word_t) return fft_data_t is
  begin
    return signed(word(FFT_DATA_W - 1 downto 0));
  end function;
end package body fft_radix2_pkg;
