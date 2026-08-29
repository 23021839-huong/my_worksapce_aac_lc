-------------------------------------------------------------------------------
-- FDK-compatible analysis-window and TDAC-fold engine.
--
-- For each scalar output j this block issues at most two PCM reads and one
-- slope-ROM read.  The address map is the four-loop mdct_block() implementation
-- rewritten in natural output order:
--
--   [0, fr/2)             : -(C*w.re + Dr*w.im), reversed placement
--   [fr/2, L/2)           : -C, reversed placement (right zero slope)
--   [L/2, L/2+nl)         : -Br                    (left zero slope)
--   [L/2+nl, L)           :  A*w.im - Br*w.re
--
-- The output has exponent 2, matching FDK's window/fold boundary.  Arithmetic
-- is PCM Q15 x coefficient Q15 -> Q31, truncation-free at this width, followed
-- by two's-complement wrap for add/sub/negate.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_window_fold is
  port (
    clk, rst_n : in std_logic;

    start           : in std_logic;
    size_mode       : in std_logic; -- 0=L128, 1=L1024
    input_base      : in mdct_pcm_addr_t;
    left_slope_len  : in unsigned(10 downto 0);
    right_slope_len : in unsigned(10 downto 0);
    left_shape      : in mdct_shape_t;
    right_shape     : in mdct_shape_t;

    busy : out std_logic;
    done : out std_logic;

    -- Registered two-port PCM memory request/response.
    pcm_rd_en     : out std_logic;
    pcm_rd_addr_a : out mdct_pcm_addr_t;
    pcm_rd_addr_b : out mdct_pcm_addr_t;
    pcm_rd_valid  : in  std_logic;
    pcm_rd_data_a : in  mdct_pcm_t;
    pcm_rd_data_b : in  mdct_pcm_t;

    -- Natural-order folded output, one value per valid cycle.
    out_valid : out std_logic;
    out_last  : out std_logic;
    out_index : out mdct_data_addr_t;
    out_data  : out mdct_data_t;
    out_exp   : out unsigned(3 downto 0)
  );
end entity mdct_window_fold;

architecture rtl of mdct_window_fold is
  type state_t is (S_IDLE, S_RUN);
  type region_t is (R_RIGHT_WINDOW, R_RIGHT_ZERO, R_LEFT_ZERO, R_LEFT_WINDOW);

  signal state_r : state_t := S_IDLE;
  signal size_mode_r : std_logic := MDCT_FFT64;
  signal base_r : natural range 0 to MDCT_SNAPSHOT_N - 1 := 0;
  signal fl_r, fr_r : natural range 0 to MDCT_FRAME_LEN := 0;
  signal left_shape_r, right_shape_r : mdct_shape_t := MDCT_SINE;
  signal transform_len_r : natural range MDCT_SHORT_LEN to MDCT_FRAME_LEN :=
    MDCT_SHORT_LEN;
  signal issue_index_r : natural range 0 to MDCT_FRAME_LEN := 0;

  -- Request decoded from issue_index_r.
  signal request_s : std_logic;
  signal request_region_s : region_t;
  signal request_last_s : std_logic;
  signal request_addr_a_s, request_addr_b_s : mdct_pcm_addr_t;
  signal request_rom_en_s, request_rom_size_s : std_logic;
  signal request_rom_shape_s : mdct_shape_t;
  signal request_rom_addr_s : unsigned(8 downto 0);

  -- Metadata delayed by the one-cycle PCM/ROM memories.
  signal tag_valid_r, tag_last_r : std_logic := '0';
  signal tag_region_r : region_t := R_RIGHT_ZERO;
  signal tag_index_r : mdct_data_addr_t := (others => '0');

  signal rom_valid_s : std_logic;
  signal rom_re_s, rom_im_s : mdct_coeff_t;
