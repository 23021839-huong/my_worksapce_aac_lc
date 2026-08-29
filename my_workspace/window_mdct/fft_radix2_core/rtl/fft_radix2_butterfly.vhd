-------------------------------------------------------------------------------
-- Three-register-stage, one-butterfly-per-clock radix-2 datapath.
--
-- Pipeline:
--   P0 CAPTURE : latch operands, phase metadata and destination addresses.
--   P1 MULTIPLY: four Q31xQ15 products in parallel, or exact W=1/-j bypass.
--   P2 ADD/SUB : finish the radix-2 butterfly and register both outputs.
--
-- Scaling is fixed to the FDK FFT64/512 contract:
--   stage 1 : one-bit scale, exact first half of the fused FDK radix-4 kernel;
--   stage 2 : no scale, W is exactly 1 or -j;
--   stage 3+: A>>1 and complex_mul_div2(B,W).
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.fft_radix2_pkg.all;

entity fft_radix2_butterfly is
  port (
    clk, rst_n : in std_logic;

    in_valid     : in std_logic;
    in_last      : in std_logic;
    in_stage     : in unsigned(3 downto 0);
    in_addr_a    : in fft_addr_t;
    in_addr_b    : in fft_addr_t;
    in_phase     : in unsigned(7 downto 0);
    in_cos_mag   : in fft_coeff_t;
    in_sin_mag   : in fft_coeff_t;
    in_cos_neg   : in std_logic;
    in_a_re      : in fft_data_t;
    in_a_im      : in fft_data_t;
    in_b_re      : in fft_data_t;
    in_b_im      : in fft_data_t;

    out_valid    : out std_logic;
    out_last     : out std_logic;
    out_stage    : out unsigned(3 downto 0);
    out_addr_a   : out fft_addr_t;
    out_addr_b   : out fft_addr_t;
    out_a_re     : out fft_data_t;
    out_a_im     : out fft_data_t;
    out_b_re     : out fft_data_t;
    out_b_im     : out fft_data_t
  );
end entity fft_radix2_butterfly;

architecture rtl of fft_radix2_butterfly is
  -- P0: raw inputs aligned to the one-cycle synchronous memory read.
  signal p0_valid_r, p0_last_r : std_logic := '0';
  signal p0_stage_r : unsigned(3 downto 0) := (others => '0');
  signal p0_addr_a_r, p0_addr_b_r : fft_addr_t := (others => '0');
  signal p0_phase_r : unsigned(7 downto 0) := (others => '0');
  signal p0_cos_r, p0_sin_r : fft_coeff_t := (others => '0');
  signal p0_cos_neg_r : std_logic := '0';
  signal p0_a_re_r, p0_a_im_r, p0_b_re_r, p0_b_im_r : fft_data_t :=
    (others => '0');

  -- P1: A at its stage scale and the already-truncated B*W result.
  signal p1_valid_r, p1_last_r, p1_stage1_r : std_logic := '0';
  signal p1_stage_r : unsigned(3 downto 0) := (others => '0');
  signal p1_addr_a_r, p1_addr_b_r : fft_addr_t := (others => '0');
  signal p1_u_re_r, p1_u_im_r, p1_t_re_r, p1_t_im_r : fft_data_t :=
    (others => '0');
  signal p1_stage1_b_re_r, p1_stage1_b_im_r : fft_data_t := (others => '0');

  -- P2/output registers.
  signal p2_valid_r, p2_last_r : std_logic := '0';
  signal p2_stage_r : unsigned(3 downto 0) := (others => '0');
  signal p2_addr_a_r, p2_addr_b_r : fft_addr_t := (others => '0');
  signal p2_a_re_r, p2_a_im_r, p2_b_re_r, p2_b_im_r : fft_data_t :=
    (others => '0');
