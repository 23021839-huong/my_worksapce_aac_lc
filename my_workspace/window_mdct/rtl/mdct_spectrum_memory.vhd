-------------------------------------------------------------------------------
-- Natural-order 1024-bin MDCT output memory.
--
-- Eight short transforms are written into consecutive 128-bin regions.  Long,
-- start and stop transforms write the full 1024-bin region.  The external read
-- interface is synchronous and remains valid until the next snapshot load.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_spectrum_memory is
  port (
    clk : in std_logic;

    wr_en   : in std_logic;
    wr_addr : in mdct_data_addr_t;
    wr_data : in mdct_data_t;

    rd_en    : in  std_logic;
    rd_addr  : in  mdct_data_addr_t;
    rd_valid : out std_logic;
    rd_data  : out mdct_data_t
  );
end entity mdct_spectrum_memory;

architecture rtl of mdct_spectrum_memory is
  type spectrum_ram_t is array (0 to MDCT_FRAME_LEN - 1) of mdct_data_t;
  signal ram : spectrum_ram_t := (others => (others => '0'));
  attribute ram_style : string;
  attribute ram_style of ram : signal is "block";
begin
  process(clk)
  begin
    if rising_edge(clk) then
      if wr_en = '1' then
        ram(to_integer(wr_addr)) <= wr_data;
      end if;
      rd_valid <= rd_en;
      if rd_en = '1' then
        rd_data <= ram(to_integer(rd_addr));
      end if;
    end if;
  end process;
end architecture rtl;
