-------------------------------------------------------------------------------
-- 2048-sample snapshot memory for one AAC-LC channel.
--
-- Port A is time-multiplexed between the external loader and fold read A;
-- port B is fold read B.  Load and transform phases never overlap, so this
-- maps cleanly to a true-dual-port RAM instead of requiring a third port.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_pcm_memory is
  port (
    clk : in std_logic;

    load_we   : in std_logic;
    load_addr : in mdct_pcm_addr_t;
    load_data : in mdct_pcm_t;

    rd_en     : in  std_logic;
    rd_addr_a : in  mdct_pcm_addr_t;
    rd_addr_b : in  mdct_pcm_addr_t;
    rd_valid  : out std_logic;
    rd_data_a : out mdct_pcm_t;
    rd_data_b : out mdct_pcm_t
  );
end entity mdct_pcm_memory;

architecture rtl of mdct_pcm_memory is
  type pcm_ram_t is array (0 to MDCT_SNAPSHOT_N - 1) of mdct_pcm_t;
  signal ram : pcm_ram_t := (others => (others => '0'));
  attribute ram_style : string;
  attribute ram_style of ram : signal is "block";
begin
  process(clk)
  begin
    if rising_edge(clk) then
      rd_valid <= rd_en;

      assert not (load_we = '1' and rd_en = '1')
        report "mdct_pcm_memory: load and fold read cannot overlap"
        severity failure;

      -- Physical port A: write while loading, read during folding.
      if load_we = '1' then
        ram(to_integer(load_addr)) <= load_data;
      elsif rd_en = '1' then
        rd_data_a <= ram(to_integer(rd_addr_a));
      end if;

      -- Physical port B is only needed during folding.
      if rd_en = '1' then
        rd_data_b <= ram(to_integer(rd_addr_b));
      end if;
    end if;
  end process;
end architecture rtl;

