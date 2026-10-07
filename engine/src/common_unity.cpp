/* sloppy-synth: unity build of Vital's src/common, plus helpers that have to
 * live in the same translation unit.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "../../vital/src/unity_build/common.cpp"

#include "synth_strings.h"

namespace sloppy {
  namespace {
    struct LookupSize {
      const std::string* lookup;
      size_t size;
    };

    template<size_t kSize>
    LookupSize lookupEntry(const std::string (&lookup)[kSize]) {
      return { lookup, kSize };
    }
  } // namespace

  // Vital's parameter table only stores a pointer to each value-name array,
  // and some parameters cover more values than their array has names
  // (filter styles depend on the filter model, for example). The arrays are
  // `const` in a header, so every translation unit has its own copy; this
  // has to sit in the same unit as synth_parameters.cpp for the pointers to
  // match.
  size_t valueNameCount(const std::string* lookup) {
    static const LookupSize kLookups[] = {
      lookupEntry(strings::kCompressorBandNames),
      lookupEntry(strings::kDelayStyleNames),
      lookupEntry(strings::kDestinationNames),
      lookupEntry(strings::kDistortionFilterOrderNames),
      lookupEntry(strings::kDistortionTypeNames),
      lookupEntry(strings::kEqBandModeNames),
      lookupEntry(strings::kEqHighModeNames),
      lookupEntry(strings::kEqLowModeNames),
      lookupEntry(strings::kFilterModelNames),
      lookupEntry(strings::kFilterStyleNames),
      lookupEntry(strings::kFrequencySyncNames),
      lookupEntry(strings::kOffOnNames),
      lookupEntry(strings::kOversamplingNames),
      lookupEntry(strings::kPhaseDistortionNames),
      lookupEntry(strings::kRandomNames),
      lookupEntry(strings::kSpectralMorphNames),
      lookupEntry(strings::kStereoModeNames),
      lookupEntry(strings::kSyncNames),
      lookupEntry(strings::kSyncedFrequencyNames),
      lookupEntry(strings::kUnisonStackNames),
      lookupEntry(strings::kVoiceOverrideNames),
      lookupEntry(strings::kVoicePriorityNames),
    };
    for (const LookupSize& entry : kLookups) {
      if (entry.lookup == lookup)
        return entry.size;
    }
    return 0;
  }
} // namespace sloppy
