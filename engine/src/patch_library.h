/* sloppy-synth: patch and bank storage, independent of any UI.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 *
 * Banks (.vitalbank) are zip files laid out the way Vital exports them:
 *   <Bank Name>/Presets/...      .vital patches
 *   <Bank Name>/Wavetables/...   .vitaltable / .wav
 *   <Bank Name>/LFOs/...         .vitallfo
 *   <Bank Name>/Samples/...      .wav / .flac
 * Importing a bank unpacks it into the library root, the same way Vital
 * does, so a library folder can also simply be pointed at an existing
 * Vital data folder.
 */
#pragma once

#include "JuceHeader.h"

#include <string>
#include <vector>

namespace sloppy {

  struct PatchEntry {
    File file;
    std::string name;      // file name without extension
    std::string bank;      // top level folder in the library, e.g. "Factory"
    std::string category;  // folder below Presets/, if any
    // Folders between the bank and the file, without Vital's "Presets"
    // folder: Bank/Presets/Leads/Bright/x.vital gives { "Leads", "Bright" }.
    std::vector<std::string> folders;
  };

  class PatchLibrary {
    public:
      // Default library location: $SLOPPY_DATA_DIR, else
      // $XDG_DATA_HOME/sloppy-synth, else ~/.local/share/sloppy-synth.
      static File defaultRoot();

      explicit PatchLibrary(File root = defaultRoot());

      const File& getRoot() const { return root_; }

      // Extra read-only folders to scan for patches, e.g. an existing
      // Vital data folder.
      void addSearchFolder(const File& folder);

      // Unpacks a .vitalbank into the library. On success, `bank_name` is
      // the folder the bank was unpacked into.
      bool importBank(const File& bank_file, std::string& bank_name, std::string& error);

      // Copies a single .vital patch into <root>/User/Presets.
      bool importPatch(const File& patch_file, File& imported, std::string& error);

      // All .vital patches in the library and search folders, sorted by
      // bank then name.
      std::vector<PatchEntry> listPatches() const;

    private:
      static void collectPatches(const File& folder, std::vector<PatchEntry>& patches);

      File root_;
      std::vector<File> search_folders_;
  };

} // namespace sloppy
