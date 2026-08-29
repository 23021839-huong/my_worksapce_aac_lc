-------------------------------------------------------------------------------
-- Natural-order complex FFT-result cache used by DCT-IV post-rotation.
--
-- A 64-bit simple-dual-port RAM stores 512 complex Q31 bins.  Short transforms
-- use only addresses 0..63.  Explicitly packing real/imag into one word avoids
-- two independent register banks and guarantees both operands share one read
-- latency.
-------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.mdct_pkg.all;

entity mdct_fft_cache_memory is
  port (
    clk : in std_logic;

    wr_en   : in std_logic;
    wr_addr : in mdct_fft_addr_t;
    wr_re   : in mdct_data_t;
    wr_im   : in mdct_data_t;

    rd_en    : in  std_logic;
    rd_addr  : in  mdct_fft_addr_t;
    rd_valid : out std_logic;
    rd_re    : out mdct_data_t;
    rd_im    : out mdct_data_t
  );
end entity mdct_fft_cache_memory;

architecture rtl of mdct_fft_cache_memory is
  subtype complex_word_t is std_logic_vector(63 downto 0);
  type cache_ram_t is array (0 to 511) of complex_word_t;
  signal ram : cache_ram_t := (others => (others => '0'));
  attribute ram_style : string;
  attribute ram_style of ram : signal is "block";
begin
  process(clk)
    variable word_v : complex_word_t;
  begin
    if rising_edge(clk) then
      if wr_en = '1' then
        ram(to_integer(wr_addr)) <= std_logic_vector(wr_re) &
                                    std_logic_vector(wr_im);
      end if;
      rd_valid <= rd_en;
      if rd_en = '1' then
        word_v := ram(to_integer(rd_addr));
        rd_re <= signed(word_v(63 downto 32));
        rd_im <= signed(word_v(31 downto 0));
      end if;
    end if;
  end process;
end architecture rtl;
