-------------------------------------------------------------------------------
-- Control plane for the complete AAC-LC MDCT transform.
--
-- Datapath and memories are intentionally absent from this file.  The FSM only
-- validates the 2048-sample snapshot, sequences the four transform engines,
-- tracks eight short sub-transforms, and preserves FDK's previous-right-window
-- state for the next left slope.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_control is
  port (
    clk, rst_n : in std_logic;

    -- Snapshot loader.  Indices must be contiguous 0..2047.
    sample_valid : in  std_logic;
    sample_index : in  mdct_pcm_addr_t;
    sample_ready : out std_logic;
    load_accept  : out std_logic;

    -- Frame command from the block-switch/buffer wrapper.
    start         : in std_logic;
    block_type    : in mdct_block_t;
    right_shape   : in mdct_shape_t;
    clear_history : in std_logic;

    busy : out std_logic;
    done : out std_logic;

    -- Completion handshakes from the structural datapath.
    fold_done      : in std_logic;
    pre_done       : in std_logic;
    fft_done       : in std_logic;
    post_load_done : in std_logic;
    post_done      : in std_logic;

    -- One-cycle engine commands.
    fold_start : out std_logic;
    pre_start  : out std_logic;
    fft_start  : out std_logic;
    post_start : out std_logic;

    -- High while the top-level adapter must read every FFT result into post.
    post_load_enable : out std_logic;

    -- Latched geometry for the active sub-transform.
    active_size_mode : out std_logic;
    active_input_base : out mdct_pcm_addr_t;
    active_output_base : out mdct_data_addr_t;
    active_left_slope_len : out unsigned(10 downto 0);
    active_right_slope_len : out unsigned(10 downto 0);
    active_left_shape : out mdct_shape_t;
    active_right_shape : out mdct_shape_t;
    active_sub_index : out unsigned(2 downto 0);

    output_exp : out unsigned(4 downto 0)
  );
end entity mdct_control;

architecture rtl of mdct_control is
  type state_t is (
    S_LOAD,
    S_FOLD_START, S_FOLD_WAIT,
    S_PRE_START, S_PRE_WAIT,
    S_FFT_START, S_FFT_WAIT,
    S_POST_LOAD,
    S_POST_START, S_POST_WAIT,
    S_DONE
  );
  signal state_r : state_t := S_LOAD;

  signal loaded_count_r : natural range 0 to MDCT_SNAPSHOT_N := 0;
  signal block_type_r : mdct_block_t := MDCT_LONG;
  signal right_shape_r : mdct_shape_t := MDCT_KBD;
  signal right_slope_len_r : natural range MDCT_SHORT_LEN to MDCT_FRAME_LEN :=
    MDCT_FRAME_LEN;
  signal sub_index_r : natural range 0 to MDCT_SHORT_COUNT - 1 := 0;

  -- Persistent FDK mdct_t::{prev_wrs,prev_fr} state.
  signal history_valid_r : std_logic := '0';
  signal prev_shape_r : mdct_shape_t := MDCT_KBD;
  signal prev_slope_len_r : natural range MDCT_SHORT_LEN to MDCT_FRAME_LEN :=
    MDCT_FRAME_LEN;

  -- Left state actually latched for the current sub-transform.
  signal left_shape_r : mdct_shape_t := MDCT_KBD;
  signal left_slope_len_r : natural range MDCT_SHORT_LEN to MDCT_FRAME_LEN :=
    MDCT_FRAME_LEN;

  signal load_accept_s : std_logic;
  signal sample_ready_s : std_logic;
