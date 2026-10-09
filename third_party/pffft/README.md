# pffft

PFFFT ("a Pretty Fast FFT") from https://github.com/marton78/pffft, tag v1.1.0
(commit 1fdcb508), single-precision files only, unmodified. Licence: BSD-style,
see `LICENSE.txt`.

sloppy-synth uses it for Vital's `FourierTransform` in place of JUCE's portable
FFT, which is several times slower (see `vital/src/common/fourier_transform.h`).
