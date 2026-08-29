-------------------------------------------------------------------------------
-- Scalar Q31 work memory between window/fold and DCT-IV pre-rotation.
--
-- The fold engine writes one natural-order value per clock.  In the following
-- non-overlapping phase, pre-rotation reads two values per clock.  Port A is
-- therefore time-multiplexed between write and read, and port B is read-only.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_work_memory is
  port (
    clk : in std_logic;

    wr_en   : in std_logic;
    wr_addr : in mdct_data_addr_t;
    wr_data : in mdct_data_t;

    rd_en     : in  std_logic;
    rd_addr_a : in  mdct_data_addr_t;
    rd_addr_b : in  mdct_data_addr_t;
    rd_valid  : out std_logic;
    rd_data_a : out mdct_data_t;
    rd_data_b : out mdct_data_t
  );
end entity mdct_work_memory;

architecture rtl of mdct_work_memory is
  type work_ram_t is array (0 to MDCT_FRAME_LEN - 1) of mdct_data_t;
  signal ram : work_ram_t := (others => (others => '0'));
  attribute ram_style : string;
  attribute ram_style of ram : signal is "block";
begin
  process(clk)
  begin
    if rising_edge(clk) then
      rd_valid <= rd_en;

      assert not (wr_en = '1' and rd_en = '1')
        report "mdct_work_memory: fold write and pre read cannot overlap"
        severity failure;

      if wr_en = '1' then
        ram(to_integer(wr_addr)) <= wr_data;
      elsif rd_en = '1' then
        rd_data_a <= ram(to_integer(rd_addr_a));
      end if;

      if rd_en = '1' then
        rd_data_b <= ram(to_integer(rd_addr_b));
      end if;
    end if;
  end process;
end architecture rtl;

