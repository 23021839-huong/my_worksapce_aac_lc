-------------------------------------------------------------------------------
-- Frame/stage controller for the shared FFT64/FFT512 engine.
--
-- This block contains protocol and stage sequencing only.  It does not know
-- butterfly equations, RAM data, or twiddle values.  Keeping those concerns
-- separate makes the state machine small and lets the datapath pipeline evolve
-- without rewriting frame control.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.fft_radix2_pkg.all;

entity fft_radix2_control is
  port (
    clk, rst_n : in std_logic;

    size_mode : in std_logic;
    ld_en     : in std_logic;
    ld_idx    : in fft_addr_t;
    start     : in std_logic;

    pipeline_out_valid : in std_logic;
    pipeline_out_last  : in std_logic;

    load_ready  : out std_logic;
    load_accept : out std_logic;
    busy        : out std_logic;
    done        : out std_logic;

    stage_start : out std_logic;
    stage_index : out unsigned(3 downto 0);
    active_n    : out unsigned(9 downto 0);
    source_bank : out std_logic;
    dest_bank   : out std_logic;
    result_bank : out std_logic;
    scale_exp   : out unsigned(3 downto 0)
  );
end entity fft_radix2_control;

architecture rtl of fft_radix2_control is
  type state_t is (S_LOAD, S_RUN, S_DONE);
  signal state_r : state_t := S_LOAD;

  signal loaded_count_r : natural range 0 to FFT_MAX_N := 0;
  signal load_mode_r, load_mode_valid_r : std_logic := '0';
  signal mode_r : std_logic := MODE_FFT64;
  signal active_n_r : natural range 64 to FFT_MAX_N := 64;
  signal active_ldn_r : natural range 6 to FFT_MAX_LDN := 6;
  signal stage_r : natural range 1 to FFT_MAX_LDN := 1;
  signal source_bank_r, dest_bank_r, result_bank_r : std_logic := '0';
  signal stage_start_r : std_logic := '0';
  signal scale_exp_r : unsigned(3 downto 0) := (others => '0');
begin
  load_ready <= '1' when state_r = S_LOAD else '0';
  busy <= '1' when state_r = S_RUN else '0';
  done <= '1' when state_r = S_DONE else '0';

  -- Invalid load requests are blocked from memory as well as reported by the
  -- assertions in the sequential process.
  load_accept <= '1' when state_r = S_LOAD and ld_en = '1' and
                          to_integer(ld_idx) = loaded_count_r and
                          ((size_mode = MODE_FFT64 and loaded_count_r < 64) or
                           (size_mode = MODE_FFT512 and loaded_count_r < 512)) and
                          (load_mode_valid_r = '0' or size_mode = load_mode_r)
                 else '0';

  stage_start <= stage_start_r;
  stage_index <= to_unsigned(stage_r, stage_index'length);
  active_n <= to_unsigned(active_n_r, active_n'length);
  source_bank <= source_bank_r;
  dest_bank <= dest_bank_r;
  result_bank <= result_bank_r;
  scale_exp <= scale_exp_r;

  process(clk, rst_n)
    variable requested_n, requested_ldn : natural;
  begin
    if rst_n = '0' then
      state_r <= S_LOAD;
      loaded_count_r <= 0;
      load_mode_r <= MODE_FFT64;
      load_mode_valid_r <= '0';
      mode_r <= MODE_FFT64;
      active_n_r <= 64;
      active_ldn_r <= 6;
      stage_r <= 1;
      source_bank_r <= '0';
      dest_bank_r <= '1';
      result_bank_r <= '0';
      stage_start_r <= '0';
      scale_exp_r <= (others => '0');
    elsif rising_edge(clk) then
      stage_start_r <= '0';

      assert size_mode = MODE_FFT64 or size_mode = MODE_FFT512
        report "fft_radix2_control: size_mode must be 0 or 1"
        severity error;
      if size_mode = MODE_FFT512 then
        requested_n := 512;
        requested_ldn := 9;
      else
        requested_n := 64;
        requested_ldn := 6;
      end if;

      case state_r is
        when S_LOAD =>
          assert not (ld_en = '1' and start = '1')
            report "fft_radix2_control: ld_en and start cannot overlap"
            severity error;

          if ld_en = '1' then
            assert to_integer(ld_idx) = loaded_count_r
              report "fft_radix2_control: ld_idx must be contiguous from zero"
              severity error;
            assert loaded_count_r < requested_n
              report "fft_radix2_control: too many samples for selected mode"
              severity error;
            assert load_mode_valid_r = '0' or size_mode = load_mode_r
              report "fft_radix2_control: size_mode changed while loading"
              severity error;

            if to_integer(ld_idx) = loaded_count_r and
               loaded_count_r < requested_n and
               (load_mode_valid_r = '0' or size_mode = load_mode_r) then
              loaded_count_r <= loaded_count_r + 1;
              if load_mode_valid_r = '0' then
                load_mode_r <= size_mode;
                load_mode_valid_r <= '1';
              end if;
            end if;
          end if;

          if start = '1' and ld_en = '0' then
            assert load_mode_valid_r = '1'
              report "fft_radix2_control: start before any input was loaded"
              severity error;
            assert size_mode = load_mode_r
              report "fft_radix2_control: start mode differs from load mode"
              severity error;
            assert loaded_count_r = requested_n
              report "fft_radix2_control: start before N samples were loaded"
              severity error;

            if load_mode_valid_r = '1' and size_mode = load_mode_r and
               loaded_count_r = requested_n then
              mode_r <= size_mode;
              active_n_r <= requested_n;
              active_ldn_r <= requested_ldn;
              stage_r <= 1;
              source_bank_r <= '0';
              dest_bank_r <= '1';
              scale_exp_r <= to_unsigned(requested_ldn - 1,
                                          scale_exp_r'length);
              loaded_count_r <= 0;
              load_mode_valid_r <= '0';
              stage_start_r <= '1';
              state_r <= S_RUN;
            end if;
          end if;

        when S_RUN =>
          assert size_mode = mode_r
            report "fft_radix2_control: size_mode changed while busy"
            severity error;
          assert ld_en = '0' and start = '0'
            report "fft_radix2_control: load/start request while busy"
            severity error;

          if pipeline_out_valid = '1' and pipeline_out_last = '1' then
            if stage_r = active_ldn_r then
              -- dest_bank_r contains the values committed on this same edge.
              result_bank_r <= dest_bank_r;
              state_r <= S_DONE;
            else
              stage_r <= stage_r + 1;
              source_bank_r <= dest_bank_r;
              dest_bank_r <= source_bank_r;
              stage_start_r <= '1';
            end if;
          end if;

        when S_DONE =>
          -- done is a one-cycle pulse.  Result RAM remains intact until the
          -- caller starts loading the next frame into bank 0.
          state_r <= S_LOAD;
      end case;
    end if;
  end process;
end architecture rtl;
