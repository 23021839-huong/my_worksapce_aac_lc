-------------------------------------------------------------------------------
-- Incremental radix-2 DIT address generator.
--
-- The old implementation derived every address with division and modulo:
--   group = pair / half; j = pair mod half.
-- This block keeps group_base, j and phase_index as running counters.  During
-- a stage it therefore uses only compares and additions on the critical path.
-- phase_index is the logical FFT-512 twiddle index k in exp(-j*2*pi*k/512).
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.fft_radix2_pkg.all;

entity fft_radix2_addr_gen is
  port (
    clk, rst_n    : in  std_logic;
    stage_start   : in  std_logic;
    stage_index   : in  unsigned(3 downto 0);
    active_n      : in  unsigned(9 downto 0);
    issue_advance : in  std_logic;

    issue_valid   : out std_logic;
    issue_last    : out std_logic;
    addr_a        : out fft_addr_t;
    addr_b        : out fft_addr_t;
    phase_index   : out unsigned(7 downto 0)
  );
end entity fft_radix2_addr_gen;

architecture rtl of fft_radix2_addr_gen is
  signal active_r     : std_logic := '0';
  signal n_r          : natural range 64 to FFT_MAX_N := 64;
  signal m_r          : natural range 2 to FFT_MAX_N := 2;
  signal half_r       : natural range 1 to FFT_MAX_N / 2 := 1;
  signal tw_step_r    : natural range 1 to FFT_MAX_N / 2 := 256;
  signal group_base_r : natural range 0 to FFT_MAX_N - 2 := 0;
  signal j_r          : natural range 0 to FFT_MAX_N / 2 - 1 := 0;
  signal phase_r      : natural range 0 to FFT_MAX_N / 2 - 1 := 0;
begin
  issue_valid <= active_r;
  issue_last <= '1' when active_r = '1' and
                         group_base_r = n_r - m_r and
                         j_r = half_r - 1 else '0';
  addr_a <= to_unsigned(group_base_r + j_r, addr_a'length);
  addr_b <= to_unsigned(group_base_r + j_r + half_r, addr_b'length);
  phase_index <= to_unsigned(phase_r, phase_index'length);

  process(clk, rst_n)
    variable requested_stage : natural;
    variable requested_n     : natural;
  begin
    if rst_n = '0' then
      active_r <= '0';
      n_r <= 64;
      m_r <= 2;
      half_r <= 1;
      tw_step_r <= 256;
      group_base_r <= 0;
      j_r <= 0;
      phase_r <= 0;
    elsif rising_edge(clk) then
      if stage_start = '1' then
        requested_stage := to_integer(stage_index);
        requested_n := to_integer(active_n);
        assert requested_stage >= 1 and requested_stage <= FFT_MAX_LDN
          report "fft_radix2_addr_gen: invalid stage_index"
          severity failure;
        assert requested_n = 64 or requested_n = 512
          report "fft_radix2_addr_gen: active_n must be 64 or 512"
          severity failure;
        assert (requested_n = 512) or requested_stage <= 6
          report "fft_radix2_addr_gen: FFT64 stage exceeds 6"
          severity failure;

        n_r <= requested_n;
        m_r <= 2 ** requested_stage;
        half_r <= 2 ** (requested_stage - 1);
        -- The master phase grid is always FFT-512.  FFT64 naturally uses
        -- phase indices with stride 8 at its final stage.
        tw_step_r <= 2 ** (FFT_MAX_LDN - requested_stage);
        group_base_r <= 0;
        j_r <= 0;
        phase_r <= 0;
        active_r <= '1';
      elsif active_r = '1' and issue_advance = '1' then
        if group_base_r = n_r - m_r and j_r = half_r - 1 then
          active_r <= '0';
        elsif j_r = half_r - 1 then
          group_base_r <= group_base_r + m_r;
          j_r <= 0;
          phase_r <= 0;
        else
          j_r <= j_r + 1;
          phase_r <= phase_r + tw_step_r;
        end if;
      end if;
    end if;
  end process;
end architecture rtl;
