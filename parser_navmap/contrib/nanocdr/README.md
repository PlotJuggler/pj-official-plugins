# nanocdr (vendored)

`nanocdr.hpp` is a verbatim copy of PlotJuggler's `3rdparty/nanocdr/include/nanocdr/nanocdr.hpp`
(PlotJuggler/PlotJuggler commit `361cdf9`), a single-header CDR encoder/decoder
derived from eProsima Fast-CDR. Apache-2.0, see `LICENSE`. Do not edit it here;
refresh it from PlotJuggler instead.

`nanocdr::Decoder` does not defend against hostile input, so `parser_navmap`
only reaches it through `navmap_cdr_reader.hpp`, which checks every read first:

- alignment padding is trimmed without a bounds check, so a truncated payload
  can wrap the remaining size and read past the end;
- `decode(std::vector&)` resizes to the wire length before checking it fits;
- the constructor reads the 4-byte header without checking the size;
- the endianness is taken from `encapsulation & 0x18`, which is always
  little-endian, so big-endian payloads would decode wrongly.
