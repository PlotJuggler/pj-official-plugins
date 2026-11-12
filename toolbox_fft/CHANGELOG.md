# Changelog — toolbox_fft

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `toolbox_fft/`.

## [1.1.0] - 2026-11-10

### Fixed
- Selections whose sample count has a large prime factor no longer freeze the panel: a 200,006-sample selection that
  took about 14 seconds now takes milliseconds, and every selection size runs in comparable time. Typical selections
  compute about 9x faster; the spectrum itself is unchanged.
- Very large selections (above about 4.6 million samples in the browser, 9.2 million on desktop) no longer risk
  running out of memory: the last few samples, well under 1%, are left out so the transform fits, and the status line
  says how many.

## [1.0.2] - 2026-11-01

### Fixed
- With two recordings carrying the same series name, a drop reads the one the user dragged; it could read the other,
  with no warning. The panel names the recording, and exports to a source named after it. On a PlotJuggler that does
  not say which one was dragged, the drop is refused with that reason.

## [1.0.1] - 2026-08-04