begin
  busy <= '1' when state_r = S_RUN else '0';
  pcm_rd_en <= request_s;
  pcm_rd_addr_a <= request_addr_a_s;
  pcm_rd_addr_b <= request_addr_b_s;
  out_exp <= to_unsigned(2, out_exp'length);

  -----------------------------------------------------------------------------
  -- Address generator.  Only adds/subtracts and comparisons are in this cone;
  -- there is no divider or multiplier in the per-sample address path.
  -----------------------------------------------------------------------------
  process(all)
    variable j, length_v, half_v, nl_v, nr_v, i_v : natural;
    variable addr_a_v, addr_b_v : natural;
  begin
    request_s <= '0';
    request_region_s <= R_RIGHT_ZERO;
    request_last_s <= '0';
    request_addr_a_s <= (others => '0');
    request_addr_b_s <= (others => '0');
    request_rom_en_s <= '0';
    request_rom_size_s <= MDCT_FFT64;
    request_rom_shape_s <= MDCT_SINE;
    request_rom_addr_s <= (others => '0');

    if state_r = S_RUN and issue_index_r < transform_len_r then
      request_s <= '1';
      j := issue_index_r;
      length_v := transform_len_r;
      half_v := length_v / 2;
      nl_v := (length_v - fl_r) / 2;
      nr_v := (length_v - fr_r) / 2;
      addr_a_v := 0;
      addr_b_v := 0;

      if j < fr_r / 2 then
        -- Fourth loop of mdct_block(), traversed backwards to keep j natural.
        request_region_s <= R_RIGHT_WINDOW;
        i_v := fr_r / 2 - 1 - j;
        addr_a_v := base_r + length_v + nr_v + i_v;
        addr_b_v := base_r + 2 * length_v - nr_v - i_v - 1;
        request_rom_en_s <= '1';
        request_rom_size_s <= MDCT_FFT512 when fr_r = MDCT_FRAME_LEN else
                              MDCT_FFT64;
        request_rom_shape_s <= right_shape_r;
        request_rom_addr_s <= to_unsigned(i_v, request_rom_addr_s'length);
      elsif j < half_v then
        -- Third loop: right-side zero-slope region.
        request_region_s <= R_RIGHT_ZERO;
        i_v := half_v - 1 - j;
        addr_a_v := base_r + length_v + i_v;
      elsif j < half_v + nl_v then
        -- First loop: left-side zero-slope region.
        request_region_s <= R_LEFT_ZERO;
        i_v := j - half_v;
        addr_a_v := base_r + length_v - i_v - 1;
      else
        -- Second loop: windowed left slope.
        request_region_s <= R_LEFT_WINDOW;
        i_v := j - (half_v + nl_v);
        addr_a_v := base_r + nl_v + i_v;
        addr_b_v := base_r + length_v - nl_v - i_v - 1;
        request_rom_en_s <= '1';
        request_rom_size_s <= MDCT_FFT512 when fl_r = MDCT_FRAME_LEN else
                              MDCT_FFT64;
        request_rom_shape_s <= left_shape_r;
        request_rom_addr_s <= to_unsigned(i_v, request_rom_addr_s'length);
      end if;

      assert addr_a_v < MDCT_SNAPSHOT_N and addr_b_v < MDCT_SNAPSHOT_N
        report "mdct_window_fold: generated PCM address outside snapshot"
        severity failure;
      request_addr_a_s <= to_unsigned(addr_a_v, request_addr_a_s'length);
      request_addr_b_s <= to_unsigned(addr_b_v, request_addr_b_s'length);
      if j = length_v - 1 then request_last_s <= '1'; end if;
    end if;
  end process;

  u_window_rom : entity work.mdct_window_rom
    port map (
      clk => clk,
      en => request_rom_en_s,
      size_mode => request_rom_size_s,
      shape => request_rom_shape_s,
      addr => request_rom_addr_s,
      valid => rom_valid_s,
      coeff_re => rom_re_s,
      coeff_im => rom_im_s
    );

  -----------------------------------------------------------------------------
  -- Pipeline control and arithmetic response stage.
  -----------------------------------------------------------------------------
  process(clk, rst_n)
    variable product_a_v, product_b_v : mdct_data_t;
  begin
    if rst_n = '0' then
      state_r <= S_IDLE;
      issue_index_r <= 0;
      tag_valid_r <= '0';
      done <= '0';
      out_valid <= '0';
      out_last <= '0';
    elsif rising_edge(clk) then
      done <= '0';
      out_valid <= '0';
      out_last <= '0';
      tag_valid_r <= request_s;

      if request_s = '1' then
        tag_region_r <= request_region_s;
        tag_index_r <= to_unsigned(issue_index_r, tag_index_r'length);
        tag_last_r <= request_last_s;
        issue_index_r <= issue_index_r + 1;
      end if;

      assert pcm_rd_valid = tag_valid_r
        report "mdct_window_fold: PCM response/tag misalignment"
        severity failure;

      if pcm_rd_valid = '1' and tag_valid_r = '1' then
        if tag_region_r = R_RIGHT_WINDOW or tag_region_r = R_LEFT_WINDOW then
          assert rom_valid_s = '1'
            report "mdct_window_fold: missing window coefficient response"
            severity failure;
        else
          assert rom_valid_s = '0'
            report "mdct_window_fold: unexpected coefficient in zero-slope region"
            severity failure;
        end if;

        case tag_region_r is
          when R_RIGHT_WINDOW =>
            product_a_v := mdct_mul_q15_q15(
              pcm_rd_data_a, mdct_pcm_t(rom_re_s));
            product_b_v := mdct_mul_q15_q15(
              pcm_rd_data_b, mdct_pcm_t(rom_im_s));
            out_data <= mdct_neg_q31(mdct_add_q31(product_a_v, product_b_v));

          when R_RIGHT_ZERO | R_LEFT_ZERO =>
            out_data <= mdct_neg_q31(mdct_pcm_shift15(pcm_rd_data_a));

          when R_LEFT_WINDOW =>
            product_a_v := mdct_mul_q15_q15(
              pcm_rd_data_a, mdct_pcm_t(rom_im_s));
            product_b_v := mdct_mul_q15_q15(
              pcm_rd_data_b, mdct_pcm_t(rom_re_s));
            out_data <= mdct_sub_q31(product_a_v, product_b_v);
        end case;

        out_index <= tag_index_r;
        out_valid <= '1';
        out_last <= tag_last_r;
        if tag_last_r = '1' then
          done <= '1';
          state_r <= S_IDLE;
        end if;
      end if;

      if start = '1' then
        assert state_r = S_IDLE
          report "mdct_window_fold: start while busy"
          severity failure;
        assert to_integer(left_slope_len) = MDCT_SHORT_LEN or
               to_integer(left_slope_len) = MDCT_FRAME_LEN
          report "mdct_window_fold: left slope must be 128 or 1024"
          severity failure;
        assert to_integer(right_slope_len) = MDCT_SHORT_LEN or
               to_integer(right_slope_len) = MDCT_FRAME_LEN
          report "mdct_window_fold: right slope must be 128 or 1024"
          severity failure;

        if size_mode = MDCT_FFT512 then
          transform_len_r <= MDCT_FRAME_LEN;
          assert to_integer(left_slope_len) <= MDCT_FRAME_LEN and
                 to_integer(right_slope_len) <= MDCT_FRAME_LEN
            report "mdct_window_fold: invalid long slope geometry"
            severity failure;
        else
          transform_len_r <= MDCT_SHORT_LEN;
          assert to_integer(left_slope_len) <= MDCT_SHORT_LEN and
                 to_integer(right_slope_len) <= MDCT_SHORT_LEN
            report "mdct_window_fold: invalid short slope geometry"
            severity failure;
        end if;

        size_mode_r <= size_mode;
        base_r <= to_integer(input_base);
        fl_r <= to_integer(left_slope_len);
        fr_r <= to_integer(right_slope_len);
        left_shape_r <= left_shape;
        right_shape_r <= right_shape;
        issue_index_r <= 0;
        tag_valid_r <= '0';
        state_r <= S_RUN;
      end if;
    end if;
  end process;
end architecture rtl;
