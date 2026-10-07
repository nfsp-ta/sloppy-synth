/* sloppy-synth: patch and bank storage, independent of any UI.
 *
 * This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
 * Licensed under the GNU General Public License v3 or later, see LICENSE.
 */
#include "patch_library.h"

#include "load_save.h"
#include "synth_constants.h"

#include <algorithm>

namespace sloppy {

  namespace {
    const String kPresetWildcard = String("*.") + vital::kPresetExtension;

    // Vital's bank exporter stores everything under "<bank>/<type>/...".
    // Reject anything that would unpack outside the library folder.
    bool isSafeZipPath(const String& path) {
      if (path.startsWithChar('/') || path.startsWithChar('\\') || path.containsChar(':'))
        return false;

      StringArray parts;
      parts.addTokens(path.replaceCharacter('\\', '/'), "/", "");
      return !parts.contains("..");
    }
  }

  File PatchLibrary::defaultRoot() {
    String env = SystemStats::getEnvironmentVariable("SLOPPY_DATA_DIR", "");
    if (env.isNotEmpty())
      return File(env);

    String xdg = SystemStats::getEnvironmentVariable("XDG_DATA_HOME", "");
    if (xdg.isNotEmpty())
      return File(xdg).getChildFile("sloppy-synth");

    return File::getSpecialLocation(File::userHomeDirectory).getChildFile(".local/share/sloppy-synth");
  }

  PatchLibrary::PatchLibrary(File root) : root_(std::move(root)) { }

  void PatchLibrary::addSearchFolder(const File& folder) {
    if (folder.isDirectory())
      search_folders_.push_back(folder);
  }

  bool PatchLibrary::importBank(const File& bank_file, std::string& bank_name, std::string& error) {
    if (!bank_file.existsAsFile()) {
      error = "Bank file not found: " + bank_file.getFullPathName().toStdString();
      return false;
    }

    FileInputStream input_stream(bank_file);
    if (!input_stream.openedOk()) {
      error = "Couldn't open bank file.";
      return false;
    }

    ZipFile zip(input_stream);
    if (zip.getNumEntries() == 0) {
      error = "Bank is empty or not a zip file.";
      return false;
    }

    StringArray top_folders;
    for (int i = 0; i < zip.getNumEntries(); ++i) {
      String path = zip.getEntry(i)->filename;
      if (!isSafeZipPath(path)) {
        error = "Bank contains an unsafe path: " + path.toStdString();
        return false;
      }
      String top = path.replaceCharacter('\\', '/').upToFirstOccurrenceOf("/", false, false);
      if (top.isNotEmpty())
        top_folders.addIfNotAlreadyThere(top);
    }

    if (!root_.createDirectory()) {
      error = "Couldn't create library folder " + root_.getFullPathName().toStdString();
      return false;
    }

    Result result = zip.uncompressTo(root_, true);
    if (result.failed()) {
      error = "Unzipping bank failed: " + result.getErrorMessage().toStdString();
      return false;
    }

    bank_name = top_folders.size() == 1 ? top_folders[0].toStdString()
                                        : bank_file.getFileNameWithoutExtension().toStdString();
    return true;
  }

  bool PatchLibrary::importPatch(const File& patch_file, File& imported, std::string& error) {
    if (!patch_file.existsAsFile()) {
      error = "Patch file not found: " + patch_file.getFullPathName().toStdString();
      return false;
    }

    File folder = root_.getChildFile(LoadSave::kUserDirectoryName).getChildFile(LoadSave::kPresetFolderName);
    if (!folder.createDirectory()) {
      error = "Couldn't create " + folder.getFullPathName().toStdString();
      return false;
    }

    imported = folder.getChildFile(patch_file.getFileName());
    if (!patch_file.copyFileTo(imported)) {
      error = "Couldn't copy patch into the library.";
      return false;
    }
    return true;
  }

  void PatchLibrary::collectPatches(const File& folder, std::vector<PatchEntry>& patches) {
    if (!folder.isDirectory())
      return;

    for (const DirectoryEntry& entry : RangedDirectoryIterator(folder, true, kPresetWildcard, File::findFiles)) {
      File file = entry.getFile();
      PatchEntry patch;
      patch.file = file;
      patch.name = file.getFileNameWithoutExtension().toStdString();

      String relative = file.getRelativePathFrom(folder).replaceCharacter('\\', '/');
      StringArray parts;
      parts.addTokens(relative, "/", "");
      parts.remove(parts.size() - 1);  // the file itself
      if (parts.size() > 0)
        patch.bank = parts[0].toStdString();

      int presets_index = parts.indexOf(LoadSave::kPresetFolderName);
      if (presets_index >= 0 && presets_index + 1 < parts.size())
        patch.category = parts[presets_index + 1].toStdString();
      for (int i = 1; i < parts.size(); ++i) {
        if (i != presets_index)
          patch.folders.push_back(parts[i].toStdString());
      }

      patches.push_back(patch);
    }
  }

  std::vector<PatchEntry> PatchLibrary::listPatches() const {
    std::vector<PatchEntry> patches;
    collectPatches(root_, patches);
    for (const File& folder : search_folders_)
      collectPatches(folder, patches);

    std::sort(patches.begin(), patches.end(), [](const PatchEntry& a, const PatchEntry& b) {
      if (a.bank != b.bank)
        return a.bank < b.bank;
      if (a.folders != b.folders)
        return a.folders < b.folders;
      return a.name < b.name;
    });
    return patches;
  }

} // namespace sloppy