begin
  -- ready includes the expected address, so ready/valid always means that the
  -- complete indexed transaction is accepted.  Gating with rst_n also keeps
  -- the unreset PCM RAM write-enable inactive during asynchronous reset.
  sample_ready_s <= '1' when rst_n = '1' and state_r = S_LOAD and
                             loaded_count_r < MDCT_SNAPSHOT_N and
                             not is_x(std_logic_vector(sample_index)) and
                             to_integer(sample_index) = loaded_count_r else '0';
  sample_ready <= sample_ready_s;
  load_accept_s <= sample_valid and sample_ready_s;
  load_accept <= load_accept_s;

  busy <= '0' when state_r = S_LOAD or state_r = S_DONE else '1';
  done <= '1' when state_r = S_DONE else '0';

  fold_start <= '1' when state_r = S_FOLD_START else '0';
  pre_start <= '1' when state_r = S_PRE_START else '0';
  fft_start <= '1' when state_r = S_FFT_START else '0';
  post_start <= '1' when state_r = S_POST_START else '0';
  post_load_enable <= '1' when state_r = S_POST_LOAD else '0';

  active_size_mode <= mdct_fft_size_mode(block_type_r);
  active_input_base <= to_unsigned(
    mdct_sub_input_base(block_type_r, sub_index_r), active_input_base'length);
  active_output_base <= to_unsigned(
    mdct_sub_output_base(block_type_r, sub_index_r), active_output_base'length);
  active_left_slope_len <= to_unsigned(left_slope_len_r,
                                        active_left_slope_len'length);
  active_right_slope_len <= to_unsigned(right_slope_len_r,
                                         active_right_slope_len'length);
  active_left_shape <= left_shape_r;
  active_right_shape <= right_shape_r;
  active_sub_index <= to_unsigned(sub_index_r, active_sub_index'length);
  output_exp <= to_unsigned(mdct_output_exp(block_type_r), output_exp'length);

  process(clk, rst_n)
    variable next_sub_v : natural;
  begin
    if rst_n = '0' then
      state_r <= S_LOAD;
      loaded_count_r <= 0;
      block_type_r <= MDCT_LONG;
      right_shape_r <= MDCT_KBD;
      right_slope_len_r <= MDCT_FRAME_LEN;
      sub_index_r <= 0;
      history_valid_r <= '0';
      prev_shape_r <= MDCT_KBD;
      prev_slope_len_r <= MDCT_FRAME_LEN;
      left_shape_r <= MDCT_KBD;
      left_slope_len_r <= MDCT_FRAME_LEN;
    elsif rising_edge(clk) then
      case state_r is
        when S_LOAD =>
          if clear_history = '1' then
            history_valid_r <= '0';
          end if;

          if sample_valid = '1' then
            assert not is_x(std_logic_vector(sample_index))
              report "mdct_control: sample_index contains an unknown value"
              severity error;
            if not is_x(std_logic_vector(sample_index)) then
              assert loaded_count_r < MDCT_SNAPSHOT_N
                report "mdct_control: too many PCM samples in snapshot"
                severity error;
              assert to_integer(sample_index) = loaded_count_r
                report "mdct_control: sample_index must be contiguous from zero"
                severity error;
            end if;
          end if;
          if load_accept_s = '1' then
            loaded_count_r <= loaded_count_r + 1;
          end if;

          assert not (sample_valid = '1' and start = '1')
            report "mdct_control: PCM load and start cannot overlap"
            severity error;

          if start = '1' and sample_valid = '0' then
            assert loaded_count_r = MDCT_SNAPSHOT_N
              report "mdct_control: start before all 2048 PCM samples were loaded"
              severity failure;

            if loaded_count_r = MDCT_SNAPSHOT_N then
              block_type_r <= block_type;
              right_shape_r <= right_shape;
              right_slope_len_r <= mdct_right_slope_len(block_type);
              sub_index_r <= 0;

              -- Cold-start behavior in mdct_block(): current right slope is
              -- first installed into history and then used as the left slope.
              if history_valid_r = '1' and clear_history = '0' then
                left_shape_r <= prev_shape_r;
                left_slope_len_r <= prev_slope_len_r;
              else
                left_shape_r <= right_shape;
                left_slope_len_r <= mdct_right_slope_len(block_type);
              end if;
              state_r <= S_FOLD_START;
            end if;
          end if;

        when S_FOLD_START =>
          state_r <= S_FOLD_WAIT;

        when S_FOLD_WAIT =>
          if fold_done = '1' then state_r <= S_PRE_START; end if;

        when S_PRE_START =>
          state_r <= S_PRE_WAIT;

        when S_PRE_WAIT =>
          if pre_done = '1' then state_r <= S_FFT_START; end if;

        when S_FFT_START =>
          state_r <= S_FFT_WAIT;

        when S_FFT_WAIT =>
          if fft_done = '1' then state_r <= S_POST_LOAD; end if;

        when S_POST_LOAD =>
          if post_load_done = '1' then state_r <= S_POST_START; end if;

        when S_POST_START =>
          state_r <= S_POST_WAIT;

        when S_POST_WAIT =>
          if post_done = '1' then
            -- The right slope becomes the left state for the next transform.
            history_valid_r <= '1';
            prev_shape_r <= right_shape_r;
            prev_slope_len_r <= right_slope_len_r;

            if sub_index_r + 1 < mdct_num_subtransforms(block_type_r) then
              next_sub_v := sub_index_r + 1;
              sub_index_r <= next_sub_v;
              -- All later short transforms use the preceding short right
              -- slope, exactly as mdct_block() updates state inside its loop.
              left_shape_r <= right_shape_r;
              left_slope_len_r <= right_slope_len_r;
              state_r <= S_FOLD_START;
            else
              state_r <= S_DONE;
            end if;
          end if;

        when S_DONE =>
          loaded_count_r <= 0;
          state_r <= S_LOAD;
      end case;

      -- Engine completion pulses are only legal in their respective waits.
      if fold_done = '1' then
        assert state_r = S_FOLD_WAIT
          report "mdct_control: unexpected fold_done"
          severity error;
      end if;
      if pre_done = '1' then
        assert state_r = S_PRE_WAIT
          report "mdct_control: unexpected pre_done"
          severity error;
      end if;
      if fft_done = '1' then
        assert state_r = S_FFT_WAIT
          report "mdct_control: unexpected fft_done"
          severity error;
      end if;
      if post_done = '1' then
        assert state_r = S_POST_WAIT
          report "mdct_control: unexpected post_done"
          severity error;
      end if;
    end if;
  end process;
end architecture rtl;
