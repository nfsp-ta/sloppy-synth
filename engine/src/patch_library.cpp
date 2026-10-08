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

    // macOS adds "__MACOSX/._name" copies of every file to zips it makes.
    bool isMacMetadata(const String& path) {
      return path.startsWith("__MACOSX/") || path.fromLastOccurrenceOf("/", false, false).startsWith("._");
    }

    bool writeEntry(ZipFile& zip, int index, const File& target) {
      std::unique_ptr<InputStream> input(zip.createStreamForEntry(index));
      if (input == nullptr || !target.getParentDirectory().createDirectory())
        return false;
      target.deleteFile();
      FileOutputStream output(target);
      if (!output.openedOk())
        return false;
      output.writeFromInputStream(*input, -1);
      output.flush();
      return output.getStatus().wasOk();
    }

    std::string plural(int count, const char* word) {
      return std::to_string(count) + " " + word + (count == 1 ? "" : "s");
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

  bool PatchLibrary::importZip(const File& zip_file, const String& zip_name, std::string& summary,
                              std::string& error) {
    FileInputStream input_stream(zip_file);
    if (!input_stream.openedOk()) {
      error = "Couldn't open " + zip_name.toStdString();
      return false;
    }

    ZipFile zip(input_stream);
    if (zip.getNumEntries() == 0) {
      error = "Not a zip file, or it's empty.";
      return false;
    }

    std::vector<int> banks, presets;
    bool bank_layout = false;
    for (int i = 0; i < zip.getNumEntries(); ++i) {
      String path = zip.getEntry(i)->filename.replaceCharacter('\\', '/');
      if (!isSafeZipPath(path)) {
        error = "Zip contains an unsafe path: " + path.toStdString();
        return false;
      }
      if (path.endsWithChar('/') || isMacMetadata(path))
        continue;

      if (path.endsWithIgnoreCase(String(".") + vital::kBankExtension))
        banks.push_back(i);
      else if (path.endsWithIgnoreCase(String(".") + vital::kPresetExtension)) {
        presets.push_back(i);
        StringArray parts;
        parts.addTokens(path, "/", "");
        if (parts.size() >= 3 && parts[1] == String(LoadSave::kPresetFolderName))
          bank_layout = true;
      }
    }

    if (banks.empty() && presets.empty()) {
      error = "No Vital presets (.vital) or banks (.vitalbank) in " + zip_name.toStdString() + ".";
      return false;
    }
    if (!root_.createDirectory()) {
      error = "Couldn't create library folder " + root_.getFullPathName().toStdString();
      return false;
    }

    int imported_banks = 0;
    int imported_presets = 0;
    std::string last_error;

    // Banks inside the zip: unpack each under its own name and import it.
    for (int index : banks) {
      TemporaryFile temp_dir;
      File folder = temp_dir.getFile();
      String name = zip.getEntry(index)->filename.replaceCharacter('\\', '/').fromLastOccurrenceOf("/", false, false);
      File bank = folder.getChildFile(File::createLegalFileName(name));
      std::string bank_name, bank_error;
      if (writeEntry(zip, index, bank) && importBank(bank, bank_name, bank_error))
        ++imported_banks;
      else
        last_error = bank_error.empty() ? "Couldn't read " + name.toStdString() : bank_error;
      folder.deleteRecursively();
    }

    if (bank_layout) {
      // Already a bank in all but name: unpack it as Vital would.
      Result result = zip.uncompressTo(root_, true);
      if (result.failed())
        last_error = "Unzipping failed: " + result.getErrorMessage().toStdString();
      else {
        StringArray bank_folders;
        for (int index : presets)
          bank_folders.addIfNotAlreadyThere(zip.getEntry(index)->filename.replaceCharacter('\\', '/')
                                                .upToFirstOccurrenceOf("/", false, false));
        imported_banks += bank_folders.size();
      }
    }
    else if (!presets.empty()) {
      // Loose presets: a bank named after the zip, keeping their folders. A
      // single folder wrapping everything (common when zipping a folder) is
      // dropped.
      String bank_name = File::createLegalFileName(zip_name.upToLastOccurrenceOf(".", false, false));
      if (bank_name.isEmpty())
        bank_name = "Imported";
      File presets_folder = root_.getChildFile(bank_name).getChildFile(LoadSave::kPresetFolderName);

      std::vector<StringArray> paths;
      for (int index : presets) {
        StringArray parts;
        parts.addTokens(zip.getEntry(index)->filename.replaceCharacter('\\', '/'), "/", "");
        parts.removeEmptyStrings();
        paths.push_back(parts);
      }
      bool shared_top = paths.size() > 0 && paths[0].size() > 1;
      for (const StringArray& parts : paths)
        shared_top = shared_top && parts.size() > 1 && parts[0] == paths[0][0];

      for (size_t i = 0; i < presets.size(); ++i) {
        StringArray parts = paths[i];
        if (shared_top)
          parts.remove(0);
        File target = presets_folder;
        for (const String& part : parts)
          target = target.getChildFile(File::createLegalFileName(part));
        if (writeEntry(zip, presets[i], target))
          ++imported_presets;
        else
          last_error = "Couldn't write " + target.getFullPathName().toStdString();
      }
    }

    if (imported_banks == 0 && imported_presets == 0) {
      error = last_error.empty() ? "Nothing could be imported." : last_error;
      return false;
    }

    summary = "Imported ";
    if (imported_banks > 0)
      summary += plural(imported_banks, "bank");
    if (imported_banks > 0 && imported_presets > 0)
      summary += " and ";
    if (imported_presets > 0)
      summary += plural(imported_presets, "preset");
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