begin
  out_valid <= p2_valid_r;
  out_last <= p2_last_r;
  out_stage <= p2_stage_r;
  out_addr_a <= p2_addr_a_r;
  out_addr_b <= p2_addr_b_r;
  out_a_re <= p2_a_re_r;
  out_a_im <= p2_a_im_r;
  out_b_re <= p2_b_re_r;
  out_b_im <= p2_b_im_r;

  process(clk, rst_n)
    variable product_br_c, product_bi_c : fft_data_t;
    variable product_br_s, product_bi_s : fft_data_t;
    variable signed_br_c, signed_bi_c : signed(FFT_DATA_W downto 0);
    variable t_re_wide, t_im_wide : signed(FFT_DATA_W downto 0);
    variable top_re_wide, top_im_wide : signed(FFT_DATA_W downto 0);
    variable bot_re_wide, bot_im_wide : signed(FFT_DATA_W downto 0);
    variable sum_re_wide, sum_im_wide : signed(FFT_DATA_W downto 0);
    variable stage1_top_re, stage1_top_im : fft_data_t;
  begin
    if rst_n = '0' then
      p0_valid_r <= '0';
      p1_valid_r <= '0';
      p2_valid_r <= '0';
      p0_last_r <= '0';
      p1_last_r <= '0';
      p2_last_r <= '0';
    elsif rising_edge(clk) then
      -------------------------------------------------------------------------
      -- P0: capture one complete butterfly transaction.
      -------------------------------------------------------------------------
      p0_valid_r <= in_valid;
      if in_valid = '1' then
        p0_last_r <= in_last;
        p0_stage_r <= in_stage;
        p0_addr_a_r <= in_addr_a;
        p0_addr_b_r <= in_addr_b;
        p0_phase_r <= in_phase;
        p0_cos_r <= in_cos_mag;
        p0_sin_r <= in_sin_mag;
        p0_cos_neg_r <= in_cos_neg;
        p0_a_re_r <= in_a_re;
        p0_a_im_r <= in_a_im;
        p0_b_re_r <= in_b_re;
        p0_b_im_r <= in_b_im;
      end if;

      -------------------------------------------------------------------------
      -- P1: multiplier/bypass stage.
      -------------------------------------------------------------------------
      p1_valid_r <= p0_valid_r;
      if p0_valid_r = '1' then
        p1_last_r <= p0_last_r;
        p1_stage_r <= p0_stage_r;
        p1_addr_a_r <= p0_addr_a_r;
        p1_addr_b_r <= p0_addr_b_r;
        p1_stage1_r <= '0';
        p1_stage1_b_re_r <= p0_b_re_r;
        p1_stage1_b_im_r <= p0_b_im_r;

        if to_integer(p0_stage_r) = 1 then
          -- P2 performs the exact FDK ordering:
          --   sum=(A+B)>>1; difference=sum-B.
          p1_stage1_r <= '1';
          p1_u_re_r <= p0_a_re_r;
          p1_u_im_r <= p0_a_im_r;
          p1_t_re_r <= p0_b_re_r;
          p1_t_im_r <= p0_b_im_r;
        elsif to_integer(p0_stage_r) = 2 then
          -- Stage 2 is unscaled and contains only W=1 or W=-j.  Bypass avoids
          -- replacing exact unity by the Q15 approximation 0x7FFF.
          p1_u_re_r <= p0_a_re_r;
          p1_u_im_r <= p0_a_im_r;
          if p0_phase_r = x"00" then
            p1_t_re_r <= p0_b_re_r;
            p1_t_im_r <= p0_b_im_r;
          else
            assert p0_phase_r = x"80"
              report "fft_radix2_butterfly: stage 2 phase must be 0 or 128"
              severity failure;
            p1_t_re_r <= p0_b_im_r;
            p1_t_im_r <= wrap_q31(-resize(p0_b_re_r, FFT_DATA_W + 1));
          end if;
        else
          p1_u_re_r <= shift_right(p0_a_re_r, 1);
          p1_u_im_r <= shift_right(p0_a_im_r, 1);

          if p0_phase_r = x"00" then
            -- FDK has a dedicated W=1 branch and shifts both operands before
            -- add/sub.  Multiplying by 0x7FFF would lose significant LSBs.
            p1_t_re_r <= shift_right(p0_b_re_r, 1);
            p1_t_im_r <= shift_right(p0_b_im_r, 1);
          elsif p0_phase_r = x"80" then
            -- Exact W=-j branch used in every FDK stage.  The negation occurs
            -- after the arithmetic shift and can therefore not be commuted.
            p1_t_re_r <= shift_right(p0_b_im_r, 1);
            p1_t_im_r <= wrap_q31(
              -resize(shift_right(p0_b_re_r, 1), FFT_DATA_W + 1));
          else
            -- Four products operate in parallel.  Each result is truncated
            -- before signs and the final add/sub are applied.
            product_br_c := mul_div2_q31_q15(p0_b_re_r, p0_cos_r);
            product_bi_c := mul_div2_q31_q15(p0_b_im_r, p0_cos_r);
            product_br_s := mul_div2_q31_q15(p0_b_re_r, p0_sin_r);
            product_bi_s := mul_div2_q31_q15(p0_b_im_r, p0_sin_r);

            if p0_cos_neg_r = '1' then
              signed_br_c := -resize(product_br_c, FFT_DATA_W + 1);
              signed_bi_c := -resize(product_bi_c, FFT_DATA_W + 1);
            else
              signed_br_c := resize(product_br_c, FFT_DATA_W + 1);
              signed_bi_c := resize(product_bi_c, FFT_DATA_W + 1);
            end if;

            -- (br+j*bi)*(cos-j*sin):
            --   re = br*cos + bi*sin
            --   im = bi*cos - br*sin
            t_re_wide := signed_br_c + resize(product_bi_s, FFT_DATA_W + 1);
            t_im_wide := signed_bi_c - resize(product_br_s, FFT_DATA_W + 1);
            p1_t_re_r <= wrap_q31(t_re_wide);
            p1_t_im_r <= wrap_q31(t_im_wide);
          end if;
        end if;
      end if;

      -------------------------------------------------------------------------
      -- P2: final add/sub and output registration.
      -------------------------------------------------------------------------
      p2_valid_r <= p1_valid_r;
      if p1_valid_r = '1' then
        p2_last_r <= p1_last_r;
        p2_stage_r <= p1_stage_r;
        p2_addr_a_r <= p1_addr_a_r;
        p2_addr_b_r <= p1_addr_b_r;

        if p1_stage1_r = '1' then
          sum_re_wide := resize(p1_u_re_r, FFT_DATA_W + 1) +
                         resize(p1_t_re_r, FFT_DATA_W + 1);
          sum_im_wide := resize(p1_u_im_r, FFT_DATA_W + 1) +
                         resize(p1_t_im_r, FFT_DATA_W + 1);
          stage1_top_re := wrap_q31(shift_right(sum_re_wide, 1));
          stage1_top_im := wrap_q31(shift_right(sum_im_wide, 1));
          p2_a_re_r <= stage1_top_re;
          p2_a_im_r <= stage1_top_im;
          p2_b_re_r <= wrap_q31(resize(stage1_top_re, FFT_DATA_W + 1) -
                                resize(p1_stage1_b_re_r, FFT_DATA_W + 1));
          p2_b_im_r <= wrap_q31(resize(stage1_top_im, FFT_DATA_W + 1) -
                                resize(p1_stage1_b_im_r, FFT_DATA_W + 1));
        else
          top_re_wide := resize(p1_u_re_r, FFT_DATA_W + 1) +
                         resize(p1_t_re_r, FFT_DATA_W + 1);
          top_im_wide := resize(p1_u_im_r, FFT_DATA_W + 1) +
                         resize(p1_t_im_r, FFT_DATA_W + 1);
          bot_re_wide := resize(p1_u_re_r, FFT_DATA_W + 1) -
                         resize(p1_t_re_r, FFT_DATA_W + 1);
          bot_im_wide := resize(p1_u_im_r, FFT_DATA_W + 1) -
                         resize(p1_t_im_r, FFT_DATA_W + 1);
          p2_a_re_r <= wrap_q31(top_re_wide);
          p2_a_im_r <= wrap_q31(top_im_wide);
          p2_b_re_r <= wrap_q31(bot_re_wide);
          p2_b_im_r <= wrap_q31(bot_im_wide);
        end if;
      end if;
    end if;
  end process;
end architecture rtl;
